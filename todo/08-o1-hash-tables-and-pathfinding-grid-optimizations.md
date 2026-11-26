# perf(sim,script): add O(1) hash tables for extent/script_id/netcode/video.idx/spells and optimize monster pathfinding grid

**Priority:** `P1`  
**Labels:** `performance`, `simulation`, `noxscript`, `pathfinding`

---

## Summary
Critical engine, NoxScript, and asset lookups perform O(N) linear linked-list or `strcmp` scans over hundreds to thousands of records, and monster pathfinding performs uninformed BFS flood-fills while re-querying static floor tiles 4 times per expanded cell.

---

## Exact Locations
- `src/GAME3.c:57855-58185` (`sub_4ECCB0`, `sub_4ECD90`, `sub_4ECE50`, `sub_4ED020`)
- `src/GAME4.c:10278-10340` (`sub_502670` — 33 `Group*` callers calling `sub_4ED020`), `22709-22920` (`sub_511B60`, `sub_511C50`, `sub_511D90` — 125 NoxScript callers)
- `src/GAME4.c:17052-18248` (`sub_50AFA0` 768 KB memset, `sub_50B2C0`, `sub_50B520`, `sub_50BA00` BFS, `sub_50C830` 4x `sub_411160`)
- `src/GAME4.c:28505-29050` (`sub_517560` monster def, `sub_517AE0` 70-ptr spatial grid), `54972-55050` (`sub_5375A0`, `sub_537610` uncached `sub_4E3AA0`)
- `src/GAME4.c:24391-24405` (`sub_513490`), `14781, 14895` (`sub_507310`), `46514-46551` (`sub_52CCD0` `strcmp("SentryGlobe")`)
- `src/GAME1.c:30266-30323` (`sub_4217B0(pt, 0)` polygon linear scan), `32330-32605` (`sub_4243F0` spells, `sub_424880` enchants), `41599-41645` (`sub_42F970`/`sub_42FA20` `video.idx` 104-byte stride `strcmp`)
- `src/GAME5.c:15105-15300` (`sub_579C40`, `sub_579E30` waypoints), `17087-17101` (`sub_57C0A0` map groups)

---

## Proposed Fixes
1. **O(1) Open-Addressed Object ID Hash Tables (`extent`, `script_id`, `netcode`):**
   - Back `sub_4ED020` (currently 0-entry cache!), `sub_511B60`, and `sub_4ECCB0` (currently thrashing 16-entry LRUs) with 8192-slot open-addressed hash tables hooked into object init (`sub_4DAA50`) and destruction (`sub_511D90`, `sub_4ECE50`).
2. **O(1) Name / Static Table Hash Maps:**
   - Hash `video.idx` sprite/anim names in `sub_42F970`/`sub_42FA20` (preserving lowest-index match for duplicates), spells (`sub_4243F0`), enchantments (`sub_424880`), waypoints (`sub_579C40`/`sub_579E30`), map groups (`sub_57C0A0`), and `monster_def_by_tt[thing_type]` (`sub_517560`).
   - Cache the two uncached `sub_4E3AA0` calls in `sub_537610` (`GAME4.c:55020-55021`), cache `"SentryGlobe"` ThingType in `sub_52CCD0`, and add `v27 == v28` before `strcmp` in `sub_507310`.
3. **Pathfinding, Spatial Grid & Polygon Lookups:**
   - Precompute `sub_50C830` static floor walkability once during `sub_50AFA0` into an unused bit (`0x80`) of the path grid.
   - Maintain a compact list of dynamic path-blocking objects (`doors/destructibles`) so `sub_50B520` walks ~50 blockers every 15 ticks instead of 3,000+ world objects.
   - Flatten `sub_517AE0`'s `70x70` spatial grid into one contiguous `78.4 KB` block, bucket map polygons into a coarse grid for `sub_4217B0(pt, 0)`, and optionally upgrade `sub_50BA00` FIFO BFS to an octile-heuristic A\* priority queue.
