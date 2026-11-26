# Nintendo 3DS Hardware Learnings & Architectural Constraints (`nox-decomp-3ds`)

> **Scope:** Hard-won hardware lessons verified across `v0.1.0`–`v0.2.7-3ds-hwfix` and the reverted `v0.3.0` optimization experiment on real Nintendo 3DS / New 3DS hardware (`ARM11 MPCore` + `PICA200` + `Horizon OS`), especially where real hardware differs sharply from Citra / Azahar emulation.

---

## 1. Dual-Screen LCD Controllers (`PDC0` / `PDC1`), VBlank Phase Offset & 30 FPS Pacing

### Independent Buffers & Scanout Registers
Top and Bottom displays use completely separate memory and hardware scanout pipelines:
- **Software buffers (linear FCRAM):** Separate Top (`NOX_3DS_SWBUF_TOP`) and Bottom (`NOX_3DS_SWBUF_BOTTOM`) buffers managed in [`src/draw.c`](../src/draw.c).
- **VRAM textures & GPU render targets:** Separate `s_top_slot` / `s_top_target` (`400x240`) and `s_bot_slot` / `s_bot_target` (`320x240`) in [`port/3ds/video_3ds.c`](../port/3ds/video_3ds.c), rendered sequentially in one PICA200 command list (`C3D_FrameBegin` .. `C3D_FrameEnd`).
- **Hardware double-buffers (`GFX_TOP` / `GFX_BOTTOM`):** `libcitro3d`'s `onQueueFinish` calls `gfxScreenSwapBuffers` independently per screen so PDC0 (`VBlank0`) and PDC1 (`VBlank1`) each latch swaps on their own next VBlank with zero tearing.

### Why `C3D_FrameRate(30.0f)` Locked Real 3DS Hardware to 20 FPS (`50.14 ms`)
- **Emulator vs. Real Hardware Difference:** Citra / Azahar fire `VBlank0` (Top) and `VBlank1` (Bottom) together, whereas real 3DS LCD controllers offset Top and Bottom VBlank interrupts by several milliseconds.
- **Software Counter Phase Trap:** Calling `C3D_FrameRate(30.0f)` causes `libcitro3d`'s `onVBlank0` and `onVBlank1` callbacks to increment `frameCounter[0]` and `frameCounter[1]` only every **2nd** 60 Hz scanout. Because `nox_3ds_console_detach()` waits on `VBlank0` right before `C3D_Init()`, `onVBlank1` and `onVBlank0` end up **1 VBlank out of phase** on real hardware (even scanouts tick Bottom, odd scanouts tick Top).
- **The 3-VBlank Barrier:** `C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` blocks inside `gspWaitForAnyEvent()` until **both** `frameCounter[0]` and `frameCounter[1]` change *after* CPU rendering finishes. Whenever normal gameplay takes `18–28 ms`, the first screen's counter has already ticked at `16.71 ms`, forcing `C3D_FRAME_SYNCDRAW` to wait **3 VBlanks (`50.14 ms` = 20.0 FPS)**.

### Wall-Clock vs. 2× VBlank Drift (Periodic Frame-Skip Stutter)
- 3DS LCD VBlank period is `16.7138 ms` (`59.83 Hz`), so 2 VBlanks = `33.4276 ms` (`29.9156 Hz`).
- Westwood's wall-clock pacing compares `30.0000 Hz` (`33.333 ms`) and integer `1000 / 30 = 33.000 ms` sleeps (`sub_416C70(30)` / `sub_416D70()`). Left enabled alongside VSync, that `~0.09–0.43 ms/frame` beat frequency periodically triggers engine frame-skips (`0x80000000`, noticeably blacking out/stalling multiplayer every ~10.6 s).

### Working Pattern (`v0.2.7-3ds-hwfix`)
- Keep `citro3d` at its default **`60.0f`** rate (never call `C3D_FrameRate(30.0f)` on dual-screen 3DS) and disable `-noskip` frame-skip on `__3DS__` (`byte_587000[85724] = 0` in [`src/GAME1.c`](../src/GAME1.c)).
- Pace `nox_3ds_video_present_dual()` strictly to **2 Top-Screen `VBlank0` ticks** (`C3D_FrameCounter(0)` + `gspWaitForEvent(GSPGPU_EVENT_VBlank0, false)`) and call `C3D_FrameBegin(0)` (`gxCmdQueueWait` only, no `VBlank1` stall), skipping `SDL_Delay` when `v3ds_did_present` already synced.
- Because *Nox* ties simulation physics/network lockstep to a fixed `30 ticks/s` without sub-tick interpolation, locked **30 FPS** keeps gameplay speed accurate, fits comfortably inside the ARM11 CPU budget, and puts Core 0 to sleep (`svcWaitSynchronization`) for ~35–40% of each frame for battery/thermals.

---

## 2. ARM11 MPCore (`ARMv6K` + `VFPv2`) CPU, Alignment & Compiler Rules

- **CPU & Cache Topology:**
  - **Old 3DS:** `268 MHz` ARM11 MPCore, `16 KB` L1 I-Cache / `16 KB` L1 D-Cache, **no L2 cache**.
  - **New 3DS (`osSetSpeedupEnable(true)`):** `804 MHz`, `32 KB` L1 I-Cache / `32 KB` L1 D-Cache + `2 MB` L2 cache, backed by high-latency FCRAM.
- **No Alignment Trap Emulation (Fatal Core 0 `Data Abort`):**
  - Horizon OS (`Kernel11`) does **not** emulate unaligned faults in software. Single-register `LDR`/`STR`/`LDRH`/`STRH` supports unaligned access in hardware (`SCTLR.U = 1`), while unaligned **`VLDR`/`VSTR` (`float *`/`double *`), `LDM`/`STM`, `LDRD`/`STRD`, or `LDREX`/`STREX`** causes an immediate **fatal `Data Abort` crash**.
  - **Rule:** Never apply `#pragma pack(push, 4)` to IDA structs like `float2`/`float4` in [`src/defs.h`](../src/defs.h) (doing so lets GCC emit `VLDR`/`LDM` on unaligned byte casts). Keep 3-byte/byte-offset reads through `memcpy`-based `nox_read_u16`/`u32`/`f32` helpers in [`port/3ds/platform_3ds.h`](../port/3ds/platform_3ds.h).
- **No NEON SIMD & Unpipelined `VFPv2`:**
  - `ARMv6K` has **no NEON**. Enabling `-ftree-vectorize` generates bloated scalar unrolled loops that pressure L1 I-cache.
  - `VFPv2` scalar divides (`VDIV.F32` ~14–16 cycles, `VDIV.F64` ~28–30 cycles) and `VCVT`/`VMOV` transfers to GPR stall the in-order pipeline — never replace integer/lookup paths with float divides (`VDIV.F32`) inside dynamic lighting (`sub_469140`/`sub_4697C0`) or raycast DDA loops.
- **Don't Oversize Static Tables (`16 KB–32 KB` L1 D-Cache):**
  - Large static tables (`64 KB–176 KB` hash/walkability caches) exceed L1 D-cache 4×–11× and thrash FCRAM worse than small hot loops.
- **Mandatory `1 MB` Main Stack (`__stacksize__`):**
  - Decompiled functions in [`src/GAME3.c`](../src/GAME3.c) and [`src/GAME4.c`](../src/GAME4.c) allocate up to `144 KB` per stack frame. Keep `__stacksize__ = 1024 * 1024` in [`port/3ds/platform_3ds.c`](../port/3ds/platform_3ds.c) (shrinking to `256 KB` is unsafe), and keep large decoder buffers like `stream_adpcm_seek`'s `64 KB` PCM temp buffer off the stack.

---

## 3. PICA200 GPU Offloading & Cache Coherency

- **Hardware Scaling Beats CPU Prescaling:**
  - *Nox* renders `RGBA5551`, matching PICA200 `GPU_RGBA5551`. Uploading via `C3D_SyncDisplayTransfer` and letting PICA200 draw Top (`400x240`, `GPU_NEAREST` 1:1 or `GPU_LINEAR`) and Bottom (`320x240`, `GPU_LINEAR`) costs **0.0 ms CPU** and **< 1.5 ms GPU**.
  - Avoid CPU software `2x2` box filters or Lanczos prescalers (`614 KB`/frame on ARM11) and keep `video.bag` color-depth locked to `16-bit` ([`src/GAME1.c`](../src/GAME1.c)).
- **Explicit Data Cache Flushes Required:**
  - ARM11 L1/L2 caches are **not coherent** with PICA200 DMA or DSP1.
  - Always call `GSPGPU_FlushDataCache()` on linear framebuffers before `C3D_SyncDisplayTransfer()` and `DSP_FlushDataCache()` on PCM `ndspWaveBuf`s before `ndspChnWaveBufAdd()`.
- **UI Readability (`font_3ds.c`):**
  - Render Top-Screen FPS telemetry on the Bottom Screen's top-right edge (`320 - w, 0`) to keep the 400×240 gameplay viewport clean and remove per-frame `mallinfo()` heap walks from `nox_3ds_get_state()`.

---

## 4. Horizon OS Cores, `soc:u` Wi-Fi Networking & SDMC Stalls

- **Core Affinity:** Core 0 runs the app main thread, Core 1 hosts Horizon OS system services (`soc:u`, `fs:USER`, `http:C`), and Core 2 is available on New 3DS (`threadCreate(..., 2)` with `-2` fallback). Do **not** reserve Core 1 for audio decoding with `APT_SetAppCpuTimeLimit(30)` — OS/socket/SDMC mutex contention stalls frame pacing.
- **Periodic Hosted Lobby Dips (Fixed in `v0.2.6`):**
  - Synchronous SDMC writes (`net.log`), uncached `lobby.txt` disk reads, per-frame `SOCU_GetIPInfo()` IPC calls in overlays, and blocking `gethostbyname()` / polling lobby workers cause periodic frame stalls during hosted lobbies.
  - Fixes kept in [`port/3ds/lobby_3ds.c`](../port/3ds/lobby_3ds.c) and [`port/3ds/net_sockets_3ds.c`](../port/3ds/net_sockets_3ds.c): opt-in `NOX_3DS_NET_LOG=1`, cached `lobby.txt` + 16-entry DNS cache, 2 s cached IP status for the menu banner, `LightEvent_Wait` event-driven worker thread, throttled LAN sweep (`NOX_3DS_LAN_SWEEP_PER_TICK=2`), and `20 s` `CONNECT_WAIT_LOOP` tolerance with `0x0E` retries.
- **Lazy `socInit()`:** Keep `socInit()` (`1 MB` `0x1000`-aligned buffer) deferred until Multiplayer entry so Solo Campaign / Quest work offline and in Airplane Mode.

---

## 5. Touchscreen Input, Modal Teardown & Native Font Integrity

- **Never Deduplicate Mouse Coordinate Injection:** Filtering repeated `(x, y)` pairs across frames (`inject_mouse_abs_dedup`) breaks stylus click/tap activation on quest and shopkeeper buttons.
- **Clear Modal Capture Before Dialog Teardown:** Calling `sub_44A360(0)` inside `sub_449A10()` before closing modal dialogs prevents dangling capture pointers from crashing Core 0 when leaving hosted chat-area dialogs.
- **Per-Widget Native Font Fallback:** Validate and fall back per widget (`sub_46C370`) instead of suppressing the whole screen (`sub_46C2E0`) so one overflowing string never degrades sibling HUD/menu labels.
