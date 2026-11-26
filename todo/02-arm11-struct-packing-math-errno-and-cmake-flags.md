# perf(build,arm11): unpack 4-byte math structs in defs.h, add -fno-math-errno/-fno-trapping-math, fix CMake -ftree-vectorize & desktop -O0

**Priority:** `P0`  
**Labels:** `performance`, `arm11`, `compiler`, `build`

---

## Summary
Disassembly of `build-3ds/nox.elf` (`arm-none-eabi-gcc 16.1.0`) exposed three high-impact compiler/build bottlenecks:
1. `#pragma pack(push, 1)` in `src/proto.h:2` wraps `src/defs.h`, forcing `alignof(float2/float4/shape) == 1` and causing **699 8-instruction byte-by-byte float load/store cascades** (`4x ldrb + 3x orr + vmov`) across hot physics, raycast, spatial-grid, and AI code.
2. Missing `-fno-math-errno -fno-trapping-math` emits serializing `vcmp + vmrs APSR_nzcv, fpscr` stalls (~12–20 cycles) and register-clobbering `bl <sqrt>` slowpaths around 106 `sqrt`/`sqrtf` call sites.
3. Explicit `-fno-tree-vectorize` in `port/3ds/CMakeLists.txt:41` silently overrides `-O3`'s vectorization on `port/3ds/*.c`, pure hand-written `src/*.c` TUs stay at `-O2`, and `src/CMakeLists.txt:21` hardcodes `-O0` on desktop builds.

---

## Exact Locations & Disassembly Evidence
- **`src/proto.h:1-5, 7763`** and **`src/defs.h:379-443`** (`float2`, `float4`, `int2`, `int4`, `shape`, `string`, `wstring`).
  - Disassembly of `sub_57B770` (`0x002bec80` in `nox.elf`) shows 8 instructions per float read (`4x ldrb`, `3x orr lsl`, `vmov s14, r3`) and 8 instructions per float write.
- **`port/3ds/CMakeLists.txt:33-47, 86-97`** & **`build-3ds/build.ninja:54, 89..368`**:
  - Disassembly of `sub_496150` (`0x1b0d38`) shows `vcmp.f64 d7, #0.0; vmrs APSR_nzcv, fpscr; bmi <bl sqrt>; vsqrt.f64`.
  - `libctru`'s `__system_initSyscalls` (`0x0031591c`) already sets `FPSCR = 0x03000000` (RunFast mode: Flush-to-Zero, Default-NaN, all FP traps masked).
- **`src/CMakeLists.txt:21`**:
  - `add_compile_options(-g -fno-strict-aliasing -fno-strict-overflow -fshort-wchar -O0)` overrides `CMAKE_C_FLAGS_RELEASE` (`-O2`/`-O3`) on all non-MSVC desktop/Docker builds.

---

## Proposed Fixes
1. **Unpack 4-byte structs in `src/defs.h`:**
   - Wrap `float2`, `float4`, `int2`, `int4`, `shape`, `string`, `wstring`, `struc_13..35` (excluding `pixel888`) in `#pragma pack(push, 4)` ... `#pragma pack(pop)`. Leave `src/windows.h` under `#pragma pack(1)` for `WAVEFORMATEX`/`MPEGLAYER3WAVEFORMAT`.
   - Collapses all 699 `ldrb` cascades to single `vldr.32` / `vstr.32` instructions.
2. **Add `-fno-math-errno -fno-trapping-math` to `port/3ds/CMakeLists.txt`:**
   - Compiles `sqrtf`/`sqrt` directly to single inline `vsqrt.f32` / `vsqrt.f64` with zero `vmrs` stalls, zero `bl sqrt` calls, and zero caller-saved register spills. Do **not** enable `-ffast-math` globally on `GAME*.c`.
3. **Fix `-ftree-vectorize` override & promote pure hand-written TUs:**
   - Change `port/3ds/CMakeLists.txt:96` to `"-O3;-ftree-vectorize;-faggressive-loop-optimizations"` and add `src/string.c`, `src/gamepad.c`, `src/compat.c`, `src/sm.c`, `src/main.cpp` (0 IDA-decompiled functions in those files; keep `GAME1..5.c`, `GAME_ABI.c`, `draw.c`, `imm.c`, `input.c`, `win.c`, `movie.c` at `-O2 -fno-tree-vectorize -fno-aggressive-loop-optimizations`).
4. **Fix Desktop `src/CMakeLists.txt:21`:**
   - Replace hardcoded `-O0` with `$<$<CONFIG:Debug>:-O0>` and `$<$<NOT:$<CONFIG:Debug>>:-O2 -fsigned-char -fno-tree-vectorize -fno-aggressive-loop-optimizations -fno-math-errno -fno-trapping-math>`.

---

## Safety & Verification Notes
- **Alignment Audit:** All 346 `(float2 *)`/`(float4 *)`/`(shape *)` casts in `GAME1..5.c` were scanned and verified 100% 4-byte aligned (0 casts from byte/packet streams), and 0 functions pass/return them by value.
- Verify in `arm-none-eabi-objdump` that `sub_57B770` uses `vldr`/`vstr` and `sub_496150` has 0 `bl <sqrt>` branches; test on Azahar / 3DS hardware.
