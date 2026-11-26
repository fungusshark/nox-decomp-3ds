# Performance & Scaling Backlog (`nox-decomp-3ds`)

This folder tracks the prioritized performance, hardware-utilization, memory-bandwidth, and UI/FOV scaling findings audited across the decompiled engine (`src/`), the New 3DS backend (`port/3ds/`), and compiled ARM11 ELF output (`build-3ds/nox.elf`).

> **Note:** GitHub Issues are currently disabled on `fungusshark/nox-decomp-3ds`. Once enabled in repository settings, the batch script at the bottom of this README can publish these specs directly as GitHub issues.

## Git Workflow & CI

* **[branching_ci.md](./branching_ci.md)** — Trunk-Based GitHub Flow, PR verification & rolling `latest-dev` pre-release rules, and concrete `.github/workflows/build.yml` integration plan (PR gate, `devkitpro/devkitarm` 3DS `.3dsx` job, rolling `latest-dev` updates, and `v*` milestone assets).

---

## Priority Matrix

| # | Priority | Status | Title | Primary Bottleneck / Impact | Spec |
| :-: | :---: | :---: | :--- | :--- | :--- |
| **01** | **P0** | **In Progress (`feat/fov_scaler`)** | Cross-platform FOV, 5:3/4:3 viewport & bandwidth-optimal UI/world scaler + Server List readability | Cuts world overdraw 1.78x–3.20x, Bottom DMA/RAM 4x (`512x256` 2x2 SWAR box), adds Lanczos-2 splash pre-scale, separates UI/Native Font from world | [01-cross-platform-fov-and-render-scaler.md](./01-cross-platform-fov-and-render-scaler.md) |
| **02** | **P0** | Planned | Unpack 4-byte math structs (`defs.h`), add `-fno-math-errno/-fno-trapping-math`, fix CMake `-ftree-vectorize` & desktop `-O0` | Collapses 699 `4x ldrb` byte-float cascades to `vldr.32`, removes `vcmp/vmrs` FPSCR stalls & `bl sqrt`, fixes desktop `-O0` | [02-arm11-struct-packing-math-errno-and-cmake-flags.md](./02-arm11-struct-packing-math-errno-and-cmake-flags.md) |
| **03** | **P0** | Planned | Disable dead O(N^2) 1999 anti-cheat shuffler, fix 50%-frame 3,000-obj walk, drop 2 KB poll `memset` | Eliminates per-tick O(N^2) heap pointer chasing (`nullsub_31`), L1 cache wipes in `sub_518EE0`, dead `sub_40ED60` memset | [03-disable-1999-anticheat-and-visibility-cache-thrash.md](./03-disable-1999-anticheat-and-visibility-cache-thrash.md) |
| **04** | **P0** | Planned | Eliminate per-vertex 32/64-bit divides in RGB dynamic lighting & vectorize Gouraud/alpha blitters | ~10x speedup in `sub_469140`/`sub_4697C0`, fullbright/SWAR floor & wall Gouraud, 15-20x speedup in `sub_49EAB0` shadow edges & `sub_49D370` | [04-dynamic-rgb-lighting-and-swar-software-blitters.md](./04-dynamic-rgb-lighting-and-swar-software-blitters.md) |
| **05** | **P0** | Planned | Remove `3x modf/VDIV.F64` per raycast step, cut 80-95% monster path raycasts, eliminate `double` promotions | Cuts `sub_535250` DDA cost ~6x, skips 80-95% of `sub_50D3B0` raycasts (bit-exact), deduplicates `WallMap` hash lookup, fixes `distSq` early-outs | [05-raycast-dda-monster-path-raycasts-and-float-math.md](./05-raycast-dda-monster-path-raycasts-and-float-math.md) |
| **06** | **P0** | Planned | Fix dual-limiter VBlank judder, Bottom `512x256` downsample, static splash reuse & 8-row sub-strip GX DMA | Eliminates 16.7ms/50.0ms beat-frequency judder & VQA 60Hz stall; cuts GX DMA by 22-75% | [06-3ds-vsync-pacing-bottom-downsample-and-substrip-dma.md](./06-3ds-vsync-pacing-bottom-downsample-and-substrip-dma.md) |
| **07** | **P1** | Planned | Offload audio stream decode/SDMC I/O to Core 2/1, expand `audio.bag` cache on N3DS, fix 64 KB `setvbuf` amplification | Eliminates mid-combat/dialog SD hitches, unlocks `sub_43EE00` 16 KB zero-copy SFX, removes per-MP3-frame 16 KB `memmove` | [07-3ds-multicore-audio-worker-and-bag-read-amplification.md](./07-3ds-multicore-audio-worker-and-bag-read-amplification.md) |
| **08** | **P1** | Planned | Add O(1) hash tables (`extent`/`script_id`/`netcode`/`video.idx`/spells/waypoints) & optimize pathfinding grid | Replaces 3,000-node linear scans in NoxScript & `video.idx` loads; precomputes `sub_50C830` floor bit & flattens `70x70` spatial grid | [08-o1-hash-tables-and-pathfinding-grid-optimizations.md](./08-o1-hash-tables-and-pathfinding-grid-optimizations.md) |
| **09** | **P1** | Planned | Collapse `abi_wrappers` VFP ping-pongs, elide single-thread `RecursiveLock`, fix `mallinfo()` getter & `video.bag` arena | Removes `r0->s0->r4` trampolines, removes `dlmalloc` heap walks in `nox_3ds_get_state()`, packs 3-5x more sprites in `video.bag` cache | [09-abi-trampoline-removal-lock-elision-and-sprite-ring-arena.md](./09-abi-trampoline-removal-lock-elision-and-sprite-ring-arena.md) |
| **10** | **P1** | Planned | Optimize `font_3ds` atlas/glyph transform, minimap divisions & software cursor blits | Frees 256 KB linear FCRAM + 224 KB VRAM, removes `12x floorf/ceilf` per glyph, replaces minimap `__aeabi_idiv` with `inv_zoom_q20` | [10-gpu-font-atlas-minimap-and-cursor-optimizations.md](./10-gpu-font-atlas-minimap-and-cursor-optimizations.md) |
| **11** | **P2** | Planned | Connect `g_raw_write_cache` to crypt reads, batch 8-byte crypt writes, shrink main stack `1 MB -> 128 KB` | Eliminates 1-3 MB SD re-reads after `.nxz` extract, batches 200k `fwrite(8)` -> ~400, reclaims ~1 MB FCRAM | [11-crypt-write-cache-handoff-batching-and-stack-shrink.md](./11-crypt-write-cache-handoff-batching-and-stack-shrink.md) |
| **12** | **P2** | Planned | Replace 128 KB VQA LUT with SWAR, silence VQA `stderr`/SD stalls, fix string/compat bugs & `-fdata-sections` | Speeds up VQA playback & removes L1 thrash; fixes `_strrev()` infinite loop, `nox_wcsstr` OOB read, desktop `casepath()` dir scans | [12-vqa-cutscenes-input-string-and-compat-hygiene.md](./12-vqa-cutscenes-input-string-and-compat-hygiene.md) |

---

## Publishing These to GitHub Issues (Once Enabled)

```bash
REPO="fungusshark/nox-decomp-3ds"
for f in todo/[0-9][0-9]-*.md; do
  title=$(head -n 1 "$f" | sed 's/^# //')
  prio=$(grep -m1 '^\*\*Priority:\*\*' "$f" | sed -E 's/.*`(P[0-2])`.*/\1/')
  gh issue create -R "$REPO" --title "[$prio] $title" --body-file "$f"
done
```
