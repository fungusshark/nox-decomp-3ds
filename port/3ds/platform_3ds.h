#ifndef NOX_PLATFORM_3DS_H
#define NOX_PLATFORM_3DS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 1. Constants & Asset Root
 * ========================================================================= */
#define NOX_3DS_DATA_ROOT       "sdmc:/3ds/nox"
#define NOX_3DS_LOGICAL_W       640
#define NOX_3DS_LOGICAL_H       480
#define NOX_3DS_TEX_W           1024
#define NOX_3DS_TEX_H           512
#define NOX_3DS_TOP_W           400
#define NOX_3DS_TOP_H           240
#define NOX_3DS_BOT_W           320
#define NOX_3DS_BOT_H           240
#define NOX_3DS_HEAP_CAP_BYTES  (110u * 1024u * 1024u)

/* =========================================================================
 * 2. Hardware Lifecycle, Paths & Diagnostics (platform_3ds.c)
 * ========================================================================= */
typedef struct {
    bool     is_new_3ds;
    bool     romfs_mounted;
    bool     sdmc_root_ready;
    bool     wide_stretch;        /* false = 320x240 4:3, true = 400x240 */
    bool     bottom_console_mode; /* true in M1-M5, toggleable in M6 */
    uint32_t fps;
    uint32_t linear_free_bytes;
    uint32_t heap_used_bytes;
} nox_3ds_state_t;

bool     nox_3ds_init(void);             /* osSetSpeedupEnable(true), gfx, console, non-fatal romfs, chdir */
void     nox_3ds_shutdown(void);         /* romfsExit (if mounted), gfxExit */
bool     nox_3ds_apt_ok(void);           /* aptMainLoop() wrapper */
uint32_t nox_3ds_get_ticks_ms(void);     /* osGetTime() relative to startup */
uint64_t nox_3ds_get_perf_counter(void); /* svcGetSystemTick() */
uint64_t nox_3ds_get_perf_freq(void);    /* SYSCLOCK_ARM11 (268111856ULL) */
void     nox_3ds_sleep_ms(uint32_t ms);  /* svcSleepThread */
const nox_3ds_state_t *nox_3ds_get_state(void);

/* Normalize '\' -> '/', strip leading 'C:', and resolve relative paths under sdmc:/3ds/nox/. */
void     nox_3ds_resolve_path(const char *path, char *out, size_t out_sz);

/* =========================================================================
 * 3. Synchronization & ARM11 Alignment-Safe Helpers
 * ========================================================================= */
typedef struct nox_3ds_mutex nox_3ds_mutex_t; /* wraps libctru RecursiveLock */
nox_3ds_mutex_t *nox_3ds_mutex_create(void);
void             nox_3ds_mutex_destroy(nox_3ds_mutex_t *m);
void             nox_3ds_mutex_lock(nox_3ds_mutex_t *m);
void             nox_3ds_mutex_unlock(nox_3ds_mutex_t *m);

static inline uint16_t nox_read_u16(const void *p) { uint16_t v; __builtin_memcpy(&v, p, 2); return v; }
static inline uint32_t nox_read_u32(const void *p) { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }
static inline float    nox_read_f32(const void *p) { float v;    __builtin_memcpy(&v, p, 4); return v; }
static inline void     nox_write_u16(void *p, uint16_t v) { __builtin_memcpy(p, &v, 2); }
static inline void     nox_write_u32(void *p, uint32_t v) { __builtin_memcpy(p, &v, 4); }
static inline void     nox_write_f32(void *p, float v)    { __builtin_memcpy(p, &v, 4); }

#ifdef __cplusplus
}
#endif

#endif /* NOX_PLATFORM_3DS_H */
