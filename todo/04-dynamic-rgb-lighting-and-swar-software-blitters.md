# perf(render): eliminate per-vertex software 32/64-bit divides in RGB dynamic lighting and vectorize Gouraud & alpha/shadow blitters

**Priority:** `P0`  
**Labels:** `performance`, `rendering`, `arm11`

---

## Summary
ARM11 MPCore has **no hardware integer `SDIV`/`UDIV` instructions**, and `-fno-strict-aliasing` forces GCC to reload global config values from `byte_5D4594` across every `*dst++` pixel store. Together these make Nox's RGB lighting solver and software blitters dramatically slower than necessary:
1. Active 16-bit RGB lighting (`sub_469140` / `sub_4697C0`) executes software `__aeabi_uidiv` + cross-TU 64-bit `__aeabi_ldivmod` (`sub_4C1C70`) + 3 cross-TU calls (`sub_4C1C60`) **per lit grid vertex per light** (~200+ cycles/vertex).
2. Floor (`sub_480BE0`) and Wall (`sub_480860`/`sub_480EF0`) Gouraud shaders lack fullbright `memcpy` / monochrome 32-bit SWAR fast paths and reload `a4[0..2]`/`a5[0..2]` from memory per pixel.
3. `sub_49EAB0` (called **10 times per FOV shadow polygon edge every frame** by `sub_498AE0`) and `sub_49D370` (50% alpha rect) reload masks, shifts, alpha, and 3 LUT pointers from `byte_5D4594` on **every pixel** (~40 cycles/px vs ~2–4 cycles/px with hoisted locals + 32-bit SWAR).

---

## Exact Locations
- `src/GAME2.c:20490-21012` (`sub_468F80`, `sub_469140` L20729-20763, `sub_4695E0` L20807-20862, `sub_4697C0` L20983-20996) & `src/GAME3.c:23114-23130` (`sub_4C1C60`, `sub_4C1C70`)
- `src/GAME2.c:21015-21065` (`sub_469920`) & `42498-42937` (`sub_484F90`, `sub_4855D0`, `sub_485740`)
- `src/GAME2.c:38135-39020` (`sub_480860`, `sub_480BE0`, `sub_480EF0`)
- `src/GAME2.c:31292-31542` (`sub_476AE0` indirect call to `sub_476D70`)
- `src/GAME2.c:59970-61738` (`sub_49D370`, `sub_49D8E0`, `sub_49DBB0`, `sub_49EAB0`)

---

## Proposed Fixes
1. **RGB Dynamic Lighting (`sub_469140`, `sub_4697C0`, `sub_468F80`):**
   - Hoist per-light constants (`num`, `c_scale`, `thresh`, `r_col/g_col/b_col`, `neg`) outside X/Y loops.
   - Compute `float k = (66.0f * c_scale) / ((float)v36 * 65536.0f);` (or Q16 `step_q16` once per light + LUT reciprocal) and inline `sub_4695E0` / `sub_4C1C60` via `((int64_t)v5 * r_col) >> 8` (`SMULL`), eliminating both `__aeabi_uidiv` and `__aeabi_ldivmod` per vertex.
   - Use `memset32` in `sub_468F80` for fullbright / monochrome ambient clears.
2. **Floor & Wall Gouraud (`sub_480BE0`, `sub_480860`, `sub_480EF0`):**
   - In `sub_480BE0`: add `memcpy` fast path for fullbright chunks (`>= 0xF80000` with 0 delta), 32-bit RB+G SWAR fast path for flat monochrome chunks, and pre-shift `s_r/s_g/s_b = val >> 16` in the general loop.
   - In `sub_480860`: hoist `a4[0..2]` and `a5[0..2]` into local ints, bypass LUTs in RGB555 mode (`byte_5D4594[3799624] == 1`) with a monochrome SWAR path; in `sub_480EF0`, replace 3 `/ v43` divides with 1 reciprocal multiply.
3. **SWAR Alpha & Shadow-Edge Lines (`sub_49D370`, `sub_49EAB0`, `sub_49D8E0`, `sub_49DBB0`):**
   - `sub_49D370`: 2-pixel-per-word SWAR 50% blend (`((d & 0x7FFF7FFFu) & c32) + ((((d & 0x7FFF7FFFu) ^ c32) & 0x7BDE7BDEu) >> 1)`), preserving `0x8000` bit-15 stencil under `#ifdef __3DS__`.
   - `sub_49EAB0`: hoist `alpha`, `inv`, `color16`, `row_tbl`; add black-line SWAR fast path (`color16 == 0`, which is 100% of `sub_498AE0` FOV edge softening!) preserving `0x8000`.
4. **Bilinear Light `/ 23` Reciprocals (`sub_469920`, `sub_485740`) & Static Floor Object Copy (`sub_476AE0`):**
   - Replace 7–12 `/ 23` & `% 23` divides with unsigned reciprocal `((uint32_t)v * 0x0B21642Du) >> 27` + 23-entry Q16 weight LUT; inline `memcpy` in `sub_476AE0` when `px_fmt == 1`.

---

## Safety Notes
- Preserve `a4[0..2]` writeback on return from `sub_480860` because `sub_480EF0` steps across subsequent transparent RLE runs.
- Preserve bit-15 (`0x8000`) in `#ifdef __3DS__` GUI alpha/darken helpers (`sub_49D370`, `sub_49EAB0`, `sub_49D8E0`, `sub_49DBB0`) so GPU font occlusion stays intact.
