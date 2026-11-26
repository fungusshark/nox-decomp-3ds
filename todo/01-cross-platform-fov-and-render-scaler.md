# perf(3ds,ui): cross-platform FOV, 5:3/4:3 viewport & bandwidth-optimal UI/world scaler + Server List readability

**Priority:** `P0`  
**Status:** In Progress on branch `feat/fov_scaler`  
**Labels:** `performance`, `3ds`, `ui`, `architecture`

---

## Summary
Nox's UI `.wnd` files are authored for `640x480`, and on 3DS the engine currently software-rasterizes the world at `640x384` (`245,760` px) and the Bottom UI at `640x480` (`307,200` px) before downscaling on the GPU to `400x240` (Top) and `320x240` (Bottom). This wastes 60–75% of software fillrate and DMA bandwidth, breaks 4:3 square pixels during gameplay, and crushes `default.fnt` (`8px` cap) down to blurry `4.0px` (`8.7'` visual arcminutes on Old 3DS) in listboxes like the Server List (`noxworld.wnd`).

A cross-platform **FOV & Render Scaler (`src/fov_scaler.{h,c}`)** solves world overdraw, multi-device physical FOV parity (Old 3DS, New 3DS, 3DS/2DS XL, Switch, Steam Deck, Desktop), Bottom UI bandwidth, 5:3 vs 4:3 aspect correctness, and Server List readability cleanly in one layer while keeping UI/HUD and Native GPU Fonts strictly separated from world rendering.

---

## Optical / Viewing-Distance Analysis
Comfortable UI text legibility sits at `16'–22'` arcminutes (ISO 9241-303 floor `12'–15'`).

| Display & Viewing Distance | Resolution | 1 Phys Px | `default.fnt` SW (`4px` on 3DS) | `large.fnt` SW (`6.5px` on 3DS) | 1:1 `default.fnt` (`8px` phys) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **17" CRT (16" vis) @ 24"** | `640x480` | `2.86'` | `22.9'` *(8px)* | `37.2'` | — |
| **17" CRT (16" vis) @ 24"** | `800x600` | `2.29'` | `18.3'` *(8px)* | `29.8'` | — |
| **Old 3DS / 2DS Bot @ 12"** | `320x240` | `2.16'` | **`8.7'` (blur)** | `14.1'` | **`17.3'`** |
| **New 3DS (std) Bot @ 12"** | `320x240` | `2.38'` | **`9.5'` (blur)** | `15.5'` | **`19.0'`** |
| **New 3DS XL / 2DS XL @ 12"** | `320x240` | `2.99'` | **`12.0'` (blur)** | `19.5'` | **`23.9'`** |

**Insight:** 1 physical 3DS pixel at 12" subtends nearly the same visual angle as 1 CRT pixel at 24". World viewport resolution can be tuned per device (`CFGU_GetSystemModel`) to match CRT pixel arcminutes while cutting pixel count:
- **Old 3DS / 2DS / New 3DS (`3.53"–3.88"`):** `400x240` (5:3) / `320x240` (4:3) -> **2.56x–3.20x fewer pixels**, `GPU_NEAREST` 1:1 crisp.
- **3DS XL / New 3DS XL / New 2DS XL (`4.88"`):** `480x288` (5:3) / `480x360` (4:3) -> **1.42x–1.78x fewer pixels**, `GPU_LINEAR` hardware bilinear.

---

## Locations
- `src/fov_scaler.h`, `src/fov_scaler.c`
- `port/3ds/platform_3ds.h`, `port/3ds/platform_3ds.c` (`CFGU_GetSystemModel`)
- `port/3ds/video_3ds.c`, `port/3ds/font_3ds.c`, `port/3ds/input_3ds.c`, `port/3ds/stubs.c`
- `src/draw.c`, `src/GAME1.c`, `src/GAME2.c`, `src/GAME3.c`

---

## Proposed Architecture & Implementation
1. **Top World Pipeline (`GPU_NEAREST` / `GPU_LINEAR`, zero CPU resample cost):**
   - Center `(vp_w, vp_h)` around `(320, 240)` in `sub_436100` (`GAME1.c:46411`), `sub_476700` (`GAME2.c:31060`), `mainloop` (`GAME1.c:52344`), and `sub_440900` (`GAME1.c:54880`).
   - Sync `wide_stretch` (`5:3` vs `4:3`) so `4:3` renders `320x240` / `480x360` / `640x480` with square pixels (`GPU_NEAREST` when 1:1).
   - Convert & flush **only** the active `[crop_x..crop_x+crop_w, crop_y..crop_y+crop_h]` rectangle in `sub_4AD1E0` (`draw.c:3517`), and DMA only the 8-row Morton strip in `video_3ds.c`.
2. **Bottom UI Pipeline (`2x2` SWAR Box Filter into `512x256`, preserving Native GPU Fonts):**
   - Shrink `g_backbuffer_bot` & `s_bot_slot` from `1024x512` (`1 MB`) to `512x256` (`256 KB`, active `320x240`, saving 768 KB VRAM + 768 KB linear RAM).
   - Fuse carry-free 32-bit SWAR `2x2` box downsampling directly into `nox_3ds_swbuf_end_bottom()` (`draw.c:653`) *after* `nox_3ds_font_resolve_occlusion()` clears bit-15 probes, keeping `font_3ds.c` 1:1 GPU text crisp on top.
3. **One-Shot Load-Time / Offline Lanczos-2 Pre-Downscaling:**
   - Pre-downsample static `legal.wnd` Title Splash once in `nox_3ds_capture_top_splash()` using Lanczos-2 (`400x240` / `320x240`) and set `top_frame.reuse_tex = true` on subsequent menu frames (zero CPU copy, zero GX DMA).
   - *Future Offline Asset Baking (`video.bag` UI sprites):* Run a build-time Lanczos-2/3 offline baker on `.wnd` panel backgrounds & UI icons (`0.5x`) to reduce `video.bag` cache pressure on `320x240` targets.
4. **Server List (`noxworld.wnd`) Column & Font Promotion:**
   - In `sub_438480()` (`GAME1.c:47722`) and `nox_3ds_listbox_sync_metrics()` (`GAME3.c:723`), rebalance `10038..10042` column widths (`Name` `100 -> 156px`, `+56%`) and promote to `large.fnt` (`13px` cap) when Native GPU Font is OFF.
5. **OpenGL Pixel-Center Sharp-Bilinear AA (`src/draw.c`):**
   - Add `SHARP_BILINEAR` texel-center snapping in `fragment_source` for Desktop / Switch targets.
