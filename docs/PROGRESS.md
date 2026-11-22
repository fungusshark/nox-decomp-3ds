# Nox New 3DS Port — Progress Tracker

**Branch:** `port-3ds`  
**Architecture Blueprint:** [docs/3DS_PORT_PLAN.md](./3DS_PORT_PLAN.md)  
**Target Artifact:** `nox.3dsx` (`sdmc:/3ds/nox/`)

---

## Milestone Status

| Milestone | Status | Summary / Exit Gate |
| :--- | :---: | :--- |
| **M0: Audit & Architecture Blueprint** | ✅ Complete | Audited `nox-decomp-3ds` + `../opennox-3ds`, memory/stack/alignment hazards, audio/movie/network boundaries; authored `docs/3DS_PORT_PLAN.md`. |
| **M1: Toolchain, CMake & Cross-Compile Boot** | ⬜ Not Started | `/opt/devkitpro/cmake/3DS.cmake` build producing `nox.3dsx`, `port/3ds/platform_3ds.h`, `osSetSpeedupEnable(true)`, 1 MB stack, bottom console, SDMC asset open test. |
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
