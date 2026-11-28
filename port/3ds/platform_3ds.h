#ifndef NOX_PLATFORM_3DS_H
#define NOX_PLATFORM_3DS_H

/*
 * src/proto.h wraps the entire engine header include chain in #pragma pack(push, 1).
 * Reset packing to natural ARM EABI alignment inside this bridge header so structs
 * shared between src/*.c and port/3ds/*.c ALWAYS have identical binary layouts.
 */
#pragma pack(push)
#pragma pack()

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

struct stat;

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

/* Centered 5:3 gameplay viewport (640x384) inside 640x480 logical space.
 * Hardware PICA200 downscales 640x384 -> 400x240 via GPU_LINEAR for full
 * horizontal FOV with exact 5:3 aspect ratio. Center stays (320, 240). */
#define NOX_3DS_VP_W            640
#define NOX_3DS_VP_H            384
#define NOX_3DS_VP_LEFT         ((NOX_3DS_LOGICAL_W - NOX_3DS_VP_W) / 2)  /* 0   */
#define NOX_3DS_VP_TOP          ((NOX_3DS_LOGICAL_H - NOX_3DS_VP_H) / 2)  /* 48  */
#define NOX_3DS_VP_RIGHT        (NOX_3DS_VP_LEFT + NOX_3DS_VP_W - 1)      /* 639 */
#define NOX_3DS_VP_BOTTOM       (NOX_3DS_VP_TOP  + NOX_3DS_VP_H - 1)      /* 431 */

/* Near-black charcoal RGB555 backdrop for Bottom Screen UI (blends cleanly
 * with the HUD and bezel while keeping sub_49CF10 darkened panels visible). */
#define NOX_3DS_BOT_BG_RGB555   ((uint16_t)((2u << 10) | (2u << 5) | 2u))

/* C-Stick aiming geometry (in 640x384 Top Screen logical pixels; 1.6x physical) */
#define NOX_3DS_AIM_CLOSE_RADIUS    24.0f /* ~15px physical tight radius around hero */
#define NOX_3DS_FREE_AIM_SPEED      18.0f /* max px/frame when L ranged aim held */
#define NOX_3DS_AIM_FRICTION_MIN    0.42f /* min speed scale when reticle crosses hostile */
#define NOX_3DS_AIM_FRICTION_RADIUS 34.0f /* world-px hostile friction bubble around reticle */

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

typedef enum {
    NOX_3DS_GUI_ALL     = 0, /* Render all root GUI windows */
    NOX_3DS_GUI_BG_ONLY = 1, /* Render ONLY MainBG.wnd (Main Menu art -> Top) */
    NOX_3DS_GUI_NO_BG   = 2  /* Render all windows except MainBG.wnd (-> Bottom) */
} nox_3ds_gui_filter_t;

typedef struct {
    bool     is_new_3ds;
    bool     romfs_mounted;
    bool     sdmc_root_ready;
    bool     dsp_audio_ready;     /* true if sdmc:/3ds/dspfirm.cdc present and ndspInit() OK */
    bool     soc_ready;           /* true once lazy socInit() succeeded on Multiplayer entry */
    bool     httpc_ready;         /* true once lazy httpcInit() succeeded for OpenNox lobby */
    bool     wide_stretch;        /* top screen: true = fill 400x240, false = preserve aspect (4:3 320x240) */
    bool     bottom_console_mode; /* true during boot before citro2d takes GFX_BOTTOM */
    bool     touch_to_aim;        /* false in dual-screen mode (touch is strict bottom UI) */
    nox_3ds_aim_mode_t    aim_mode;
    nox_3ds_cstick_mode_t cstick_mode;
    uint32_t fps;
    uint32_t linear_free_bytes;
    uint32_t heap_used_bytes;
} nox_3ds_state_t;

bool     nox_3ds_init(void);             /* osSetSpeedupEnable(true), gfx, boot console, romfs, chdir */
void     nox_3ds_console_detach(void);   /* hand GFX_BOTTOM from boot console to citro2d */
void     nox_3ds_shutdown(void);         /* net/video shutdown, console exit prompt, romfsExit, gfxExit */
bool     nox_3ds_apt_ok(void);           /* aptMainLoop() wrapper */
uint32_t nox_3ds_get_ticks_ms(void);     /* osGetTime() relative to startup */
uint64_t nox_3ds_get_perf_counter(void); /* svcGetSystemTick() */
uint64_t nox_3ds_get_perf_freq(void);    /* SYSCLOCK_ARM11 (268111856ULL) */
void     nox_3ds_sleep_ms(uint32_t ms);  /* svcSleepThread */
const nox_3ds_state_t *nox_3ds_get_state(void);
nox_3ds_state_t       *nox_3ds_state_mut(void);
static inline bool     nox_3ds_is_new_3ds(void) { return nox_3ds_get_state()->is_new_3ds; }

/* Single-precision VFPv2 hardware square root for known non-negative inputs (x >= 0.0f).
 * Avoids double-precision promotion (vsqrt.f64 + __aeabi_d2lz) and libm errno branches. */
static inline float nox_3ds_vsqrtf_pos(float x)
{
#if defined(__arm__) && defined(__VFP_FP__)
    float r;
    __asm__ ("vsqrt.f32 %0, %1" : "=t"(r) : "t"(x));
    return r;
#else
    return __builtin_sqrtf(x);
#endif
}

/* =========================================================================
 * 2b. SD Card File System & Persistence Hooks (fs_3ds.c)
 * ========================================================================= */
/* Recursively convert '\' -> '/' and collapse duplicate slashes in-place. */
void     nox_3ds_sanitize_path(char *path);
/* Normalize '\' -> '/', strip leading 'C:', and redirect all game paths to sdmc:/3ds/nox/. */
void     nox_3ds_resolve_path(const char *path, char *out, size_t out_sz);
/* Create any missing parent directories on SDMC via mkdir() before writing. */
bool     nox_3ds_ensure_parent_dirs(const char *resolved_path);
/* Create a directory (and any missing parents) under sdmc:/3ds/nox/. */
bool     nox_3ds_ensure_dir(const char *path);
/* Redirected, sanitized fopen() with pre-write mkdir and write-handle tracking. */
FILE    *nox_3ds_fopen(const char *path, const char *mode);
/* Flushes writable stdio buffers without per-file fsync() thrashing, then fclose(). */
int      nox_3ds_fclose(FILE *stream);
/* Redirected, sanitized stat() wrapper for SDMC paths. */
int      nox_3ds_stat(const char *path, struct stat *st);
/* Flush + fsync all tracked open writable streams to SDMC. */
void     nox_3ds_fs_sync_all(void);
/* Batch save committer: flushes all stdio buffers + open descriptors once per save event. */
void     nox_3ds_commit_save(void);
/* Direct zero-chdir save-slot copier (replaces sub_4DBE10+sub_4DC100 IPC churn on 3DS). */
int      nox_3ds_save_copy(const char *src_slot, const char *dst_slot);

/* =========================================================================
 * 2c. Fast Blowfish & RAM-Cached Read-Only Cryptfile Reader (fs_3ds.c)
 * ========================================================================= */
/* Fast -O3 Blowfish block transform matching sub_409860/sub_4097C0/sub_4099C0.
 * P: 18 uint32_t subkeys (&byte_5D4594[2659820] enc / [2659916] dec)
 * S: 1024 uint32_t S-box entries (&byte_5D4594[2655724]) */
int      nox_3ds_blowfish_crypt(uint8_t *dst, const uint8_t *src, int len,
                                const uint32_t *P, const uint32_t *S);
FILE    *nox_3ds_crypt_try_open_cached(const char *path);
bool     nox_3ds_crypt_is_virtual(FILE *fp);
bool     nox_3ds_crypt_cache_has(const char *resolved_path);
void     nox_3ds_crypt_on_open(FILE *fp, const char *path, int mode);
int      nox_3ds_crypt_try_cache(FILE *fp, int key_id);
void     nox_3ds_crypt_set_promote_ctx(FILE *fp,
                                       const uint32_t *P_dec, const uint32_t *S,
                                       void (*pump_cb)(void));
bool     nox_3ds_crypt_arm_reader(FILE *fp, int key_id,
                                  const uint32_t *P_dec, const uint32_t *S,
                                  void (*pump_cb)(void));
bool     nox_3ds_crypt_arm_raw(FILE *fp);
bool     nox_3ds_crypt_read(FILE *fp, void *dst, int elem_size, int count,
                            int *out_pos, int *out_eof, int *out_ret);
bool     nox_3ds_crypt_seek(FILE *fp, int offset, int whence,
                            int *out_pos, int *out_eof);
bool     nox_3ds_crypt_close(FILE *fp);
void     nox_3ds_crypt_reset(void);
void     nox_3ds_crypt_cache_invalidate(const char *path);
void     nox_3ds_crypt_cache_clear(void);

/* Lightweight campaign load-phase timing log (buffered in RAM, flushed on demand). */
void     nox_3ds_load_log(const char *fmt, ...);
void     nox_3ds_load_log_flush(void);

/* =========================================================================
 * 3. Dual-Screen Software Framebuffer & PICA200 Video Bridge (video_3ds.c & draw.c)
 * ========================================================================= */
typedef struct {
    const void *pixels;
    int         w;
    int         h;
    int         pitch;
    uint32_t    sdl_format;
    int         crop_x;       /* source sub-rect inside pixels (0,0,w,h = full) */
    int         crop_y;
    int         crop_w;
    int         crop_h;
    bool        is_linear;
    bool        fill_screen;  /* true = fill full target; false = letterbox aspect */
    bool        reuse_tex;    /* true = skip cache flush + GX upload, redraw existing VRAM tex */
} nox_3ds_screen_frame_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(nox_3ds_screen_frame_t, crop_x) == 20, "ABI pack guard failed on nox_3ds_screen_frame_t");
_Static_assert(sizeof(nox_3ds_screen_frame_t) == 40, "Unexpected sizeof(nox_3ds_screen_frame_t)");
#endif

bool  nox_3ds_video_init(int logical_w, int logical_h);
void  nox_3ds_video_shutdown(void);
bool  nox_3ds_video_is_active(void);
void *nox_3ds_linear_alloc(size_t size);
void  nox_3ds_linear_free(void *ptr);
void  nox_3ds_video_set_gamma(float gamma);
void  nox_3ds_video_toggle_wide(void);
void  nox_3ds_video_present_dual(const nox_3ds_screen_frame_t *top,
                                 const nox_3ds_screen_frame_t *bottom);
void  nox_3ds_video_present(const void *pixels, int w, int h, int pitch,
                            uint32_t sdl_format, bool is_linear);
void  nox_3ds_video_present_movie(const void *pixels, int w, int h, int pitch,
                                  uint32_t sdl_format, bool is_linear,
                                  int crop_x, int crop_y, int crop_w, int crop_h);
int   nox_3ds_blit_scaled_16(const void *src_pixels, int sw_surf, int sh_surf, int spitch,
                             bool has_ckey, uint16_t ckey,
                             int sx, int sy, int sw, int sh,
                             void *dst_pixels, int dw_surf, int dh_surf, int dpitch,
                             int dx, int dy, int dw, int dh);

/* Dual-screen software buffer & GUI split helpers (implemented in src/draw.c) */
#define NOX_3DS_SWBUF_TOP       0  /* primary 640x480 surface -> Top Screen (world)   */
#define NOX_3DS_SWBUF_BOTTOM    1  /* secondary 640x480 surface -> Bottom Screen (UI) */
int  nox_3ds_swbuf_select(int target);   /* fast pointer swap, returns previous target */
void nox_3ds_swbuf_begin_bottom(uint16_t clear_rgb555);
void nox_3ds_swbuf_resolve_bottom_occlusion(void);
void nox_3ds_swbuf_end_bottom(void);
void nox_3ds_swbuf_invalidate_bottom(void);
bool nox_3ds_swbuf_reuse_bottom(void);
bool nox_3ds_is_bottom_swbuf_active(void);
void nox_3ds_set_top_render_active(bool active);
bool nox_3ds_is_top_render_active(void);
bool nox_3ds_font_metrics_use_bottom(void);
void nox_3ds_set_top_crop(int x, int y, int w, int h, bool fill_screen);
void nox_3ds_blit_cursor_to_top(void);
void nox_3ds_blit_cursor_to_bottom(void);
void nox_3ds_set_gui_filter(nox_3ds_gui_filter_t filter);
nox_3ds_gui_filter_t nox_3ds_get_gui_filter(void);
void nox_3ds_set_world_cursor_state(int cursor_id, const void *tooltip_wstr);
int  nox_3ds_get_world_cursor_id(void);
const void *nox_3ds_get_world_tooltip(void);
void nox_3ds_capture_top_splash(void);
bool nox_3ds_has_top_splash(void);
bool nox_3ds_present_top_splash(void);
void nox_3ds_skip_default_cursor_blit(void);

/* Native 1:1 PICA200/citro2d Pixel Font Renderer (port/3ds/font_3ds.c) */
bool nox_3ds_font_init(void);
void nox_3ds_font_upload_pending(void);
void nox_3ds_font_shutdown(void);
void nox_3ds_font_begin_slot(int font_slot, int src_h, int line_pitch);
void nox_3ds_font_add_glyph(int font_slot, uint16_t ch, int src_w, const uint8_t *bits, int row_stride, int src_h);
void nox_3ds_font_finish_slot(int font_slot);
bool nox_3ds_font_is_enabled(void);
void nox_3ds_font_set_enabled(bool enabled);
void nox_3ds_font_toggle(void);
uint32_t nox_3ds_font_generation(void);
void nox_3ds_font_push_shadow(bool shadow);
void nox_3ds_font_pop_shadow(bool shadow);
void nox_3ds_font_push_fallback(bool force_software);
void nox_3ds_font_pop_fallback(bool force_software);
bool nox_3ds_font_fits_single(int font_slot, const uint16_t *wstr, int max_w, int tab_w);
bool nox_3ds_font_fits_multiline(int font_slot, const uint16_t *wstr, int max_w, int max_h, int tab_w);
int  nox_3ds_font_slot_from_ptr(int font_ptr);
void nox_3ds_font_clear_screen(int screen);
void nox_3ds_font_clear_all(void);
int  nox_3ds_font_glyph_width_logical(int font_slot, uint16_t ch, bool is_bottom);
int  nox_3ds_font_line_height_logical(int font_slot, bool is_bottom);
bool nox_3ds_font_enqueue_glyph(int screen, int font_slot, uint16_t ch,
                                int x, int y, uint16_t rgb555, bool want_shadow,
                                bool clip_enabled, int clip_l, int clip_t, int clip_r, int clip_b,
                                uint16_t **sw_rows, int sw_w, int sw_h,
                                int *out_logical_advance);
void nox_3ds_font_resolve_occlusion(int screen, uint16_t **sw_rows, int sw_w, int sw_h);
void nox_3ds_font_render_screen(int screen,
                                int crop_x, int crop_y, int crop_w, int crop_h,
                                float draw_x, float draw_y, float dst_w, float dst_h,
                                float gamma);
void nox_3ds_font_set_show_version(bool show);
bool nox_3ds_debug_hud_enabled(void);
void nox_3ds_ui_queue_scrollbar(int screen, int x, int y, int w, int h,
                                int total_h, int view_h, int scroll_y,
                                bool highlighted);

#include "ui_scroll_3ds.h"

/* =========================================================================
 * 4. Hybrid Dual-Screen Touch, C-Stick & Button Input Bridge (input_3ds.c)
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
    float deadzone;        /* normalized C-Stick deadzone (0.0..0.9, default 0.14) */
    float curve_exp;       /* non-linear exponent tuned for New 3DS nub (default 1.45) */
    float sensitivity;     /* scalar multiplier (default 1.0) */
    float close_radius;    /* fixed close-aim radius around character (default 24.0 px) */
    float free_speed;      /* max px/frame in L-held ranged free-aim (default 18.0 px/frame) */
    float friction_min;    /* min speed multiplier over hostile target (default 0.42; 1.0=off) */
    float friction_radius; /* world-px hostile slowdown radius (default 34.0) */
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
    bool     ranged_aim_held;   /* L modifier held: detached top-screen free aim */
    bool     bottom_touch_frame;/* true when this frame routes pointer/click to Bottom UI */
    int      world_aim_x;       /* 120..519 (Top viewport coords inside 640x480) */
    int      world_aim_y;       /* 120..359 */
    int      bottom_touch_x;    /* 0..639 (Bottom UI coords mapped from 320x240) */
    int      bottom_touch_y;    /* 0..479 */
    nox_3ds_cstick_cfg_t cstick_cfg;
} nox_3ds_input_t;

void nox_3ds_input_init(void);
void nox_3ds_input_poll(nox_3ds_input_t *out);
const nox_3ds_input_t *nox_3ds_input_get(void);
void nox_3ds_input_set_cstick_params(float deadzone, float curve_exp, float sensitivity);
bool nox_3ds_is_gameplay_active(void);
bool nox_3ds_input_is_world_mode(void);
bool nox_3ds_input_is_bottom_touch_frame(void);
bool nox_3ds_input_is_bottom_touch_active(void);
bool nox_3ds_input_is_ranged_aim_held(void);
bool nox_3ds_input_is_cstick_or_r_active(void);
bool nox_3ds_input_get_aim_lock(int *out_orient, int *out_x, int *out_y);
bool nox_3ds_input_get_spell_pin(int *out_wx, int *out_wy);
bool nox_3ds_input_is_touch_move_active(void);
bool nox_3ds_input_is_cpad_move_active(void);
void nox_3ds_input_notify_bottom_ui_hit(bool hit_ui);
void nox_3ds_input_get_world_aim(int *out_x, int *out_y);
void nox_3ds_input_get_bottom_touch(int *out_x, int *out_y);
void nox_3ds_input_process_hybrid(void);
void nox_3ds_input_process_twin_stick_and_touch(void);
void nox_3ds_input_reset_after_applet(void);

/* =========================================================================
 * 4b. Native 3DS Software Keyboard (swkbd) Bridge (swkbd_3ds.c)
 * ========================================================================= */
void nox_3ds_swkbd_request(void *entry_win, bool auto_submit);
void nox_3ds_swkbd_cancel_pending(void *entry_win);
bool nox_3ds_swkbd_pump(void);
bool nox_3ds_swkbd_is_active(void);
bool nox_3ds_consume_direct_ip_request(void);
bool nox_3ds_swkbd_prompt_direct_ip(char *out_entry, size_t out_sz);

/* =========================================================================
 * 5. Audio Bridge: Miles AIL -> libctru 24-ch ndsp (audio_3ds.c)
 * ========================================================================= */
bool nox_3ds_audio_init(void);           /* Check sdmc:/3ds/dspfirm.cdc, ndspInit(), 24-ch setup */
void nox_3ds_audio_shutdown(void);       /* Drain/clear all 24 channels, ndspExit(), free linear pools */
void nox_3ds_audio_pump(void);           /* Refill ndsp wave buffers (invoked from AIL_serve()) */
bool nox_3ds_audio_is_available(void);   /* True if DSP firmware present and ndspInit() succeeded */

/* Native VQA cutscene audio bridge -> dedicated libctru ndsp channel (audio_3ds.c) */
typedef void (*nox_3ds_movie_audio_req_cb_t)(uintptr_t user);
typedef void (*nox_3ds_movie_audio_done_cb_t)(uintptr_t user, uintptr_t chunk_ptr);

bool     nox_3ds_movie_audio_open(uint32_t sample_rate, uint8_t channels, uint8_t bits,
                                  uint32_t chunk_bytes, uintptr_t user,
                                  nox_3ds_movie_audio_req_cb_t req_cb,
                                  nox_3ds_movie_audio_done_cb_t done_cb);
void     nox_3ds_movie_audio_start(void);
bool     nox_3ds_movie_audio_submit(const void *pcm, uint32_t byte_len, uintptr_t tag_ptr);
void     nox_3ds_movie_audio_pump(void);
void     nox_3ds_movie_audio_pause(bool paused);
void     nox_3ds_movie_audio_stop(void);
void     nox_3ds_movie_audio_close(void);
uint64_t nox_3ds_movie_audio_played_bytes(void);
bool     nox_3ds_movie_audio_is_active(void);

/* Persistent cutscene diagnostic log (sdmc:/3ds/nox/movie.log + SVC stderr) */
void     nox_3ds_movie_log(const char *fmt, ...);

/* =========================================================================
 * 6. Dual-Transport Networking (net_3ds.c -> net_loopback_3ds.c / net_sockets_3ds.c)
 * ========================================================================= */
#define NOX_3DS_LOOPBACK_FD_BASE 1024
#define NOX_3DS_LOOPBACK_FD_MAX  16
#define NOX_3DS_SOCKETS_FD_BASE  2048
#define NOX_3DS_SOCKETS_FD_MAX   32

typedef enum {
    NOX_3DS_NET_MODE_LOOPBACK = 0, /* Solo Campaign & Solo Quest (zero socInit, Airplane Mode OK) */
    NOX_3DS_NET_MODE_WIFI     = 1  /* Multiplayer menus / Direct IPv4 / Wi-Fi host via soc:u */
} nox_3ds_net_mode_t;

struct hostent;

typedef struct {
    void (*init)(void);
    void (*shutdown)(void);
    int  (*socket)(int domain, int type, int protocol);
    int  (*closesocket)(int fd);
    int  (*bind)(int fd, const void *addr, unsigned int addrlen);
    int  (*setsockopt)(int fd, int level, int opt, const void *val, unsigned int len);
    int  (*ioctlsocket)(int fd, long cmd, unsigned long *argp);
    int  (*sendto)(int fd, const void *buf, unsigned int len, int flags,
                   const void *to, unsigned int tolen);
    int  (*recvfrom)(int fd, void *buf, unsigned int len, int flags,
                     void *from, unsigned int *fromlen);
    int  (*listen)(int fd, int backlog);
    int  (*accept)(int fd, void *addr, unsigned int *addrlen);
    int  (*shutdown_sock)(int fd, int how);
    int  (*send)(int fd, const void *buf, unsigned int len, int flags);
    int  (*recv)(int fd, void *buf, unsigned int len, int flags);
    int  (*gethostname)(char *name, size_t len);
    struct hostent *(*gethostbyname)(const char *name);
} nox_3ds_net_ops_t;

extern const nox_3ds_net_ops_t g_nox_3ds_loopback_ops;
extern const nox_3ds_net_ops_t g_nox_3ds_sockets_ops;

#define NOX_PROTO_VER_LEGACY   0x0001039Au /* 66458: Nox 1.2b / nox-decomp / opennox-ivan */
#define NOX_PROTO_VER_HIGHRES  0x000F039Au /* 983962: OpenNox HD (highres) */
#define NOX_IS_COMPAT_PROTO_VER(v) \
    (((uint32_t)(v) == NOX_PROTO_VER_LEGACY) || ((uint32_t)(v) == NOX_PROTO_VER_HIGHRES))

void               nox_3ds_net_init(void);
void               nox_3ds_net_shutdown(void);
void               nox_3ds_net_log(const char *fmt, ...);
void               nox_3ds_net_set_mode(nox_3ds_net_mode_t mode);
nox_3ds_net_mode_t nox_3ds_net_get_mode(void);
bool               nox_3ds_soc_is_ready(void);
bool               nox_3ds_soc_ensure_ready(void);
bool               nox_3ds_soc_get_ip_info(uint32_t *out_ip_be, uint32_t *out_mask_be, uint32_t *out_bcast_be);
bool               nox_3ds_soc_resolve_ipv4(const char *hostname, uint32_t *out_ip_be);
void               nox_3ds_net_on_apt_suspend(bool suspended);
void               nox_3ds_lobby_init(void);
void               nox_3ds_lobby_on_mode_change(nox_3ds_net_mode_t mode);
void               nox_3ds_lobby_prefetch(void);
bool               nox_3ds_lobby_has_pending_work(void);
void               nox_3ds_lobby_shutdown(void);
void               nox_3ds_reset_join_ver_retry(void);
int                nox_3ds_try_flip_join_proto_ver(void);
void               nox_3ds_accept_join_peer_ip(uint32_t ip_be);
void               nox_3ds_set_host_client_proto_ver(uint32_t ver);
uint32_t           nox_3ds_get_host_client_proto_ver(void);
void               nox_3ds_lobby_on_host_close(int sockfd);
int                nox_3ds_net_socket(int domain, int type, int protocol);
int                nox_3ds_net_closesocket(int fd);
int                nox_3ds_net_bind(int fd, const void *addr, unsigned int addrlen);
int                nox_3ds_net_setsockopt(int fd, int level, int opt, const void *val, unsigned int len);
int                nox_3ds_net_ioctlsocket(int fd, long cmd, unsigned long *argp);
int                nox_3ds_net_sendto(int fd, const void *buf, unsigned int len, int flags,
                                      const void *to, unsigned int tolen);
int                nox_3ds_net_recvfrom(int fd, void *buf, unsigned int len, int flags,
                                        void *from, unsigned int *fromlen);
int                nox_3ds_net_last_error(void);

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

#pragma pack(pop)

#endif /* NOX_PLATFORM_3DS_H */
