# perf(physics,ai): remove 3x modf/VDIV.F64 per raycast step, cut 80-95% of monster path raycasts, and eliminate double/__aeabi_d2lz promotions

**Priority:** `P0`  
**Labels:** `performance`, `physics`, `ai`, `arm11`

---

## Summary
Core raycasting (`sub_535250`), monster path following (`sub_50D3B0`), narrowphase collision, and AI/spell math suffer heavily on scalar ARM11 VFPv2 from IDA-decompiled `double` promotions, `libm` `modf`/`sqrt` calls, premature `sqrt()` before squared-distance checks, and `(__int64)` float-to-int casts (`251` `__aeabi_d2lz`/`__aeabi_f2lz` calls in `nox.elf`).

---

## Exact Locations
- `src/GAME4.c:53125-53459` (`sub_535250`, DDA step loops `53299-53335` & `53388-53425`)
- `src/GAME4.c:18834-18948` (`sub_50D3B0`, waypoint scan `18879-18911`)
- `src/GAME5.c:17606-17734` (`sub_57C9A0` L17650-17653) & `src/GAME1.c:14821-14866` (`sub_410580`/`sub_4105E0`)
- `src/GAME1.c:23441-23497` (`sub_419A10`, `sub_419A70`, `sub_419AF0`, `sub_419B10` — 339 call sites)
- `src/GAME4.c:28535-28552` (`sub_5175E0` x87 magic-float cell conversion)
- `src/GAME5.c:8833-8928` (`sub_550D00`), `8406-8448` (`sub_550380`), `2761-2874` (`sub_5488B0`), `8617-8830` (`sub_550760`, `sub_550A10`), `17419-17488` (`sub_57C650`)
- `src/GAME4.c:29209-29247` (`sub_5180B0`), `29507-29605` (`sub_518550`), `43299-43338` (`sub_5286D0`), `18951-19020` (`sub_50D5A0`), `21976-22055` (`sub_510E60`), `48699-49050` (`sub_52F8A0`), `31874-31889` (`sub_51B100`)
- `src/GAME2.c:54678` (dead `sub_419B10(sqrt(...))` in `sub_496150`) & `55421-55464` (`sub_497C40`), `55673` (`sub_498130`/`sub_4982E0`)

---

## Proposed Fixes
1. **Eliminate `3x modf(x / 23.0)` + `VDIV.F64` per step in `sub_535250`:**
   - Replace `modf(expr / 23.0, &v73); v = sub_419B10(v73);` with `(int)((expr) / 23.0f)` (or `* 0.043478261f`), and convert local `double` temps/constants to `float`.
2. **Cut 80–95% of monster path-following raycasts in `sub_50D3B0` (`GAME4.c:18883`):**
   - Compute `float v8 = v19 * v19 + v18 * v18;` **before** `sub_535250(&v23, 0, 0, 132)` and skip the raycast whenever `v8 > 64.0f && v8 >= v17` (100% bit-exact!).
3. **Deduplicate `WallMap` hash lookup in `sub_57C9A0` (`GAME5.c:17650`):**
   - Call `sub_4105E0`/`sub_410580` **once** per cell and pass the resolved wall pointer into an inline `sub_57B500` check.
4. **Inline `sub_419A10..B10` & `sub_5175E0`:**
   - Move `sub_419A10` (`fabsf`, removing global-memory `0x7FFFFFFF` bit-mask store-forwarding stall!), `sub_419A70..B10` casts, and `sub_5175E0` into `static inline` helpers.
5. **Squared-Distance Early-Outs & `float` Math:**
   - Gate `sqrtf(distSq)` behind `distSq < rSum * rSum` in `sub_550D00`, `sub_550380`, `sub_5488B0`.
   - Delete `sqrt()` completely in `sub_57C650` (`GAME5.c:17440`, used only as `v6 * v6`) and dead `sub_419B10(sqrt(...))` in `sub_496150` (`GAME2.c:54678`).
   - Replace `sqrt` comparisons in `sub_5180B0`, `sub_518550`, `sub_5286D0` (FOV dot product), and `sub_50D5A0` with squared comparisons; convert `sin`/`cos`/`atan2`/`sqrt` in `sub_510E60`, `sub_52F8A0`, `sub_51B100` to `float`; replace 32-bit-range `(__int64)(float_expr)` casts with `(int)` (`vcvt.s32.f32`).
   - Fix `sub_498130` (`GAME2.c:55673`) to remove occluded FOV segments at known index `i` without linear rescanning, and constant-fold `sub_419A70(11.5)`/`(5.75)` in `sub_497C40`.
