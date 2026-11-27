#!/usr/bin/env python3
"""
Directed X11 + XTest UI & Gameplay Harness for WSL/Linux Azahar 3DS Emulator.

Features:
  - Per-session state & optional zero-copy isolated SDMC/config sandbox (`--isolated-sdmc`)
    so multiple parallel subagents can run Azahar concurrently under `xvfb-run -a`
    without colliding on saves, `nox.cfg`, UDP ports, or X11 cursors.
  - Hard per-invocation `SIGALRM` watchdog (`--timeout`, default 60s) so no step can hang.
  - Captures crisp cropped Top (`400x240`) + Bottom (`320x240`) PNGs directly from
    Azahar's Qt render subwindow via `XGetImage`.
"""

import argparse
import ctypes
import json
import os
import shutil
import signal
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

BUTTON_KEYSYMS = {
    "a": "a",
    "b": "s",
    "x": "z",
    "y": "x",
    "l": "q",
    "r": "w",
    "zl": "1",
    "zr": "2",
    "start": "m",
    "select": "n",
    "dup": "t",
    "ddown": "g",
    "dleft": "f",
    "dright": "h",
    "cpad_up": "Up",
    "cpad_down": "Down",
    "cpad_left": "Left",
    "cpad_right": "Right",
    "cstick_up": "i",
    "cstick_down": "k",
    "cstick_left": "j",
    "cstick_right": "l",
}


def write_cropped_png(w, h, bpp, rows, box, out_path):
    x0, y0, x1, y1 = box
    x0 = max(0, min(w, int(x0)))
    y0 = max(0, min(h, int(y0)))
    x1 = max(x0 + 1, min(w, int(x1)))
    y1 = max(y0 + 1, min(h, int(y1)))
    cw, ch = x1 - x0, y1 - y0
    out_raw = bytearray()
    for y in range(y0, y1):
        out_raw.append(0)
        row = rows[y]
        if bpp == 3:
            out_raw.extend(row[x0 * 3:x1 * 3])
        else:
            for x in range(x0, x1):
                o = x * 4
                out_raw.extend(row[o:o + 3])

    def png_chunk(ctype, payload):
        crc = zlib.crc32(ctype + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + ctype + payload + struct.pack(">I", crc)

    ihdr = struct.pack(">IIBBBBB", cw, ch, 8, 2, 0, 0, 0)
    idat = zlib.compress(bytes(out_raw), 6)
    Path(out_path).write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"IDAT", idat)
        + png_chunk(b"IEND", b"")
    )


class XKeyEvent(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
        ("display", ctypes.c_void_p), ("window", ctypes.c_ulong), ("root", ctypes.c_ulong),
        ("subwindow", ctypes.c_ulong), ("time", ctypes.c_ulong), ("x", ctypes.c_int),
        ("y", ctypes.c_int), ("x_root", ctypes.c_int), ("y_root", ctypes.c_int),
        ("state", ctypes.c_uint), ("keycode", ctypes.c_uint), ("same_screen", ctypes.c_int),
    ]


class XFocusChangeEvent(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
        ("display", ctypes.c_void_p), ("window", ctypes.c_ulong), ("mode", ctypes.c_int),
        ("detail", ctypes.c_int),
    ]


class XSynthEvent(ctypes.Union):
    _fields_ = [("xkey", XKeyEvent), ("xfocus", XFocusChangeEvent), ("pad", ctypes.c_long * 24)]


class XWindowAttributes(ctypes.Structure):
    _fields_ = [
        ("x", ctypes.c_int), ("y", ctypes.c_int), ("width", ctypes.c_int), ("height", ctypes.c_int),
        ("border_width", ctypes.c_int), ("depth", ctypes.c_int), ("visual", ctypes.c_void_p),
        ("root", ctypes.c_ulong), ("c_class", ctypes.c_int), ("bit_gravity", ctypes.c_int),
        ("win_gravity", ctypes.c_int), ("backing_store", ctypes.c_int),
        ("backing_planes", ctypes.c_ulong), ("backing_pixel", ctypes.c_ulong),
        ("save_under", ctypes.c_int), ("colormap", ctypes.c_ulong), ("map_installed", ctypes.c_int),
        ("map_state", ctypes.c_int), ("all_event_masks", ctypes.c_long),
        ("your_event_mask", ctypes.c_long), ("do_not_propagate_mask", ctypes.c_long),
        ("override_redirect", ctypes.c_int), ("screen", ctypes.c_void_p),
    ]


class XImage(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_int), ("height", ctypes.c_int), ("xoffset", ctypes.c_int),
        ("format", ctypes.c_int), ("data", ctypes.POINTER(ctypes.c_ubyte)),
        ("byte_order", ctypes.c_int), ("bitmap_unit", ctypes.c_int),
        ("bitmap_bit_order", ctypes.c_int), ("bitmap_pad", ctypes.c_int),
        ("depth", ctypes.c_int), ("bytes_per_line", ctypes.c_int), ("bits_per_pixel", ctypes.c_int),
    ]


class X11Driver:
    def __init__(self, display_name=None):
        self.x11 = ctypes.CDLL("libX11.so.6")
        self.xtst = ctypes.CDLL("libXtst.so.6")

        self.x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x11.XOpenDisplay.restype = ctypes.c_void_p
        self.x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        self.x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        self.x11.XDefaultRootWindow.restype = ctypes.c_ulong
        self.x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.x11.XFetchName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_char_p)]
        self.x11.XFetchName.restype = ctypes.c_int
        self.x11.XFree.argtypes = [ctypes.c_void_p]
        self.x11.XQueryTree.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
            ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
            ctypes.POINTER(ctypes.c_uint),
        ]
        self.x11.XQueryTree.restype = ctypes.c_int
        self.x11.XGetWindowAttributes.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(XWindowAttributes)]
        self.x11.XGetWindowAttributes.restype = ctypes.c_int
        self.x11.XTranslateCoordinates.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
            ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_ulong),
        ]
        self.x11.XTranslateCoordinates.restype = ctypes.c_int
        self.x11.XRaiseWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.x11.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
        self.x11.XMoveResizeWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint]
        self.x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
        self.x11.XStringToKeysym.restype = ctypes.c_ulong
        self.x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.x11.XKeysymToKeycode.restype = ctypes.c_ubyte
        self.x11.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.POINTER(XSynthEvent)]
        self.x11.XQueryPointer.argtypes = [
            ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
            ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
            ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_uint),
        ]
        self.xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        self.xtst.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
        self.xtst.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]

        CMPFUNC = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)
        self._err_cb = CMPFUNC(lambda d, e: 0)
        self.x11.XSetErrorHandler(self._err_cb)

        dname = (display_name or os.environ.get("DISPLAY") or ":0").encode()
        self.dpy = self.x11.XOpenDisplay(dname)
        if not self.dpy:
            raise RuntimeError(f"Failed to open X11 display {dname.decode()}")
        self.root = self.x11.XDefaultRootWindow(self.dpy)

    def close(self):
        if getattr(self, "dpy", None):
            self.x11.XCloseDisplay(self.dpy)
            self.dpy = None

    def get_window_name(self, wid):
        name_p = ctypes.c_char_p()
        if self.x11.XFetchName(self.dpy, wid, ctypes.byref(name_p)) and name_p.value:
            val = name_p.value.decode("utf-8", errors="replace")
            self.x11.XFree(name_p)
            return val
        return ""

    def walk_windows(self, wid=None, depth=0):
        if wid is None:
            wid = self.root
        if depth > 8:
            return []
        results = []
        attr = XWindowAttributes()
        if self.x11.XGetWindowAttributes(self.dpy, wid, ctypes.byref(attr)):
            rx, ry, child_ret = ctypes.c_int(), ctypes.c_int(), ctypes.c_ulong()
            self.x11.XTranslateCoordinates(self.dpy, wid, self.root, 0, 0, ctypes.byref(rx), ctypes.byref(ry), ctypes.byref(child_ret))
            results.append({
                "wid": int(wid), "name": self.get_window_name(wid),
                "x": int(rx.value), "y": int(ry.value),
                "w": int(attr.width), "h": int(attr.height),
                "map_state": int(attr.map_state), "depth": depth,
            })
        root_ret, parent_ret = ctypes.c_ulong(), ctypes.c_ulong()
        children_p = ctypes.POINTER(ctypes.c_ulong)()
        nchildren = ctypes.c_uint()
        if self.x11.XQueryTree(self.dpy, wid, ctypes.byref(root_ret), ctypes.byref(parent_ret), ctypes.byref(children_p), ctypes.byref(nchildren)):
            for i in range(nchildren.value):
                results.extend(self.walk_windows(children_p[i], depth + 1))
            if children_p:
                self.x11.XFree(children_p)
        return results

    def find_azahar_window(self):
        wins = self.walk_windows()
        candidates = [
            w for w in wins
            if w["map_state"] == 2 and ("azahar" in w["name"].lower() or "nox" in w["name"].lower()) and w["w"] >= 320 and w["h"] >= 240
        ]
        if not candidates:
            for w in wins:
                if w["map_state"] == 2 and w["w"] >= 400 and w["h"] >= 480 and w["depth"] <= 3:
                    try:
                        out = subprocess.check_output(["xprop", "-id", hex(w["wid"]), "_NET_WM_NAME", "WM_CLASS"], stderr=subprocess.DEVNULL, text=True, timeout=1.0).lower()
                        if "azahar" in out or "nox" in out:
                            w["name"] = out.strip()
                            candidates.append(w)
                    except Exception:
                        pass
        if not candidates:
            return None
        candidates.sort(key=lambda x: (x["w"] * x["h"], -x["depth"]), reverse=True)
        main_win = candidates[0]
        main_win["subwindows"] = [
            w for w in self.walk_windows(main_win["wid"], 0)
            if w["map_state"] == 2 and w["wid"] != main_win["wid"] and w["w"] >= 320 and w["h"] >= 240
        ]
        return main_win

    def focus_window(self, wid):
        self.x11.XRaiseWindow(self.dpy, wid)
        self.x11.XSetInputFocus(self.dpy, wid, 2, 0)
        self.x11.XSync(self.dpy, 0)
        time.sleep(0.02)

    def move_resize(self, wid, x, y, w, h):
        self.x11.XMoveResizeWindow(self.dpy, wid, x, y, w, h)
        self.x11.XSync(self.dpy, 0)
        time.sleep(0.15)

    def get_pointer(self):
        r, c = ctypes.c_ulong(), ctypes.c_ulong()
        rx, ry, wx, wy = ctypes.c_int(), ctypes.c_int(), ctypes.c_int(), ctypes.c_int()
        mask = ctypes.c_uint()
        self.x11.XQueryPointer(self.dpy, self.root, ctypes.byref(r), ctypes.byref(c), ctypes.byref(rx), ctypes.byref(ry), ctypes.byref(wx), ctypes.byref(wy), ctypes.byref(mask))
        return int(rx.value), int(ry.value)

    def keysym_to_keycode(self, keysym_name):
        sym = self.x11.XStringToKeysym(keysym_name.encode())
        if not sym:
            raise ValueError(f"Unknown X11 keysym: {keysym_name}")
        kc = self.x11.XKeysymToKeycode(self.dpy, sym)
        if not kc:
            raise ValueError(f"No keycode mapped for keysym: {keysym_name}")
        return int(kc)

    def ensure_qt_focus(self, win_info):
        for w in [win_info["wid"]] + [s["wid"] for s in win_info.get("subwindows", [])]:
            fev = XSynthEvent()
            fev.xfocus.type = 9
            fev.xfocus.window = w
            fev.xfocus.detail = 3
            self.x11.XSendEvent(self.dpy, w, 0, 1 << 21, ctypes.byref(fev))
        self.x11.XSync(self.dpy, 0)

    def _send_keycode(self, win_info, kc, down):
        ev_type = 2 if down else 3
        ev_mask = 1 if down else 2
        ts = int(time.time() * 1000) & 0xFFFFFFFF
        for w in [win_info["wid"]] + [s["wid"] for s in win_info.get("subwindows", [])]:
            kev = XSynthEvent()
            kev.xkey.type = ev_type
            kev.xkey.display = self.dpy
            kev.xkey.window = w
            kev.xkey.root = self.root
            kev.xkey.time = ts
            kev.xkey.keycode = kc
            kev.xkey.same_screen = 1
            self.x11.XSendEvent(self.dpy, w, 0, ev_mask, ctypes.byref(kev))
        self.x11.XSync(self.dpy, 0)

    def press_key(self, keysym_name, hold_ms=90, win_info=None):
        kc = self.keysym_to_keycode(keysym_name)
        if win_info:
            self.ensure_qt_focus(win_info)
            self._send_keycode(win_info, kc, True)
            time.sleep(max(0.03, hold_ms / 1000.0))
            self._send_keycode(win_info, kc, False)
        else:
            self.xtst.XTestFakeKeyEvent(self.dpy, kc, 1, 0)
            self.x11.XSync(self.dpy, 0)
            time.sleep(max(0.03, hold_ms / 1000.0))
            self.xtst.XTestFakeKeyEvent(self.dpy, kc, 0, 0)
            self.x11.XSync(self.dpy, 0)

    def hold_keys(self, keysym_names, hold_ms=120, win_info=None):
        kcs = [self.keysym_to_keycode(k) for k in keysym_names]
        if win_info:
            self.ensure_qt_focus(win_info)
            for kc in kcs:
                self._send_keycode(win_info, kc, True)
            time.sleep(max(0.03, hold_ms / 1000.0))
            for kc in reversed(kcs):
                self._send_keycode(win_info, kc, False)
        else:
            for kc in kcs:
                self.xtst.XTestFakeKeyEvent(self.dpy, kc, 1, 0)
            self.x11.XSync(self.dpy, 0)
            time.sleep(max(0.03, hold_ms / 1000.0))
            for kc in reversed(kcs):
                self.xtst.XTestFakeKeyEvent(self.dpy, kc, 0, 0)
            self.x11.XSync(self.dpy, 0)

    def click_root(self, rx, ry, hold_ms=90, restore_pointer=True):
        orig_x, orig_y = self.get_pointer()
        self.xtst.XTestFakeMotionEvent(self.dpy, -1, int(rx), int(ry), 0)
        self.x11.XSync(self.dpy, 0)
        time.sleep(0.03)
        self.xtst.XTestFakeButtonEvent(self.dpy, 1, 1, 0)
        self.x11.XSync(self.dpy, 0)
        time.sleep(max(0.04, hold_ms / 1000.0))
        self.xtst.XTestFakeButtonEvent(self.dpy, 1, 0, 0)
        self.x11.XSync(self.dpy, 0)
        time.sleep(0.03)
        if restore_pointer:
            self.xtst.XTestFakeMotionEvent(self.dpy, -1, orig_x, orig_y, 0)
            self.x11.XSync(self.dpy, 0)

    def drag_root(self, rx1, ry1, rx2, ry2, duration_ms=350, steps=14, restore_pointer=True):
        orig_x, orig_y = self.get_pointer()
        self.xtst.XTestFakeMotionEvent(self.dpy, -1, int(rx1), int(ry1), 0)
        self.x11.XSync(self.dpy, 0)
        time.sleep(0.03)
        self.xtst.XTestFakeButtonEvent(self.dpy, 1, 1, 0)
        self.x11.XSync(self.dpy, 0)
        step_dt = max(0.015, (duration_ms / 1000.0) / max(1, steps))
        for i in range(1, steps + 1):
            t = i / float(steps)
            self.xtst.XTestFakeMotionEvent(self.dpy, -1, int(round(rx1 + (rx2 - rx1) * t)), int(round(ry1 + (ry2 - ry1) * t)), 0)
            self.x11.XSync(self.dpy, 0)
            time.sleep(step_dt)
        self.xtst.XTestFakeButtonEvent(self.dpy, 1, 0, 0)
        self.x11.XSync(self.dpy, 0)
        time.sleep(0.03)
        if restore_pointer:
            self.xtst.XTestFakeMotionEvent(self.dpy, -1, orig_x, orig_y, 0)
            self.x11.XSync(self.dpy, 0)


def calibrate_viewports_from_win(win_info):
    wx, wy, ww, wh = win_info["x"], win_info["y"], win_info["w"], win_info["h"]
    subs = sorted(win_info.get("subwindows", []), key=lambda s: s["w"] * s["h"], reverse=True)
    rx, ry, rw, rh = (subs[0]["x"], subs[0]["y"], subs[0]["w"], subs[0]["h"]) if (subs and subs[0]["w"] >= 320 and subs[0]["h"] >= 240) else (wx, wy + 22, ww, max(240, wh - 46))
    scale = min(rw / 400.0, rh / 480.0)
    top_w, top_h = 400.0 * scale, 240.0 * scale
    bot_w, bot_h = 320.0 * scale, 240.0 * scale
    top_x = rx + (rw - top_w) / 2.0
    top_y = ry + (rh - (top_h + bot_h)) / 2.0
    bot_x = rx + (rw - bot_w) / 2.0
    bot_y = top_y + top_h
    return {
        "render_rect": [int(round(rx)), int(round(ry)), int(round(rw)), int(round(rh))],
        "scale": scale,
        "top_rect": [top_x, top_y, top_w, top_h],
        "bot_rect": [bot_x, bot_y, bot_w, bot_h],
    }


def capture_window_png(xdrv, win_info, out_png, crop_to_render=True):
    out_p = Path(out_png)
    out_p.parent.mkdir(parents=True, exist_ok=True)
    cal = calibrate_viewports_from_win(win_info)
    xdrv.x11.XGetImage.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_int]
    xdrv.x11.XGetImage.restype = ctypes.POINTER(XImage)
    subs = sorted(win_info.get("subwindows", []), key=lambda s: s["w"] * s["h"], reverse=True)
    target = subs[0] if subs else win_info
    img_p = xdrv.x11.XGetImage(xdrv.dpy, target["wid"], 0, 0, target["w"], target["h"], 0xFFFFFFFF, 2)
    if not img_p:
        raise RuntimeError("XGetImage failed to capture Azahar render subwindow")
    im = img_p.contents
    buf = bytes(ctypes.cast(im.data, ctypes.POINTER(ctypes.c_ubyte * (im.bytes_per_line * im.height))).contents)
    # Free the XImage (data + struct) now that pixels are copied; without this every capture leaked
    # ~w*h*4 bytes (long-running recorders grew to multiple GB and got OOM-killed).
    im_w, im_h, im_bpl = im.width, im.height, im.bytes_per_line
    xdrv.x11.XFree(ctypes.cast(im.data, ctypes.c_void_p))
    xdrv.x11.XFree(ctypes.cast(img_p, ctypes.c_void_p))

    class _Dims:  # keep the rest of the function unchanged
        width, height, bytes_per_line = im_w, im_h, im_bpl
    im = _Dims
    rows = []
    for y in range(im.height):
        line = bytearray(im.width * 3)
        base = y * im.bytes_per_line
        for x in range(im.width):
            o = base + x * 4
            line[x * 3], line[x * 3 + 1], line[x * 3 + 2] = buf[o + 2], buf[o + 1], buf[o]
        rows.append(bytes(line))
    if crop_to_render:
        tx = int(round(cal["top_rect"][0] - target["x"]))
        ty = int(round(cal["top_rect"][1] - target["y"]))
        tw = int(round(cal["top_rect"][2]))
        th = int(round(cal["top_rect"][3] + cal["bot_rect"][3]))
        box = (max(0, tx), max(0, ty), min(im.width, tx + tw), min(im.height, ty + th))
    else:
        box = (0, 0, im.width, im.height)
    write_cropped_png(im.width, im.height, 3, rows, box, str(out_p))
    return cal


def session_paths(session_id: str) -> tuple[Path, Path, Path]:
    safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in session_id)
    return (
        Path(f"/tmp/azahar_state_{safe}.json"),
        Path(f"/tmp/azahar_log_{safe}.log"),
        Path(f"/tmp/azahar_sandbox_{safe}"),
    )


def prepare_isolated_sandbox(sandbox_dir: Path) -> dict[str, str]:
    """Create a zero-copy symlinked SDMC + isolated config dir so parallel Azahar runs never collide."""
    if sandbox_dir.exists():
        shutil.rmtree(sandbox_dir, ignore_errors=True)
    cfg_dir = sandbox_dir / "config" / "azahar-emu"
    data_dir = sandbox_dir / "data" / "azahar-emu"
    sdmc_nox = data_dir / "sdmc" / "3ds" / "nox"
    cfg_dir.mkdir(parents=True, exist_ok=True)
    sdmc_nox.mkdir(parents=True, exist_ok=True)

    base_data = Path.home() / ".local" / "share" / "azahar-emu"
    if (base_data / "nand").exists():
        (data_dir / "nand").symlink_to(base_data / "nand")
    if (base_data / "sdmc" / "3ds" / "dspfirm.cdc").exists():
        (data_dir / "sdmc" / "3ds" / "dspfirm.cdc").symlink_to(base_data / "sdmc" / "3ds" / "dspfirm.cdc")

    src_nox = base_data / "sdmc" / "3ds" / "nox"
    if src_nox.is_dir():
        for entry in src_nox.iterdir():
            if entry.name.lower() in ("save", "nox.cfg", "net.log"):
                if entry.is_file():
                    shutil.copy2(entry, sdmc_nox / entry.name)
                continue
            (sdmc_nox / entry.name).symlink_to(entry)
        (sdmc_nox / "save").mkdir(exist_ok=True)

    src_ini = Path.home() / ".config" / "azahar-emu" / "qt-config.ini"
    if src_ini.is_file():
        lines = []
        for ln in src_ini.read_text(errors="replace").splitlines():
            if ln.startswith("sdmc_directory="):
                lines.append(f"sdmc_directory={data_dir / 'sdmc'}/")
            elif ln.startswith("nand_directory="):
                lines.append(f"nand_directory={data_dir / 'nand'}/")
            elif ln.startswith(r"profiles\1\udp_input_port="):
                lines.append(f"profiles\\1\\udp_input_port={28000 + (os.getpid() % 10000)}")
            else:
                lines.append(ln)
        (cfg_dir / "qt-config.ini").write_text("\n".join(lines) + "\n")

    env = os.environ.copy()
    env["XDG_CONFIG_HOME"] = str(sandbox_dir / "config")
    env["XDG_DATA_HOME"] = str(sandbox_dir / "data")
    return env


def is_pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def kill_pgrp(pid: int):
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(os.getpgid(pid), sig)
        except OSError:
            try:
                os.kill(pid, sig)
            except OSError:
                pass
        for _ in range(10):
            if not is_pid_alive(pid):
                return
            time.sleep(0.1)


def map_bot_coord(cal, bx, by, logical_640=False):
    if logical_640:
        bx = bx * 320.0 / 640.0
        by = by * 240.0 / 480.0
    bot_x, bot_y, bot_w, bot_h = cal["bot_rect"]
    return int(round(bot_x + (float(bx) / 320.0) * bot_w)), int(round(bot_y + (float(by) / 240.0) * bot_h))


def cmd_start(args):
    state_file, log_file, sandbox_dir = session_paths(args.session)
    if state_file.exists():
        cmd_stop(args)

    rom_path = os.path.abspath(args.rom)
    if not os.path.isfile(rom_path):
        raise FileNotFoundError(f"ROM not found: {rom_path}")

    env = prepare_isolated_sandbox(sandbox_dir) if args.isolated_sdmc else os.environ.copy()
    azahar_bin = env.get("AZAHAR_BIN") or shutil.which("azahar")
    if not azahar_bin:
        raise RuntimeError("'azahar' binary not found in PATH")

    log_f = open(log_file, "w")
    proc = subprocess.Popen([azahar_bin, rom_path], stdout=log_f, stderr=subprocess.STDOUT, env=env, start_new_session=True)

    xdrv = X11Driver()
    try:
        win = None
        deadline = time.time() + args.wait_boot
        while time.time() < deadline:
            if proc.poll() is not None:
                raise RuntimeError(f"Azahar exited early ({proc.returncode}); see {log_file}")
            win = xdrv.find_azahar_window()
            time.sleep(0.5)
        win = xdrv.find_azahar_window()
        if not win:
            kill_pgrp(proc.pid)
            raise RuntimeError(f"Azahar window did not appear within {args.wait_boot}s")
        if args.resize:
            xdrv.move_resize(win["wid"], 80, 40, 900, 1020)
            win = xdrv.find_azahar_window() or win
        xdrv.focus_window(win["wid"])
        cal_png = args.out or f"/tmp/azahar_boot_{args.session}.png"
        cal = capture_window_png(xdrv, win, cal_png)
        st = {
            "pid": proc.pid,
            "rom": rom_path,
            "wid": win["wid"],
            "win": win,
            "calibration": cal,
            "isolated_sdmc": bool(args.isolated_sdmc),
        }
        state_file.write_text(json.dumps(st, indent=2))
        print(json.dumps({"pid": proc.pid, "out": cal_png, "scale": cal["scale"], "bot_rect": cal["bot_rect"]}))
    finally:
        xdrv.close()


def cmd_stop(args):
    state_file, _, sandbox_dir = session_paths(args.session)
    if state_file.exists():
        try:
            st = json.loads(state_file.read_text())
            if st.get("pid"):
                kill_pgrp(int(st["pid"]))
        except Exception:
            pass
        state_file.unlink(missing_ok=True)
    if sandbox_dir.exists():
        shutil.rmtree(sandbox_dir, ignore_errors=True)
    print(f"[azahar-play:{args.session}] Stopped cleanly.")


def cmd_batch(args):
    state_file, _, _ = session_paths(args.session)
    if not state_file.exists():
        raise RuntimeError(f"Session '{args.session}' not running; call 'start' first.")
    st = json.loads(state_file.read_text())
    steps = json.loads(args.steps_json)
    xdrv = X11Driver()
    try:
        win = xdrv.find_azahar_window()
        if not win:
            raise RuntimeError("Azahar X11 window not found")
        st["win"] = win
        xdrv.focus_window(win["wid"])
        cal = st["calibration"]
        for idx, step in enumerate(steps):
            op = step["op"]
            if op == "sleep":
                time.sleep(min(15.0, float(step["sec"])))
            elif op == "touch":
                rx, ry = map_bot_coord(cal, step["x"], step["y"], logical_640=step.get("logical", False))
                xdrv.click_root(rx, ry, hold_ms=int(step.get("hold", 90)))
                time.sleep(min(10.0, float(step.get("wait", 0.35))))
            elif op == "drag":
                rx1, ry1 = map_bot_coord(cal, step["x1"], step["y1"], logical_640=step.get("logical", False))
                rx2, ry2 = map_bot_coord(cal, step["x2"], step["y2"], logical_640=step.get("logical", False))
                xdrv.drag_root(rx1, ry1, rx2, ry2, duration_ms=int(step.get("duration", 400)), steps=int(step.get("steps", 14)))
                time.sleep(min(10.0, float(step.get("wait", 0.35))))
            elif op == "key":
                names = [k.strip() for k in step["button"].split("+") if k.strip()]
                syms = [BUTTON_KEYSYMS.get(n.lower(), n) for n in names]
                rep = int(step.get("repeat", 1))
                hold = int(step.get("hold", 100))
                # Guard: values >= 20 are treated as milliseconds so 'interval: 800' never sleeps 800s!
                raw_iv = float(step.get("interval_ms", step.get("interval", 120)))
                iv_sec = (raw_iv / 1000.0) if raw_iv >= 10.0 else raw_iv
                for _ in range(rep):
                    if len(syms) == 1:
                        xdrv.press_key(syms[0], hold_ms=hold, win_info=st["win"])
                    else:
                        xdrv.hold_keys(syms, hold_ms=hold, win_info=st["win"])
                    if rep > 1:
                        time.sleep(min(5.0, iv_sec))
                time.sleep(min(10.0, float(step.get("wait", 0.35))))
            elif op == "shot":
                st["win"] = xdrv.find_azahar_window() or st["win"]
                cal = capture_window_png(xdrv, st["win"], step["out"], crop_to_render=step.get("crop_render", True))
                st["calibration"] = cal
                state_file.write_text(json.dumps(st, indent=2))
                print(f"[batch:{idx}] Saved {step['out']}")
            else:
                raise ValueError(f"Unknown batch op: {op}")
    finally:
        xdrv.close()


def main() -> int:
    ap = argparse.ArgumentParser(description="Directed Azahar 3DS UI/Play Test Driver")
    ap.add_argument("--session", default=os.environ.get("AZAHAR_SESSION", "default"), help="Isolated session name")
    ap.add_argument("--timeout", type=int, default=60, help="Hard SIGALRM watchdog timeout in seconds")
    sub = ap.add_subparsers(dest="cmd", required=True)

    ps = sub.add_parser("start")
    ps.add_argument("rom")
    ps.add_argument("--wait-boot", type=float, default=9.0)
    ps.add_argument("--out", default="")
    ps.add_argument("--resize", action="store_true")
    ps.add_argument("--isolated-sdmc", action="store_true", help="Run inside symlinked zero-copy SDMC/config sandbox")
    ps.set_defaults(func=cmd_start)

    pstop = sub.add_parser("stop")
    pstop.set_defaults(func=cmd_stop)

    pb = sub.add_parser("batch")
    pb.add_argument("steps_json")
    pb.set_defaults(func=cmd_batch)

    ns = ap.parse_args()
    if hasattr(signal, "SIGALRM") and ns.timeout > 0:
        def _on_timeout(_sig, _frm):
            print(f"[azahar-play:{ns.session}] ERROR: Hard timeout ({ns.timeout}s) exceeded!", file=sys.stderr)
            sys.exit(124)
        signal.signal(signal.SIGALRM, _on_timeout)
        signal.alarm(ns.timeout)

    ns.func(ns)
    return 0


if __name__ == "__main__":
    sys.exit(main())
