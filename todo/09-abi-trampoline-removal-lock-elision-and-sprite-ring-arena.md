# perf(abi,cache): collapse abi_wrappers VFP ping-pongs, elide single-thread RecursiveLocks, fix mallinfo() getter, and add variable-size video.bag ring arena

**Priority:** `P1`  
**Labels:** `performance`, `abi`, `cache`, `3ds`

---

## Summary
1. **`abi_wrappers.c` + `GAME_ABI.c` VFP Ping-Pongs:** Every monster update (`sub_50A5C0`) and spell tick calls a trampoline in `abi_wrappers.o` that executes `vmov s0, r0; b *__abi_raw`, and the callee immediately executes `vmov r4, s0` (~15–20 cycle GPR↔VFP stall + ~1 MB `.text` jump). Plus 5 prototypes in `src/proto.h` still declare `(float)` instead of `(void *)`.
2. **Uncontested `RecursiveLock` Overhead:** Despite 100% single-threaded CPU execution on 3DS, `EnterCriticalSection` / `nox_3ds_mutex_lock` execute 4 cross-TU calls + CP15 TLS `mrc` + `ldrex/strex` + `dmb` barriers.
3. **`mallinfo()` Landmine in `nox_3ds_get_state()`:** Walks the entire 80–110 MB `dlmalloc` heap free list on every call even though callers only inspect `.is_new_3ds`.
4. **`video.bag` Fixed Slab Waste (`sub_42EE30`):** Allocates every cache slot at `max_segment_size` (~65 KB), wasting 75–95% of FCRAM on small 4 KB sprites (~256 slots in 16 MB, ~64 slots on Old 3DS 4 MB).

---

## Exact Locations
- `abi_wrappers.c:25-341`, `src/GAME_ABI.c:264-3837`, `src/GAME5.c:3317` (`sub_549380__abi_raw`), `src/GAME2.c:48968` (`sub_48EA70__abi_raw`), `src/proto.h:5315, 6564, 6618, 7076, 7078`
- `port/3ds/platform_3ds.c:231-269` (`nox_3ds_get_state`, `nox_3ds_mutex_*`) & `src/compat.c:306, 2618-2636`
- `src/GAME1.c:40944-41023, 41797-41884` (`sub_42EE30`, `sub_42FE30`, `get_bag`)

---

## Proposed Fixes
1. **Collapse `abi_wrappers.c`:** Define public `(void *)` entry points directly in `GAME_ABI.c` (`sub_549380` in `GAME5.c`, `sub_48EA70` in `GAME2.c`), update the 5 `(float)` prototypes in `src/proto.h` to `(void *)`, and retire `libabi_wrappers.a`.
2. **Single-Thread Lock Fast-Path:** Guard `_beginthread` in `compat.c:306` on `#ifdef __3DS__` (unless audio worker thread is enabled; if audio worker thread is enabled, keep `audio_3ds.c` using real `RecursiveLock` and only elide `EnterCriticalSection` / `net_loopback` locks!).
3. **O(1) `nox_3ds_get_state()`:** Remove `linearSpaceFree()` and `mallinfo()` from `nox_3ds_get_state()`; move them to an explicit `nox_3ds_refresh_mem_stats()` called at boot/load/1 Hz diagnostics.
4. **Variable-Size Circular Arena in `sub_42EE30` / `sub_42FE30`:** Pack decompressed sprites sequentially (`(size + 15) & ~15`) in a non-moving circular buffer, evicting oldest LRU segments on wrap — fitting 3–5x more sprites in the same 16 MB / 4 MB budget without moving live pointers.
