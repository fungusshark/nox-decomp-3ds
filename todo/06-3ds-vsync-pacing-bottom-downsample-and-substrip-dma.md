# perf(3ds,video): fix dual-limiter VBlank judder, double-buffer linear staging, and optimize VQA / idle GPU cadence

**Priority:** `P0`  
**Labels:** `performance`, `3ds`, `video`, `vsync`

---

## Summary
*(Note: Bottom `512x256` VRAM/2x2 SWAR downsample, static menu splash `reuse_tex`, and 8-row sub-strip GX DMA are being implemented as part of Issue #01 on `feat/fov_scaler`. This issue tracks the remaining VBlank frame-pacing, double-buffered DMA, and VQA pacing fixes.)*

`nox_3ds_video_init()` leaves `citro3d` at 60 Hz (`C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` blocks up to 16.67 ms on VBlank), after which `mainloop()` runs a **second, unsynchronized 30.30 Hz software `Sleep()` / `SDL_Delay(1)` limiter**. This causes 1:3 VBlank beat-frequency judder (16.7 ms / 50.0 ms alternating frames) and premature 30 -> 20 FPS drops. In 15 FPS audio-clocked VQA movies, `C3D_FRAME_SYNCDRAW` also stalls Core 0 needlessly for up to 16.67 ms/frame. Furthermore, `C3D_SyncDisplayTransfer` queues asynchronously inside `C3D_FrameBegin..FrameEnd`, risking single-buffer overwrites if Core 0 starts the next frame early.

---

## Exact Locations
- `port/3ds/video_3ds.c:272-305` (`nox_3ds_video_init`), `401` (`nox_3ds_video_present_dual`), `496` (`nox_3ds_video_present_movie`)
- `src/GAME1.c:20813-20889` (`sub_416C70`, `sub_416CD0`, `sub_416DD0`), `52091`, `52317-52318`, `52567-52590` (`mainloop` frame limiter)
- `src/movie.c:1814-1822` (`sub_556050`), `3944`, `3968-3970` (`sub_558370`)

---

## Proposed Fixes
1. **Eliminate Double-Sleep VBlank Judder:**
   - Call `C3D_FrameRate(30.0f)` (`waitCount = 2` = exact 30.0 Hz hardware VSync) in `nox_3ds_video_init()`, and skip the redundant `Sleep` / `SDL_Delay(1)` loop in `GAME1.c:52567-52590` on `#ifdef __3DS__` whenever `do_present` presented a frame (keeping `sub_416DD0` only on skipped/headless frames, and updating `sub_416D40()` to stay anchored to VBlank) — **OR** use non-blocking `C3D_FrameBegin(0)` with ping-pong linear buffers.
2. **Non-Blocking VQA Movie Pacing:**
   - Pass `C3D_FrameBegin(0)` in `nox_3ds_video_present_movie()` (`video_3ds.c:496`) so NDSP's audio clock (`nox_3ds_movie_audio_played_bytes()`) is the sole master clock, and pass letterboxed active `(crop_x, crop_y, crop_w, crop_h)` from `sub_556050()` (`movie.c:1814`) to cut VQA flush/DMA by ~30%.
3. **Ping-Pong Double-Buffered Linear Staging:**
   - Alternate between two `linearAlloc` Top/Bottom staging buffers so `C3D_SyncDisplayTransfer` (async inside `gxCmdQueue`) never races Core 0 writing the next frame.
4. **Idle Title / Minimap Bottom Cadence:**
   - Use `nox_3ds_swbuf_reuse_bottom()` on static Title frames (`GAME1.c:52317`) after frame 1, and optionally throttle idle `minimap_on` updates when player/camera hasn't moved.
