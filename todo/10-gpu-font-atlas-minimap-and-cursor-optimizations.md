# perf(3ds,ui): optimize font_3ds atlas/glyph transform, minimap divisions, and cursor blitting

**Priority:** `P1`  
**Labels:** `performance`, `3ds`, `ui`, `font`

---

## Summary
1. **`port/3ds/font_3ds.c` Memory & Per-Glyph CPU:** Permanently leaks 256 KB of `linearAlloc` (`s_atlas_staging`) after Frame 1 upload and allocates a `256x256 GPU_RGBA8` VRAM texture (256 KB) when only `128x48` (9.4%) is used; executes up to **12 `floorf`/`ceilf` calls + 4 float UV divides per shadowed glyph**; and `sub_43F840`/`43F9E0`/`43FAF0` (`GAME1.c`) scan `nox_3ds_font_slot_from_ptr` per character instead of once per string.
2. **`sub_46C2E0` Recursive Auto-Fit Pre-Pass (`GAME2.c:23533-23652`):** Re-measures every visible widget's UTF-16 string width every frame even when static.
3. **Enlarged `220x220` Minimap (`sub_472600`, `GAME2.c:28158-28520`):** Runs 2–5 software `__aeabi_idiv` divides by zoom + `(__int64)` `__aeabi_f2lz` casts per wall, door, waypoint, and entity.
4. **Software Cursor (`draw.c:2768, 3884, 3910-3971`):** Calls `SDL_BlitScaled` for 1:1 blits, allocates an unused 32 KB `dword_6F7C48`, and re-converts all `128x128` (`16,384`) pixels with a redundant `if (v == key555)` branch on every cursor animation tick.

---

## Exact Locations
- `port/3ds/font_3ds.c:33-59, 256-376, 721-782, 846-936`
- `src/GAME1.c:53739-53981` (`sub_43F840`, `sub_43F9E0`, `sub_43FAF0`)
- `src/GAME2.c:23533-23652` (`nox_3ds_widget_fits_crisp`, `sub_46C2E0`)
- `src/GAME2.c:28064-28521` (`sub_472600` minimap)
- `src/draw.c:2768-2771, 3884, 3910-3971` (`sdl_unk1`, `sub_48BF70`, `sub_48B3F0`)

---

## Proposed Fixes
1. **Shrink & Free Font Atlas Staging:** Resize atlas to `128x64` (`16 KB` `GPU_RGBA4` or `32 KB` `GPU_RGBA8`, saving `224–240 KB` VRAM) and `linearFree(s_atlas_staging)` 2 frames after initial GX upload (`s_atlas_upload_age >= 2`).
2. **Precompute Glyph UVs & Hoist Widget Clips:** Precompute `static Tex3DS_SubTexture s_crisp_subtex[95]` at init, cache transformed `(cx0, cy0, cx1, cy1)` across consecutive glyphs sharing a clip rect, and use integer/16.16 fixed-point screen coordinates (`(x + 1) >> 1` on `640x480 -> 320x240`).
3. **Hoist Font Slot Per-String & Memoize `sub_46C2E0`:** Compute `fslot`, `use_bot`, `nat_en` once at entry of `sub_43F840`/`43F9E0`/`43FAF0` (skipping `sub_43FE30()` on hits), and memoize `v3ds_gui_fb` across unchanged GUI frames.
4. **Minimap & Cursor:** Precompute `inv_zoom_q20 = (100 << 20) / zoom` once per `sub_472600` call (`SMULL` instead of `__aeabi_idiv`) and cast waypoints with `(int)`. Use `SDL_BlitSurface` when `src.w == dst.w && src.h == dst.h` in `draw.c:2768, 2771`, and restrict `sub_48B3F0` RGB555->RGBA5551 SWAR conversion to the active cursor bounding box (`w * h`).
