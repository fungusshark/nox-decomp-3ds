# Nox New 3DS Port — Progress Tracker

**Branch:** `port-3ds`  
**Architecture Blueprint:** [docs/3DS_PORT_PLAN.md](./3DS_PORT_PLAN.md)  
**Target Artifact:** `nox.3dsx` (`sdmc:/3ds/nox/`)

---

## Milestone Status

| Milestone | Status | Summary / Exit Gate |
| :--- | :---: | :--- |
| **M0: Audit & Architecture Blueprint** | ✅ Complete | Audited `nox-decomp-3ds` + `../opennox-3ds`, memory/stack/alignment hazards, audio/movie/network boundaries; authored `docs/3DS_PORT_PLAN.md`. |
| **M1: Toolchain, CMake & Cross-Compile Boot** | ✅ Complete | Standalone `port/3ds/CMakeLists.txt` (`/opt/devkitpro/cmake/3DS.cmake`, `-g`, `-Wcast-align`, `-fshort-wchar`, `-O2`) builds `nox.elf` (6.21 MB static) and `nox.3dsx` (2.6 MB), copies to `/mnt/c/3ds-dev/nox.3dsx`, and exposes `run-3ds` (`3dslink`). Implemented `port/3ds/platform_3ds.h` / `platform_3ds.c` (1 MB stack, `osSetSpeedupEnable(true)`, non-fatal `romfsInit()`, `sdmc:/3ds/nox/` path resolver + `chdir`, bottom console, `aptMainLoop()`), `port/3ds/stubs.c` (`LOG_STUB()`), and SDL2/GL/AL shim headers. Desktop `build-linux` verified green. |
| **M2: PICA200 Video & Main Menu Bring-Up** | ⬜ Not Started | 640×480 `RGBA5551` `linearAlloc` + `GSPGPU_FlushDataCache` → 1024×512 `C3D_Tex` → 400×240 `citro2d` top screen, movie stub, basic menu navigation. |
| **M3: Loopback Net, Controls, Alignment & Playable Ch.1** | ⬜ Not Started | `net_loopback_3ds.c` (no `socInit`), 53 float + 12 global + `video.idx` alignment fixes, full New 3DS controls, playable Solo Campaign Ch.1 + save/load. |
| **M4: OpenNox Bug-Fix Backports & Stability** | ⬜ Not Started | Backport P0/P1 fixes from `../opennox-3ds` (`v1.8.0..v1.9.0-alpha`) covering campaign softlocks, NoxScript, GUI memory corruption, spells, and audio races. |
| **M5: Full `ndsp` Audio Subsystem** | ⬜ Not Started | 24-ch `ndsp` (`linearAlloc` + `DSP_FlushDataCache`), IMA ADPCM SFX (`audio.bag`), looping music, MP3-in-WAV dialog (`minimp3.h`), `AIL_serve()` sync timers, fix 64 KB stack buf in `stream_adpcm_seek`. |
| **M6: Dual-Screen Touch HUD, `swkbd` & Polish** | ⬜ Not Started | Bottom 320×240 `citro2d` touch HUD (spells/potions/UI), `swkbdInit` text input, optional 400×240 gameplay viewport mode, FPS/RAM tuning (< 110 MB), `.smdh` packaging. |

---

## Key Metrics & Budgets

| Metric | Target | Current |
| :--- | :--- | :--- |
| Peak Regular Heap + Linear Pool | `< 110 MB` | TBD (Linux 32-bit ELF static baseline: `8.12 MB`) |
| Main Thread Stack (`__stacksize__`) | `1 MB` | Planned (max audited C frame: `144 KB` in `GAME3.c`) |
| Unaligned `*(float *)` Sites Fixed | `53 / 53` | `0 / 53` (audited) |
| Priority OpenNox Backports Applied | `P0 + P1` | `0` (26 prioritized items triaged) |
| Top Screen Gameplay FPS (New 3DS) | `≥ 30 FPS` | TBD |

---

## Activity Log

- **2026-11-21:** Completed full codebase & git history audit (`nox-decomp-3ds` 368 commits + `../opennox-3ds` 213 fixes since `v1.8.0`), verified devkitARM macros (`__ARM_PCS_VFP=1`, `__ARM_ARCH_6K__=1`, 4-byte default `wchar_t` requiring `-fshort-wchar`), measured static ELF footprint (`8.12 MB`), scanned stack frames & ARM11 alignment hazards, and published `docs/3DS_PORT_PLAN.md` and `docs/PROGRESS.md`.
- **2026-11-21 (M1):** Added `port/3ds/` CMake build system (`/opt/devkitpro/cmake/3DS.cmake`), `platform_3ds.h`/`platform_3ds.c`, `stubs.c` with rate-limited `LOG_STUB()`, and shim headers (`SDL2/`, `GL/`, `AL/`). Fixed `sub_549860__abi_raw` linkage in `abi_wrappers.c` and added `__3DS__` guards in `defs.h`, `main.cpp`, `win.c`, and `compat.c`. Verified `build-3ds/nox.elf` (`6.21 MB` static), `build-3ds/nox.3dsx` (`2.6 MB`), `/mnt/c/3ds-dev/nox.3dsx`, and desktop `build-linux` all build cleanly.
