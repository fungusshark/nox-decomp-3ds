#ifndef NOX_FOV_SCALER_H
#define NOX_FOV_SCALER_H

/*
 * src/proto.h wraps engine headers in #pragma pack(push, 1).
 * Reset packing to natural platform ABI alignment across this header.
 */
#pragma pack(push)
#pragma pack()

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NOX_FOV_DEV_AUTO = 0,
    NOX_FOV_DEV_O3DS,
    NOX_FOV_DEV_O2DS,
    NOX_FOV_DEV_N3DS,
    NOX_FOV_DEV_N3DSXL,
    NOX_FOV_DEV_SWITCH_HANDHELD,
    NOX_FOV_DEV_SWITCH_OLED,
    NOX_FOV_DEV_SWITCH_DOCKED,
    NOX_FOV_DEV_STEAM_DECK,
    NOX_FOV_DEV_CRT_17,
    NOX_FOV_DEV_DESKTOP_1080P,
    NOX_FOV_DEV_DESKTOP_4K,
    NOX_FOV_DEV_COUNT
} nox_fov_device_id_t;

typedef enum {
    NOX_FOV_PRESET_AUTO = 0,
    NOX_FOV_PRESET_CRISP,
    NOX_FOV_PRESET_BALANCED,
    NOX_FOV_PRESET_WIDE,
    NOX_FOV_PRESET_COUNT
} nox_fov_preset_t;

typedef enum {
    NOX_SCALER_AUTO = 0,
    NOX_SCALER_NEAREST,
    NOX_SCALER_BILINEAR,
    NOX_SCALER_SHARP_BILINEAR,
    NOX_SCALER_COUNT
} nox_scaler_filter_t;

typedef struct {
    nox_fov_device_id_t device_id;
    const char         *name;
    float               top_diag_in;
    float               bot_diag_in;
    float               view_dist_in;
    int                 phys_top_w;
    int                 phys_top_h;
    int                 phys_bot_w;
    int                 phys_bot_h;
} nox_fov_device_profile_t;

/* Initialize FOV & scaling engine for the active platform and buffer bounds. */
void nox_fov_scaler_init(nox_fov_device_id_t detected_dev,
                         int logical_buf_w, int logical_buf_h,
                         bool wide_5_3);

/* Update logical buffer bounds (e.g. desktop resolution switches). */
void nox_fov_scaler_set_logical_size(int logical_buf_w, int logical_buf_h);

/* Synchronize 5:3 wide vs 4:3 classic aspect mode (keeps square world pixels). */
void nox_fov_scaler_set_wide_aspect(bool wide_5_3);
bool nox_fov_scaler_get_wide_aspect(void);

/* Query centered world render viewport (16-aligned W, 8-aligned H). */
void nox_fov_scaler_get_world_viewport(int *out_l, int *out_t,
                                       int *out_r, int *out_b,
                                       int *out_w, int *out_h);

/* Cycle or inspect FOV preset and OpenGL/present filter mode. */
nox_fov_preset_t nox_fov_scaler_cycle_preset(void);
nox_fov_preset_t nox_fov_scaler_get_preset(void);
void             nox_fov_scaler_set_preset(nox_fov_preset_t preset);

nox_scaler_filter_t nox_fov_scaler_cycle_filter(void);
nox_scaler_filter_t nox_fov_scaler_get_filter(void);
void                nox_fov_scaler_set_filter(nox_scaler_filter_t filter);

const char *nox_fov_scaler_device_name(void);
const char *nox_fov_scaler_status_label(char *buf, size_t cap);
const char *nox_fov_scaler_filter_label(void);

/* Returns true once after viewport geometry changes so callers re-anchor tile caches. */
bool nox_fov_scaler_consume_viewport_dirty(void);

/* Optical legibility check: true when 0.5x default.fnt (8px cap) subtends < 12.0 arcmin. */
bool nox_fov_scaler_should_promote_small_ui_font(void);
float nox_fov_scaler_ui_default_font_arcmin(void);
const nox_fov_device_profile_t *nox_fov_scaler_get_profile(void);

/*
 * Fast 2x2 SWAR box filter downsampler for cached RAM UI buffers:
 * Converts 0RGB555 (src_w x src_h) -> RGBA5551 ((src_w/2) x (src_h/2)),
 * masking out bit 15 (0x8000) stencil artifacts and setting alpha=1.
 */
void nox_fov_scaler_downsample_2x_box_555_to_5551(
    uint16_t * const *src_rows, int src_w, int src_h,
    uint16_t *dst_pixels, int dst_pitch_bytes);

/*
 * One-shot / load-time Lanczos-2 (a=2, band-limited on downscale) pre-downsampler
 * for static RGBA5551 splash / UI artwork. Never called per-frame.
 */
void nox_fov_scaler_precompute_lanczos2_5551(
    const uint16_t *src_pixels, int src_pitch_bytes, int src_w, int src_h,
    uint16_t *dst_pixels, int dst_pitch_bytes, int dst_w, int dst_h);

#ifdef __cplusplus
}
#endif

#pragma pack(pop)

#endif /* NOX_FOV_SCALER_H */
