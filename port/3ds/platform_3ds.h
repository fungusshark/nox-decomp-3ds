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
typedef enum {
    NOX_3DS_AIM_RADIAL = 0,
    NOX_3DS_AIM_FREE_CURSOR = 1
} nox_3ds_aim_mode_t;

typedef enum {
    NOX_3DS_CSTICK_AIM_CAMERA = 0,
    NOX_3DS_CSTICK_SPELL_CAST = 1
} nox_3ds_cstick_mode_t;

typedef struct {
    bool     is_new_3ds;
    bool     romfs_mounted;
    bool     sdmc_root_ready;
    bool     wide_stretch;        /* false = 320x240 4:3, true = 400x240 */
    bool     bottom_console_mode; /* true in M1-M5, toggleable in M6 */
    bool     touch_to_aim;        /* gameplay upper touchscreen aim toggle */
    nox_3ds_aim_mode_t    aim_mode;    /* radial twin-stick vs free-cursor */
    nox_3ds_cstick_mode_t cstick_mode; /* radial aim+camera vs quick-spell cast */
    uint32_t fps;
    uint32_t linear_free_bytes;
    uint32_t heap_used_bytes;
} nox_3ds_state_t;

bool     nox_3ds_init(void);             /* osSetSpeedupEnable(true), gfx, console, non-fatal romfs, chdir */
void     nox_3ds_shutdown(void);         /* net/video shutdown, romfsExit, gfxExit */
bool     nox_3ds_apt_ok(void);           /* aptMainLoop() wrapper */
uint32_t nox_3ds_get_ticks_ms(void);     /* osGetTime() relative to startup */
uint64_t nox_3ds_get_perf_counter(void); /* svcGetSystemTick() */
uint64_t nox_3ds_get_perf_freq(void);    /* SYSCLOCK_ARM11 (268111856ULL) */
void     nox_3ds_sleep_ms(uint32_t ms);  /* svcSleepThread */
const nox_3ds_state_t *nox_3ds_get_state(void);
nox_3ds_state_t       *nox_3ds_state_mut(void);

/* Normalize '\' -> '/', strip leading 'C:', and resolve relative paths under sdmc:/3ds/nox/. */
void     nox_3ds_resolve_path(const char *path, char *out, size_t out_sz);

/* =========================================================================
 * 3. Software Framebuffer & PICA200 Video Bridge (video_3ds.c)
 * ========================================================================= */
bool  nox_3ds_video_init(int logical_w, int logical_h);
void  nox_3ds_video_shutdown(void);
bool  nox_3ds_video_is_active(void);
void *nox_3ds_linear_alloc(size_t size);
void  nox_3ds_linear_free(void *ptr);
void  nox_3ds_video_set_gamma(float gamma);
void  nox_3ds_video_toggle_wide(void);
void  nox_3ds_video_present(const void *pixels, int w, int h, int pitch,
                            uint32_t sdl_format, bool is_linear);
int   nox_3ds_blit_scaled_16(const void *src_pixels, int sw_surf, int sh_surf, int spitch,
                             bool has_ckey, uint16_t ckey,
                             int sx, int sy, int sw, int sh,
                             void *dst_pixels, int dw_surf, int dh_surf, int dpitch,
                             int dx, int dy, int dw, int dh);

/* =========================================================================
 * 4. Twin-Stick, Touch & Button Input Bridge (input_3ds.c)
 * ========================================================================= */
#define NOX_3DS_KEY_A       (1u << 0)
#define NOX_3DS_KEY_B       (1u << 1)
#define NOX_3DS_KEY_SELECT  (1u << 2)
#define NOX_3DS_KEY_START   (1u << 3)
#define NOX_3DS_KEY_DRIGHT  (1u << 4)
#define NOX_3DS_KEY_DLEFT   (1u << 5)
#define NOX_3DS_KEY_DUP     (1u << 6)
#define NOX_3DS_KEY_DDOWN   (1u << 7)
#define NOX_3DS_KEY_R       (1u << 8)
#define NOX_3DS_KEY_L       (1u << 9)
#define NOX_3DS_KEY_X       (1u << 10)
#define NOX_3DS_KEY_Y       (1u << 11)
#define NOX_3DS_KEY_ZL      (1u << 14)
#define NOX_3DS_KEY_ZR      (1u << 15)
#define NOX_3DS_KEY_TOUCH   (1u << 20)

typedef struct {
    float deadzone;     /* normalized C-Stick deadzone (0.0..0.9, default 0.15) */
    float curve_exp;    /* non-linear exponent tuned for New 3DS nub (default 1.85) */
    float sensitivity;  /* scalar multiplier (default 1.0) */
    float min_radius;   /* inner radial aim radius in 640x480 pixels (default 56.0) */
    float max_radius;   /* outer radial aim/camera look-ahead radius (default 210.0) */
} nox_3ds_cstick_cfg_t;

typedef struct {
    uint32_t keys_held;
    uint32_t keys_down;
    uint32_t keys_up;
    int16_t  cpad_x;   /* -156..+156 */
    int16_t  cpad_y;
    int16_t  cstick_x; /* New 3DS C-Stick nub */
    int16_t  cstick_y;
    bool     touch_active;
    bool     touch_down;
    bool     touch_up;
    uint16_t touch_x;  /* 0..319 */
    uint16_t touch_y;  /* 0..239 */
    bool     in_gameplay;
    bool     select_combo_used;
    nox_3ds_aim_mode_t    aim_mode;
    nox_3ds_cstick_mode_t cstick_mode;
    bool     touch_to_aim;
    nox_3ds_cstick_cfg_t cstick_cfg;
} nox_3ds_input_t;

void nox_3ds_input_init(void);
void nox_3ds_input_poll(nox_3ds_input_t *out);
const nox_3ds_input_t *nox_3ds_input_get(void);
void nox_3ds_input_set_cstick_params(float deadzone, float curve_exp, float sensitivity);
void nox_3ds_input_toggle_aim_mode(void);
void nox_3ds_input_toggle_cstick_mode(void);
void nox_3ds_input_toggle_touch_aim(void);
bool nox_3ds_is_gameplay_active(void);
void nox_3ds_input_process_twin_stick_and_touch(void);

/* =========================================================================
 * 5. In-Memory UDP Loopback Transport (net_loopback_3ds.c, NO socInit)
 * ========================================================================= */
void nox_3ds_net_init(void);
void nox_3ds_net_shutdown(void);
int  nox_3ds_net_socket(int domain, int type, int protocol);
int  nox_3ds_net_closesocket(int fd);
int  nox_3ds_net_bind(int fd, const void *addr, unsigned int addrlen);
int  nox_3ds_net_setsockopt(int fd, int level, int opt, const void *val, unsigned int len);
int  nox_3ds_net_ioctlsocket(int fd, long cmd, unsigned long *argp);
int  nox_3ds_net_sendto(int fd, const void *buf, unsigned int len, int flags,
                        const void *to, unsigned int tolen);
int  nox_3ds_net_recvfrom(int fd, void *buf, unsigned int len, int flags,
                          void *from, unsigned int *fromlen);
int  nox_3ds_net_last_error(void);

/* =========================================================================
 * 6. Synchronization & ARM11 Alignment-Safe Helpers
 * ========================================================================= */
typedef struct nox_3ds_mutex nox_3ds_mutex_t; /* wraps libctru RecursiveLock */
nox_3ds_mutex_t *nox_3ds_mutex_create(void);
void             nox_3ds_mutex_destroy(nox_3ds_mutex_t *m);
void             nox_3ds_mutex_lock(nox_3ds_mutex_t *m);
void             nox_3ds_mutex_unlock(nox_3ds_mutex_t *m);

#ifndef NOX_ALIGN_HELPERS_DEFINED
#define NOX_ALIGN_HELPERS_DEFINED
#if defined(__GNUC__) || defined(__clang__)
#define NOX_ALIGN4  __attribute__((aligned(4)))
#define NOX_ALIGN16 __attribute__((aligned(16)))
#else
#define NOX_ALIGN4
#define NOX_ALIGN16
#endif

static inline uint16_t nox_read_u16(const void *p) { uint16_t v; __builtin_memcpy(&v, p, 2); return v; }
static inline int16_t  nox_read_s16(const void *p) { int16_t v;  __builtin_memcpy(&v, p, 2); return v; }
static inline uint32_t nox_read_u32(const void *p) { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }
static inline int32_t  nox_read_s32(const void *p) { int32_t v;  __builtin_memcpy(&v, p, 4); return v; }
static inline uint64_t nox_read_u64(const void *p) { uint64_t v; __builtin_memcpy(&v, p, 8); return v; }
static inline float    nox_read_f32(const void *p) { float v;    __builtin_memcpy(&v, p, 4); return v; }
static inline double   nox_read_f64(const void *p) { double v;   __builtin_memcpy(&v, p, 8); return v; }
static inline void     nox_write_u16(void *p, uint16_t v) { __builtin_memcpy(p, &v, 2); }
static inline void     nox_write_u32(void *p, uint32_t v) { __builtin_memcpy(p, &v, 4); }
static inline void     nox_write_u64(void *p, uint64_t v) { __builtin_memcpy(p, &v, 8); }
static inline void     nox_write_f32(void *p, float v)    { __builtin_memcpy(p, &v, 4); }
static inline void     nox_write_f64(void *p, double v)   { __builtin_memcpy(p, &v, 8); }
#endif

#ifdef __cplusplus
}
#endif

#endif /* NOX_PLATFORM_3DS_H */
