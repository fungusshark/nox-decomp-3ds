# perf(movie,input,compat): replace 128 KB VQA color LUT with SWAR, silence VQA stderr/SD stalls, and fix string/compat bugs & overhead

**Priority:** `P2`  
**Labels:** `performance`, `bug`, `movie`, `input`, `compat`

---

## Summary
Several localized bottlenecks and latent bugs affect VQA cutscenes, per-frame input, string helpers, and desktop/shared `compat.c`:
1. **VQA Cutscenes (`src/movie.c`):** `sub_55BB30(2)` allocates a **128 KB LUT** (4x ARM11's 32 KB L1 D-cache!) just to compute `((v & 0x7FFF) << 1) | 1` in `sub_55BC80`; `sub_558370` triggers ~30 unbuffered `svcOutputDebugString` traps/sec (`movie.c:3926`) and synchronous SDMC `fopen/fflush/fclose` via `nox_3ds_movie_log()` mid-playback; and LCW `sub_562C9B` copies non-overlapping matches byte-by-byte.
2. **Input Math & Uncached `getenv`:** `gamepad.c:94,105` call `getenv()` every frame; `input_3ds.c:145` calls software `powf(u, 1.75f)` instead of `sqrtf`; `input_3ds.c` injects stationary `MOUSE_ABS` every idle frame and calls `nox_3ds_input_is_world_mode()` 4x/frame; `input.c:1676` uses `atan2/cos/sin` to clamp a 2D vector.
3. **String & Compat Bugs/Overhead:**
   - **Latent Infinite Loop (`src/compat.c:332-337`):** `_strrev()` never increments `begin` or decrements `end`!
   - **OOB Read (`src/string.c:504-515`):** `nox_wcsstr()` calls `memcmp(..., len * sizeof(wchar_t))` past `haystack`'s `L'\0'` terminator.
   - **Heap Churn (`src/string.c:565-584`):** `nox_wcstol()` runs `nox_wcslen` + `malloc` + `free` on every integer parse.
   - **Desktop `casepath()` (`src/compat.c:2245-2352`):** Runs `opendir`/`readdir`/`closedir` per path component on every file open, plus unconditional `fprintf` spam.
   - **Hygiene (`src/windows.h:193`, `port/3ds/CMakeLists.txt`):** Missing `typedef` on `struct WSAData` emits a 398-byte common global in every TU; missing `-fdata-sections` leaves ~6 KB of dead data attached to stripped functions.

---

## Exact Locations
- `src/movie.c:1799-1811` (`sub_556050`), `3923-3938` (`sub_558370`), `6769-6884` (`sub_55BB30`/`sub_55BC80`), `13612-13717` (`sub_562C9B`)
- `port/3ds/platform_3ds.c:71, 271-291` (`stderr` `_IONBF`, `nox_3ds_movie_log`)
- `src/gamepad.c:92-111, 972-985, 1072-1140`, `port/3ds/input_3ds.c:135-147, 198-232, 604-673`, `src/input.c:1653-1701`
- `src/string.c:88, 111, 263, 286, 504-515, 565-584`
- `src/compat.c:321-339` (`_strrev`), `601, 638, 2240, 2245-2352` (`casepath`), `2401-2451`
- `src/windows.h:193-201`, `src/GAME_data.c:6-7` & `src/proto.h:14` (`byte_563006[16]` vs `[26]`)

---

## Proposed Fixes
1. Replace `sub_55BB30(2)` / `sub_55BC80` on `#ifdef __3DS__` with 32-bit SWAR `((v & 0x7FFF7FFFu) << 1) | 0x00010001u`, add `memcpy`/`memset` fast paths for non-overlapping / RLE runs in `sub_562C9B`, unroll 4px in `sub_556050`, remove `movie.c:3926` `fprintf`, buffer `nox_3ds_movie_log` in RAM, and set `stderr` to `_IOLBF`.
2. Memoize `gamepad.c` `getenv()` getters in `static int`, fast-path `powf(u, 1.75f)` as `float s = sqrtf(u); u * s * sqrtf(s)`, replace `atan2/cos/sin` clamp in `input.c:1676` with vector normalization, and skip idle `MOUSE_ABS` injection when unchanged.
3. Fix `_strrev()` (`*begin++ = *end; *end-- = tmp;`), rewrite `nox_wcsstr()` without OOB `memcmp`, make `nox_wcstol()` stack-only, add `access(path, F_OK) == 0` fast-path + debug-gate logs in `compat.c`, add `typedef` to `struct WSAData`, fix `byte_563006[16]` in `proto.h:14`, and add `-fdata-sections`.
