# perf(sim): disable dead O(N^2) 1999 anti-cheat shuffler, fix 50%-frame 3,000-object visibility walk, and drop 2 KB per-poll memset

**Priority:** `P0`  
**Labels:** `performance`, `simulation`, `cache`

---

## Summary
Core simulation ticks waste substantial ARM11 CPU and 32 KB L1 D-cache bandwidth on three completely avoidable overheads:
1. **`sub_56F5C0` (`O(N²)` anti-cheat linked-list shuffler)** runs every server tick and every protected stat write (HP/mana/gold/armor), only to call `nullsub_31(1)` (a literal no-op) on failure.
2. **`sub_518EE0`** runs on **50% of all frames** (`tick & 8`) even in Solo mode, walking all 1,000–4,000 map objects + missiles and executing `sub_4173D0` + 4 `double` bounds comparisons *before* testing the `v14 & *(k + 148)` bitmask (which is set on only ~10–40 objects).
3. **`sub_40ED60`** executes `memset(..., 0, 2048)` on every `recvfrom` poll before checking whether a packet arrived.

---

## Exact Locations
- `src/GAME5.c:13392-13620` (`sub_56F5C0` at `13417`, `sub_56F590` at `13392`, `sub_56F6F0` at `13482`, `sub_56F780..56F870`)
- `src/GAME3.c:36348, 36399, 50191-50204` (`sub_4D2580` callers & `sub_4E3C20` `thing.bin` checksum loop)
- `src/GAME4.c:30124-30161` (`sub_518EE0`, called from `sub_4DEC80` at `GAME3.c:46051`)
- `src/GAME1.c:13370-13411` (`sub_40ED60`, `memset` at `13381`)

---

## Proposed Fixes
1. **Short-circuit `sub_56F5C0()` & `sub_4E3C20()`:**
   - Return immediately from `sub_56F5C0()` and `sub_4E3C20()` (if `.plr` demo replay parity is active via `byte_5D4594[2650636] & 0x600000`, advance `byte_5D4594[371260]` RNG index or fall back).
   - Replace `sub_56F590` O(N) linked-list search in `sub_56F360..sub_56F870` with flat/direct storage.
2. **Fix `sub_518EE0` (`GAME4.c:30135-30148`):**
   - Immediate 1-line win: check `(v14 & *(_DWORD *)(v16 + 148))` **first** in the `if` chain before `sub_4173D0` and the 4 `double` bounds checks.
   - Complete O(Visible) fix: maintain a compact per-player visible-object list updated in `sub_417290` / `sub_4172C0` / unit destroy so `sub_518EE0` iterates ~20–40 visible objects instead of 3,000+ world objects.
3. **Remove 2 KB `memset` in `sub_40ED60` (`GAME1.c:13381`):**
   - Remove `memset(&byte_5D4594[207988], 0, 0x800u)` and null-terminate only at `v6` (`v6 >= 0 ? v6 : 0`).

---

## Safety Notes
- `nullsub_31` is an empty stub; `sub_40ED60` callers (`sub_4E66B0` / `sub_48EA70`) bound reads strictly by `*a3` (`v6`).
- Check `sub_417290` / `sub_4172C0` for all mutation sites of `*(k + 148)` before switching `sub_518EE0` to a tracked visible list.
