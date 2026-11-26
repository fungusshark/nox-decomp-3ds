# perf(3ds,audio): offload stream decode/SDMC I/O to Core 2/1, expand audio.bag cache on N3DS to unlock zero-copy, and remove 64 KB setvbuf read amplification

**Priority:** `P1`  
**Labels:** `performance`, `3ds`, `audio`, `multicore`, `io`

---

## Summary
Cores 1 and 2 on New 3DS sit 100% idle while `AIL_serve()` runs synchronously on Core 0, performing SDMC `fread()`s, IMA-ADPCM decoding, and scalar-float `minimp3.h` MP3 decoding on the main gameplay thread (`AIL_start_stream()` primes all 4 buffers ~360 ms in one frame!). Meanwhile, `audio.bag` uses a 1999-era 1 MB / 8 KB cache that defeats `sub_43EE00`'s 16 KB zero-copy path, and `setvbuf(..., 65536)` causes 64 KB SDMC read amplification on small random SFX `fseek`s.

---

## Exact Locations
- `port/3ds/platform_3ds.c:59-130` (missing `APT_SetAppCpuTimeLimit(30)` & worker threads)
- `port/3ds/audio_3ds.c:844-865` (`stream_adpcm_rewind_for_loop`), `899-951` (`stream_adpcm_decode`), `1060-1135` (`stream_mp3_decode`), `1328-1492` (`stream_work`, `pump_driver`, `AIL_serve`), `1951-2348` (`AIL_set_sample_*`, `AIL_start_stream`, `AIL_stream_status`)
- `src/compat_mss.c:120, 685, 801-829, 1528` (desktop `64 KB` per-stream seek buf, `memmove`, static work buffer)
- `src/GAME2.c:3210` (`sub_4BD340(a3, 0x100000, 200, 0x2000)`), `43823` (`setvbuf` 64 KB on `audio.bag`), `44014` (`sub_486B60`)
- `src/GAME3.c:19527-19665` (`sub_4BD340`, O(N) `sub_4BD420`)
- `src/GAME1.c:40959` (`setvbuf` 64 KB on `video.bag`), `52880-52881` (commented-out `AIL_release_sample_handle`), `53030-53076` (`sub_43EE00` zero-copy check `>= 0x4000`)

---

## Proposed Fixes
1. **Multicore Stream Decode/I/O Offload (Core 2 on N3DS, Core 1 fallback):**
   - Call `APT_SetAppCpuTimeLimit(30)` in `nox_3ds_init()`.
   - Spawn a stream refill thread (`stack >= 32 KB` for `mp3dec_scratch_t`, `cpu_id = is_new_3ds ? 2 : 1`) woken by `ndspSetCallback` / `LightEvent` to run `_STREAM` PCM filling (`fread` + ADPCM/MP3 decode + `DSP_FlushDataCache` + `ndspChnWaveBufAdd`), while keeping `dispatch_timers_sync()` and EOB/EOS engine callbacks strictly on Core 0 inside `AIL_serve()`. Prevents music dropouts during `swkbdInputText` too.
   - Cache `data_offset` at open time so `stream_adpcm_rewind_for_loop()` is a single `fseek` instead of a RIFF chunk walk, and prime only 1 buffer on `AIL_start_stream()`.
2. **Unlock `audio.bag` Zero-Copy & Eliminate 64 KB Read Amplification:**
   - On New 3DS, raise `GAME2.c:3210` from `(a3, 0x100000, 200, 0x2000)` (`1 MB` / `8 KB`) to `(a3, 0x400000, 512, 0x4000)` (`4 MB` / `16 KB`), unlocking `sub_43EE00`'s `>= 0x4000` zero-copy fast path and increasing cached SFX capacity 2–3x.
   - Index `sub_4BD420` by `sound_id` instead of an O(N) linked-list walk.
   - Replace `setvbuf(..., _IOFBF, 65536)` on random-access `audio.bag` (`GAME2.c:43823`) and `video.bag` (`GAME1.c:40959`) with `_IONBF` (or `4096`) to stop 64 KB SDMC over-reads on small random `fseek`s.
3. **Eliminate Per-MP3-Frame `16 KB` `memmove` & Coalesce `ndsp` Mix Updates:**
   - Add `size_t buf_pos` read cursor to `struct _STREAM` (`audio_3ds.c` & `compat_mss.c`), compacting `< 2048` bytes only when exhausted (~95% less `memmove` + batched `fread`).
   - Isolate `minimp3.h` in a `-O3 -ffast-math` TU (`mp3_decode.c`).
   - Early-return unchanged values in `AIL_set_sample_volume`/`pan`/`playback_rate` and set `S->mix_dirty`/`rate_dirty` so `pump_driver()` calls `ndspChnSetMix` once per tick instead of twice; uncomment `AIL_release_sample_handle` at `GAME1.c:52881`.
