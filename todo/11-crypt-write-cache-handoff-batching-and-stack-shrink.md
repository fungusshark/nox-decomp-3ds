# perf(3ds,io): connect g_raw_write_cache to crypt reads, batch 8-byte crypt writes, and shrink main stack 1 MB -> 128 KB

**Priority:** `P2`  
**Labels:** `performance`, `3ds`, `io`, `memory`

---

## Summary
1. **`g_raw_write_cache` Handoff Gap (`fs_3ds.c`):** After `sub_57BC50` extracts `<map>.nxz` into `Save\TEMP\<map>\<map>.map` and caches the raw bytes in `g_raw_write_cache`, `crypt_reader_promote_full()` (`"rb"`) never checks `g_raw_write_cache` — re-reading 1–3 MB from SDMC while simultaneously holding duplicate buffers in FCRAM! Also `memfile_close()` flushes clean `!mf->dirty` `"r+b"` opens back to SDMC.
2. **8-Byte Crypt Writes & Uncached Key Expansion (`GAME1.c`):** `sub_409200` calls `nox_3ds_blowfish_crypt(..., 8)` + `fwrite(..., 8, 1)` ~200,000 times for a 1.6 MB `.map` save; `sub_409580` re-runs 521 Blowfish key-expansion rounds on every write/header open; `sub_426BD0` stores CRC32 to global `byte_5D4594[739996]` every byte.
3. **Massive Stack Frames Pinning `__stacksize__ = 1 MB`:** `sub_4DBE10` (`GAME3.c:44014`, 146 KB stack) lacks a `#ifdef __3DS__` streaming `opendir`/`unlink` path (unlike `sub_4DC100`), and `sub_51F230` (`GAME4.c:34862`) allocates 75 KB on stack.
4. **Loopback Net Ring & `nox.csf` / Path Churn:** `net_loopback_3ds.c` eagerly allocates 65.8 KB per socket; `nox_3ds_resolve_path` calls `getcwd()` + recursive char-by-char `sanitize_slashes_recursive()` 3× per crypt open; `sub_40F280` does ~50k tiny reads + ~35k `calloc`s for `nox.csf`.

---

## Exact Locations
- `port/3ds/fs_3ds.c:466-480, 571-727, 886, 1397-1613`
- `src/GAME1.c:7993-8254` (`sub_409200`, `sub_409470`, `sub_409580`), `13702-13851` (`sub_40F280` `nox.csf`), `34805-34827` (`sub_426BD0`)
- `src/GAME5.c:16861-16920` (`sub_57BC50` `.nxz` extract)
- `src/GAME3.c:44014-44148` (`sub_4DBE10` 146 KB stack) & `src/GAME4.c:34862-35139` (`sub_51F230` 75 KB stack), `31261-31285` (`sub_51A960` 3 KB per-tick stack)
- `port/3ds/platform_3ds.c:20` (`__stacksize__`)
- `port/3ds/net_loopback_3ds.c:184, 292-480`

---

## Proposed Fixes
1. Check `raw_write_cache_lookup(resolved)` in `crypt_reader_promote_full()`, `crypt_reader_ensure_prefix()`, and `nox_3ds_stat()`; guard `memfile_flush_to_sd()` in `memfile_close()` with `if (mf->dirty)`; `free(v7)` before `fopen("wb")` in `sub_57BC50`.
2. Batch `sub_409200` aligned Blowfish writes into a 4 KB stack buffer (~500x fewer calls), memoize expanded Blowfish `(P_enc, P_dec, S)` per `key_id`, and accumulate CRC32 locally in `sub_426BD0`.
3. Replace `sanitize_slashes_recursive()` with an iterative loop, cache CWD components until `chdir`/`SetCurrentDirectoryA`, replace `snprintf` segment loop with `memcpy`, and coalesce `sub_408CC0` path resolution.
4. Add `nox_3ds_save_rm()` (`opendir`/`unlink`) in `sub_4DBE10`, move `sub_51F230`'s 75 KB arrays off-stack (`malloc`/`static`), make `sub_51A960` `v8[3072]` `static`, and lower `__stacksize__` from `1 MB` -> `128 KB` (reclaiming **~896 KB RAM**).
5. Switch `net_loopback_3ds.c` to lazy 16 KB variable-length byte rings + lock-free atomic empty check on `FIONREAD`/`recvfrom`, and single-buffer `nox.csf` loading in `sub_40F280`.
