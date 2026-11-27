#!/usr/bin/env python3
"""Host-side client for the Nox 3DS playtest hook (issue #36).

Stdlib only. Wire layout must match port/3ds/playtest_hook_3ds.h exactly.

  # print decoded state packets as JSON lines
  python3 scripts/playtest_hook_client.py listen --port 47800

  # send commands to the 3DS (cmd port = listen_port in playtest.cfg)
  python3 scripts/playtest_hook_client.py send --to 127.0.0.1:47801 move --dx 1 --dy 0 --run
  python3 scripts/playtest_hook_client.py send --to 127.0.0.1:47801 aim --x 1200 --y 800
  python3 scripts/playtest_hook_client.py send --to 127.0.0.1:47801 slot 1
  python3 scripts/playtest_hook_client.py send --to 127.0.0.1:47801 attack

  python3 -I scripts/playtest_hook_client.py --selftest
"""
import argparse
import json
import math
import socket
import struct
import sys
import time

MAGIC_STATE = 0x5450584E  # b"NXPT"
MAGIC_CMD = 0x4350584E    # b"NXPC"
VERSION = 1
MAX_OBJECTS = 32
NUM_ABILITIES = 5

# Struct formats ('<' = little-endian, no padding: matches __attribute__((packed))).
ABILITY_FMT = "<BBBBHH"
HDR_FMT_PREFIX = "<IHHIIIHBBIHHHHiiBBHii"
HDR_FMT = HDR_FMT_PREFIX + ABILITY_FMT[1:] * NUM_ABILITIES
OBJ_FMT = "<HHIiiBBH"
CMD_FMT = "<IBBHIii"

# Must equal the C _Static_asserts in playtest_hook_3ds.h.
ABILITY_SIZE = 8
HDR_SIZE = 96
OBJ_SIZE = 20
CMD_SIZE = 20

assert struct.calcsize(ABILITY_FMT) == ABILITY_SIZE
assert struct.calcsize(HDR_FMT) == HDR_SIZE
assert struct.calcsize(OBJ_FMT) == OBJ_SIZE
assert struct.calcsize(CMD_FMT) == CMD_SIZE

FLAGS = {
    0x0001: "menu_shell",
    0x0002: "map_loaded",
    0x0004: "in_world",
    0x0008: "is_mp",
    0x0010: "is_host",
    0x0020: "player_valid",
    0x0040: "observer",
    0x0080: "hp_zero",
    0x0100: "bots_ok",
    0x0200: "cmds_active",
    0x0400: "obj_truncated",
}

OPS = {
    "nop": 0, "move": 1, "aim": 2, "aimdir": 3, "attack": 4,
    "slot": 5, "heal": 6, "mana": 7, "jump": 8, "stop": 9,
}

# playerInfo+2251; mapping is the OpenNox convention (UNVERIFIED).
CLASSES = {0: "warrior", 1: "wizard", 2: "conjurer"}
ABILITY_NAMES = {1: "berserker_charge", 2: "war_cry", 3: "harpoon",
                 4: "tread_lightly", 5: "eye_of_the_wolf"}


def orient_from_vec(dx, dy):
    """Same convention as src/gamepad.c update_left_analog_absolute()."""
    theta = math.atan2(-dy, -dx) / math.pi
    return int((theta + 1.0) * 128.0 + 0.5) & 255


def vec_from_orient(o):
    ang = ((o / 128.0) - 1.0) * math.pi
    return -math.cos(ang), -math.sin(ang)


def derive_mode(flags):
    if flags & 0x0001 or not flags & 0x0002:
        return "menu"
    if flags & 0x0040:
        return "observer"
    if flags & 0x0080:
        return "dead?"
    if flags & 0x0004:
        return "in_world"
    return "in_world_modal"


def decode_state(data):
    if len(data) < HDR_SIZE:
        raise ValueError("short packet: %d" % len(data))
    f = struct.unpack_from(HDR_FMT, data, 0)
    (magic, version, header_size, seq, game_frame, hook_tick, flags, pclass,
     n_obj, net_id, hp, hp_max, mp, mp_max, x, y, facing, anim, obj_size,
     cam_x, cam_y) = f[:21]
    if magic != MAGIC_STATE:
        raise ValueError("bad magic 0x%08X" % magic)
    if version != VERSION or header_size != HDR_SIZE or obj_size != OBJ_SIZE:
        raise ValueError("version/size mismatch v=%d hdr=%d obj=%d" % (version, header_size, obj_size))
    if len(data) != HDR_SIZE + n_obj * OBJ_SIZE:
        raise ValueError("length %d != %d" % (len(data), HDR_SIZE + n_obj * OBJ_SIZE))
    abilities = []
    ab = f[21:]
    for i in range(NUM_ABILITIES):
        aid, ready, active, _res, cd_rem, cd_total = ab[i * 6:(i + 1) * 6]
        abilities.append({
            "slot": i + 1, "id": aid, "name": ABILITY_NAMES.get(aid),
            "ready": bool(ready), "active": bool(active),
            "cd_remaining": cd_rem, "cd_total": cd_total,
        })
    objects = []
    for i in range(n_obj):
        o_net, o_type, o_cls, o_x, o_y, o_face, o_hp, _res = struct.unpack_from(
            OBJ_FMT, data, HDR_SIZE + i * OBJ_SIZE)
        objects.append({
            "net_id": o_net, "type_id": o_type, "class_flags": o_cls,
            "is_player": bool(o_cls & 4), "is_monster": bool(o_cls & 2),
            "x": o_x, "y": o_y, "facing": o_face,
            "hp_pct": None if o_hp == 0xFF else o_hp,
        })
    return {
        "seq": seq, "game_frame": game_frame, "hook_tick": hook_tick,
        "flags": flags, "flag_names": [n for b, n in FLAGS.items() if flags & b],
        "mode": derive_mode(flags),
        "player": {
            "net_id": net_id,
            "class_id": None if pclass == 0xFF else pclass,
            "class": CLASSES.get(pclass),
            "hp": hp, "hp_max": hp_max, "mp": mp, "mp_max": mp_max,
            "x": x, "y": y, "facing": facing, "anim": anim,
            "abilities": abilities,
        },
        "camera": {"x": cam_x, "y": cam_y},
        "objects": objects,
    }


def encode_state(seq=1, game_frame=0, hook_tick=0, flags=0, pclass=0xFF, net_id=0,
                 hp=0, hp_max=0, mp=0, mp_max=0, x=0, y=0, facing=0, anim=0,
                 cam_x=0, cam_y=0, abilities=(), objects=()):
    """Encoder mirroring publish_state() (used by --selftest and fakes)."""
    ab = list(abilities) + [(0, 0, 0, 0, 0)] * (NUM_ABILITIES - len(abilities))
    ab_flat = []
    for aid, ready, active, cd_rem, cd_total in ab[:NUM_ABILITIES]:
        ab_flat += [aid, ready, active, 0, cd_rem, cd_total]
    objs = list(objects)[:MAX_OBJECTS]
    hdr = struct.pack(HDR_FMT, MAGIC_STATE, VERSION, HDR_SIZE, seq, game_frame, hook_tick,
                      flags, pclass, len(objs), net_id, hp, hp_max, mp, mp_max, x, y,
                      facing, anim, OBJ_SIZE, cam_x, cam_y, *ab_flat)
    body = b"".join(struct.pack(OBJ_FMT, o[0], o[1], o[2], o[3], o[4], o[5], o[6], 0)
                    for o in objs)
    return hdr + body


def encode_cmd(op, a=0, b=0, ttl_ms=0, seq=0):
    if isinstance(op, str):
        op = OPS[op]
    return struct.pack(CMD_FMT, MAGIC_CMD, VERSION, op, ttl_ms, seq, a, b)


def decode_cmd(data):
    if len(data) != CMD_SIZE:
        raise ValueError("cmd size %d" % len(data))
    magic, version, op, ttl_ms, seq, a, b = struct.unpack(CMD_FMT, data)
    if magic != MAGIC_CMD or version != VERSION:
        raise ValueError("bad cmd header")
    return {"op": op, "ttl_ms": ttl_ms, "seq": seq, "a": a, "b": b}


class HookClient:
    """Minimal programmatic API: recv_state() and send(op, a, b)."""

    def __init__(self, listen_port=47800, target=None, bind="0.0.0.0"):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((bind, listen_port))
        self.target = target  # (ip, port); learned from first state packet if None
        self.cmd_port = target[1] if target else 47801
        self.seq = 0

    def recv_state(self, timeout=None):
        self.sock.settimeout(timeout)
        data, addr = self.sock.recvfrom(2048)
        if self.target is None:
            self.target = (addr[0], self.cmd_port)
        return decode_state(data)

    def send(self, op, a=0, b=0, ttl_ms=0):
        if self.target is None:
            raise RuntimeError("no target yet; pass --to or receive a state packet first")
        self.seq += 1
        self.sock.sendto(encode_cmd(op, a, b, ttl_ms, self.seq), self.target)


def parse_hostport(s):
    host, _, port = s.rpartition(":")
    return host, int(port)


def build_cmd(args):
    op = args.op
    v = args.args
    a = b = 0
    if op == "move":
        if args.orient is not None:
            a = args.orient
        else:
            a = orient_from_vec(args.dx, args.dy)
        b = 0 if args.stop else (3 if args.run else 1)
    elif op == "aim":
        a, b = args.x, args.y
    elif op == "aimdir":
        a = args.orient if args.orient is not None else orient_from_vec(args.dx, args.dy)
    elif op == "attack":
        a = int(v[0]) if v else 0
    elif op == "slot":
        if not v:
            raise SystemExit("slot needs a number 1..5")
        a = int(v[0])
        b = 1 if args.self_cast else 0
    return encode_cmd(op, a, b, args.ttl, 0)


def selftest():
    abilities = [(1, 1, 0, 0, 300), (2, 0, 1, 120, 300), (3, 0, 0, 75, 150),
                 (4, 1, 0, 0, 30), (5, 0, 0, 600, 600)]
    objects = [(17, 1234, 4, 100, -20, 3, 0xFF), (900, 77, 0x2000000, 5000, 6000, 0, 50)]
    pkt = encode_state(seq=7, game_frame=123456, hook_tick=99, flags=0x0226, pclass=0,
                       net_id=42, hp=150, hp_max=150, mp=0, mp_max=0, x=2048, y=-1,
                       facing=6, anim=1, cam_x=1900, cam_y=-120, abilities=abilities,
                       objects=objects)
    assert len(pkt) == HDR_SIZE + 2 * OBJ_SIZE, len(pkt)
    st = decode_state(pkt)
    assert st["seq"] == 7 and st["game_frame"] == 123456 and st["hook_tick"] == 99
    assert st["mode"] == "in_world" and "cmds_active" in st["flag_names"]
    p = st["player"]
    assert (p["net_id"], p["hp"], p["hp_max"], p["x"], p["y"], p["facing"], p["anim"]) == \
        (42, 150, 150, 2048, -1, 6, 1)
    assert p["class"] == "warrior"
    assert [(x["id"], x["ready"], x["cd_remaining"], x["cd_total"]) for x in p["abilities"]] == \
        [(1, True, 0, 300), (2, False, 120, 300), (3, False, 75, 150), (4, True, 0, 30),
         (5, False, 600, 600)]
    assert st["camera"] == {"x": 1900, "y": -120}
    o = st["objects"]
    assert (o[0]["net_id"], o[0]["type_id"], o[0]["is_player"], o[0]["x"], o[0]["y"],
            o[0]["hp_pct"]) == (17, 1234, True, 100, -20, None)
    assert (o[1]["class_flags"], o[1]["hp_pct"]) == (0x2000000, 50)
    # Byte-level spot checks of the C layout.
    assert pkt[0:4] == b"NXPT"
    assert struct.unpack_from("<I", pkt, 24)[0] == 42          # net_id offset
    assert struct.unpack_from("<i", pkt, 36)[0] == 2048        # x offset
    assert struct.unpack_from("<i", pkt, 48)[0] == 1900        # cam_x offset
    assert pkt[56] == 1                                        # abilities[0].id
    assert struct.unpack_from("<H", pkt, 56 + 8 + 4)[0] == 120  # abilities[1].cd_remaining
    for bad in (pkt[:-1], b"XXXX" + pkt[4:]):
        try:
            decode_state(bad)
        except ValueError:
            pass
        else:
            raise AssertionError("bad packet accepted")
    c = encode_cmd("move", orient_from_vec(1, 0), 3, ttl_ms=300, seq=5)
    assert len(c) == CMD_SIZE and c[0:4] == b"NXPC"
    assert decode_cmd(c) == {"op": 1, "ttl_ms": 300, "seq": 5, "a": 0, "b": 3}
    assert decode_cmd(encode_cmd("aim", -5, 70000)) == {"op": 2, "ttl_ms": 0, "seq": 0,
                                                        "a": -5, "b": 70000}
    # Orientation convention round-trips (east=0, south=64, west=128, north=192).
    assert [orient_from_vec(*d) for d in ((1, 0), (0, 1), (-1, 0), (0, -1))] == [0, 64, 128, 192]
    for o_ in range(0, 256, 7):
        assert orient_from_vec(*vec_from_orient(o_)) == o_
    print("selftest OK (hdr=%d obj=%d cmd=%d)" % (HDR_SIZE, OBJ_SIZE, CMD_SIZE))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true", help="round-trip encode/decode and exit")
    sub = ap.add_subparsers(dest="cmd")

    ls = sub.add_parser("listen", help="print state packets as JSON lines")
    ls.add_argument("--port", type=int, default=47800)
    ls.add_argument("--bind", default="0.0.0.0")
    ls.add_argument("--count", type=int, default=0, help="exit after N packets (0 = forever)")
    ls.add_argument("--every", type=int, default=1, help="print every Nth packet")

    sd = sub.add_parser("send", help="send one command")
    sd.add_argument("--to", required=True, help="3DS IP:listen_port, e.g. 127.0.0.1:47801")
    sd.add_argument("--ttl", type=int, default=0, help="latch ms for move/aim (0 -> 250)")
    sd.add_argument("--repeat", type=int, default=1, help="send N times")
    sd.add_argument("--interval", type=float, default=0.1, help="seconds between repeats")
    sd.add_argument("--dx", type=float, default=1.0)
    sd.add_argument("--dy", type=float, default=0.0)
    sd.add_argument("--orient", type=int, default=None, help="0..255 (0=east, 64=south)")
    sd.add_argument("--run", action="store_true")
    sd.add_argument("--stop", action="store_true", help="move: hold still (override stick)")
    sd.add_argument("--x", type=int, default=0)
    sd.add_argument("--y", type=int, default=0)
    sd.add_argument("--self", dest="self_cast", action="store_true", help="slot: cast on self")
    sd.add_argument("op", choices=sorted(OPS))
    sd.add_argument("args", nargs="*")

    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.cmd == "listen":
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind((args.bind, args.port))
        n = 0
        while True:
            data, addr = s.recvfrom(2048)
            try:
                st = decode_state(data)
            except ValueError as e:
                st = {"error": str(e), "len": len(data)}
            st["from"] = "%s:%d" % addr
            n += 1
            if n % max(1, args.every) == 0:
                sys.stdout.write(json.dumps(st, separators=(",", ":")) + "\n")
                sys.stdout.flush()
            if args.count and n >= args.count:
                return 0
    if args.cmd == "send":
        pkt = build_cmd(args)
        target = parse_hostport(args.to)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for i in range(max(1, args.repeat)):
            s.sendto(pkt, target)
            if i + 1 < args.repeat:
                time.sleep(args.interval)
        return 0
    ap.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
