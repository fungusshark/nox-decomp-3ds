#include "fov_scaler.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Reference target: 17" CRT (16" viewable) at 800x600 @ 24" (~2.29 arcmin/px). */
#define NOX_FOV_TARGET_ARCMIN_PER_PX 2.35f
#define NOX_FOV_UI_MIN_ARCMIN        12.0f

static const nox_fov_device_profile_t s_profiles[NOX_FOV_DEV_COUNT] = {
    /* AUTO (resolved at init) */
    { NOX_FOV_DEV_AUTO,            "Auto",            3.88f,  3.33f, 12.0f,  400,  240, 320, 240 },
    { NOX_FOV_DEV_O3DS,            "Old 3DS",         3.53f,  3.02f, 11.5f,  400,  240, 320, 240 },
    { NOX_FOV_DEV_O2DS,            "Old 2DS",         3.53f,  3.02f, 11.5f,  400,  240, 320, 240 },
    { NOX_FOV_DEV_N3DS,            "New 3DS",         3.88f,  3.33f, 12.0f,  400,  240, 320, 240 },
    { NOX_FOV_DEV_N3DSXL,          "3DS XL / 2DS XL", 4.88f,  4.18f, 12.5f,  400,  240, 320, 240 },
    { NOX_FOV_DEV_SWITCH_HANDHELD, "Switch Handheld", 6.20f,  6.20f, 14.0f, 1280,  720, 1280, 720 },
    { NOX_FOV_DEV_SWITCH_OLED,     "Switch OLED",     7.00f,  7.00f, 14.5f, 1280,  720, 1280, 720 },
    { NOX_FOV_DEV_SWITCH_DOCKED,   "Switch Docked",  55.00f, 55.00f, 84.0f, 1920, 1080, 1920, 1080 },
    { NOX_FOV_DEV_STEAM_DECK,      "Steam Deck",      7.00f,  7.00f, 14.0f, 1280,  800, 1280, 800 },
    { NOX_FOV_DEV_CRT_17,          "17in CRT",       16.00f, 16.00f, 24.0f,  800,  600,  800, 600 },
    { NOX_FOV_DEV_DESKTOP_1080P,   "Desktop 1080p",  24.00f, 24.00f, 24.0f, 1920, 1080, 1920, 1080 },
    { NOX_FOV_DEV_DESKTOP_4K,      "Desktop 4K",     27.00f, 27.00f, 24.0f, 3840, 2160, 3840, 2160 }
};

typedef struct {
    bool                     inited;
    nox_fov_device_profile_t prof;
    int                      buf_w;
    int                      buf_h;
    bool                     wide_5_3;
    nox_fov_preset_t         preset;
    nox_scaler_filter_t      filter;
    int                      vp_l;
    int                      vp_t;
    int                      vp_r;
    int                      vp_b;
    int                      vp_w;
    int                      vp_h;
    bool                     vp_dirty;
} nox_fov_state_t;

static nox_fov_state_t s_fov = {
    false,
    { NOX_FOV_DEV_N3DS, "New 3DS", 3.88f, 3.33f, 12.0f, 400, 240, 320, 240 },
    640, 480, true,
    NOX_FOV_PRESET_AUTO, NOX_SCALER_AUTO,
    120, 120, 519, 359, 400, 240, false
};

static int strieq_ascii(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        ++a;
        ++b;
    }
    return (*a == '\0' && *b == '\0');
}

static float vertical_height_in(float diag_in, int w, int h)
{
    if (diag_in <= 0.0f || w <= 0 || h <= 0)
        return 2.0f;
    float fw = (float)w;
    float fh = (float)h;
    return diag_in * (fh / sqrtf(fw * fw + fh * fh));
}

static float vertical_fov_arcmin(float height_in, float dist_in)
{
    if (dist_in <= 0.5f) dist_in = 0.5f;
    float rad = 2.0f * atanf((height_in * 0.5f) / dist_in);
    return rad * (180.0f / (float)M_PI) * 60.0f;
}

static nox_fov_device_id_t parse_device_env(const char *v)
{
    if (!v || !*v) return NOX_FOV_DEV_AUTO;
    if (strieq_ascii(v, "o3ds")) return NOX_FOV_DEV_O3DS;
    if (strieq_ascii(v, "o2ds") || strieq_ascii(v, "2ds")) return NOX_FOV_DEV_O2DS;
    if (strieq_ascii(v, "n3ds")) return NOX_FOV_DEV_N3DS;
    if (strieq_ascii(v, "n3dsxl") || strieq_ascii(v, "xl") || strieq_ascii(v, "n2dsxl") || strieq_ascii(v, "3dsxl"))
        return NOX_FOV_DEV_N3DSXL;
    if (strieq_ascii(v, "switch") || strieq_ascii(v, "switch_handheld")) return NOX_FOV_DEV_SWITCH_HANDHELD;
    if (strieq_ascii(v, "switch_oled")) return NOX_FOV_DEV_SWITCH_OLED;
    if (strieq_ascii(v, "switch_docked")) return NOX_FOV_DEV_SWITCH_DOCKED;
    if (strieq_ascii(v, "steam_deck") || strieq_ascii(v, "deck")) return NOX_FOV_DEV_STEAM_DECK;
    if (strieq_ascii(v, "crt") || strieq_ascii(v, "crt17")) return NOX_FOV_DEV_CRT_17;
    if (strieq_ascii(v, "1080p")) return NOX_FOV_DEV_DESKTOP_1080P;
    if (strieq_ascii(v, "4k")) return NOX_FOV_DEV_DESKTOP_4K;
    return NOX_FOV_DEV_AUTO;
}

static nox_fov_preset_t parse_preset_env(const char *v)
{
    if (!v || !*v) return NOX_FOV_PRESET_AUTO;
    if (strieq_ascii(v, "crisp") || strieq_ascii(v, "1:1")) return NOX_FOV_PRESET_CRISP;
    if (strieq_ascii(v, "balanced") || strieq_ascii(v, "bal")) return NOX_FOV_PRESET_BALANCED;
    if (strieq_ascii(v, "wide") || strieq_ascii(v, "full")) return NOX_FOV_PRESET_WIDE;
    return NOX_FOV_PRESET_AUTO;
}

static nox_scaler_filter_t parse_filter_env(const char *v)
{
    if (!v || !*v) return NOX_SCALER_AUTO;
    if (strieq_ascii(v, "nearest") || strieq_ascii(v, "point")) return NOX_SCALER_NEAREST;
    if (strieq_ascii(v, "bilinear") || strieq_ascii(v, "linear")) return NOX_SCALER_BILINEAR;
    if (strieq_ascii(v, "sharp") || strieq_ascii(v, "sharp_bilinear") || strieq_ascii(v, "aa"))
        return NOX_SCALER_SHARP_BILINEAR;
    return NOX_SCALER_AUTO;
}

static int clamp_aligned(int val, int align, int lo, int hi)
{
    int a = ((val + (align / 2)) / align) * align;
    if (a < lo) a = lo;
    if (a > hi) a = hi;
    a = (a / align) * align;
    if (a < lo) a = lo;
    return a;
}

static void recompute_viewport(void)
{
    int bw = (s_fov.buf_w > 0) ? s_fov.buf_w : 640;
    int bh = (s_fov.buf_h > 0) ? s_fov.buf_h : 480;

    int w = bw;
    int h = bh;

    /*
     * Handheld 3DS / 640x480 target family:
     * Keep square world pixels in BOTH 5:3 (wide) and 4:3 (pillarboxed) modes
     * with 16-aligned width and 8-aligned height (required by PICA200/DMA).
     */
    if (bw <= 640 && bh <= 480) {
        nox_fov_preset_t eff = s_fov.preset;
        if (eff == NOX_FOV_PRESET_AUTO) {
            float top_h_in = vertical_height_in(s_fov.prof.top_diag_in, s_fov.prof.phys_top_w, s_fov.prof.phys_top_h);
            float fov_min  = vertical_fov_arcmin(top_h_in, s_fov.prof.view_dist_in);
            int target_h   = (int)lrintf(fov_min / NOX_FOV_TARGET_ARCMIN_PER_PX);
            /* O3DS (~226) & N3DS (~242) -> 240 (CRISP 1:1); 3DS XL (~293) -> BALANCED */
            if (target_h <= 264) {
                eff = NOX_FOV_PRESET_CRISP;
            } else if (target_h <= 340) {
                eff = NOX_FOV_PRESET_BALANCED;
            } else {
                eff = NOX_FOV_PRESET_WIDE;
            }
        }

        if (s_fov.wide_5_3) {
            /* 5:3 aspect filling 400x240 Top LCD */
            switch (eff) {
            case NOX_FOV_PRESET_CRISP:
                w = 400; h = 240;
                break;
            case NOX_FOV_PRESET_BALANCED:
                w = 480; h = 288;
                break;
            case NOX_FOV_PRESET_WIDE:
            default:
                w = 640; h = 384;
                break;
            }
        } else {
            /* 4:3 classic aspect centered at 320x240 on Top LCD */
            switch (eff) {
            case NOX_FOV_PRESET_CRISP:
                w = 320; h = 240;
                break;
            case NOX_FOV_PRESET_BALANCED:
                w = 480; h = 360;
                break;
            case NOX_FOV_PRESET_WIDE:
            default:
                w = 640; h = 480;
                break;
            }
        }
    } else {
        /* Switch / Steam Deck / Desktop scalable viewport */
        if (s_fov.preset == NOX_FOV_PRESET_CRISP) {
            w = clamp_aligned(s_fov.prof.phys_top_w / 2, 16, 320, bw);
            h = clamp_aligned(s_fov.prof.phys_top_h / 2, 8,  200, bh);
        } else if (s_fov.preset == NOX_FOV_PRESET_BALANCED) {
            float top_h_in = vertical_height_in(s_fov.prof.top_diag_in, s_fov.prof.phys_top_w, s_fov.prof.phys_top_h);
            float fov_min  = vertical_fov_arcmin(top_h_in, s_fov.prof.view_dist_in);
            int target_h   = (int)lrintf(fov_min / NOX_FOV_TARGET_ARCMIN_PER_PX);
            h = clamp_aligned(target_h, 8, 240, bh);
            int target_w   = (h * s_fov.prof.phys_top_w) / ((s_fov.prof.phys_top_h > 0) ? s_fov.prof.phys_top_h : 1);
            w = clamp_aligned(target_w, 16, 320, bw);
        } else {
            w = bw;
            h = bh;
        }
    }

    if (w > bw) w = (bw / 16) * 16;
    if (h > bh) h = (bh / 8) * 8;
    if (w < 320) w = 320;
    if (h < 200) h = 200;

    int l = (bw - w) / 2;
    int t = (bh - h) / 2;
    l &= ~1;
    t &= ~1;
    int r = l + w - 1;
    int b = t + h - 1;

    if (s_fov.vp_w != w || s_fov.vp_h != h || s_fov.vp_l != l || s_fov.vp_t != t) {
        s_fov.vp_dirty = true;
    }
    s_fov.vp_l = l;
    s_fov.vp_t = t;
    s_fov.vp_r = r;
    s_fov.vp_b = b;
    s_fov.vp_w = w;
    s_fov.vp_h = h;
}

void nox_fov_scaler_init(nox_fov_device_id_t detected_dev,
                         int logical_buf_w, int logical_buf_h,
                         bool wide_5_3)
{
    nox_fov_device_id_t dev = detected_dev;
    nox_fov_device_id_t env_dev = parse_device_env(getenv("NOX_FOV_DEVICE"));
    if (env_dev != NOX_FOV_DEV_AUTO) {
        dev = env_dev;
    }
    if (dev <= NOX_FOV_DEV_AUTO || dev >= NOX_FOV_DEV_COUNT) {
#ifdef __3DS__
        dev = NOX_FOV_DEV_N3DS;
#else
        dev = NOX_FOV_DEV_DESKTOP_1080P;
#endif
    }

    s_fov.prof = s_profiles[dev];

    const char *diag_env = getenv("NOX_FOV_DIAG_IN");
    if (diag_env && *diag_env) {
        float d = strtof(diag_env, NULL);
        if (d >= 2.0f && d <= 120.0f) {
            s_fov.prof.top_diag_in = d;
            s_fov.prof.bot_diag_in = d;
        }
    }
    const char *dist_env = getenv("NOX_FOV_VIEW_DIST_IN");
    if (dist_env && *dist_env) {
        float vd = strtof(dist_env, NULL);
        if (vd >= 4.0f && vd <= 240.0f) {
            s_fov.prof.view_dist_in = vd;
        }
    }

    s_fov.buf_w    = (logical_buf_w > 0) ? logical_buf_w : 640;
    s_fov.buf_h    = (logical_buf_h > 0) ? logical_buf_h : 480;
    s_fov.wide_5_3 = wide_5_3;
    s_fov.preset   = parse_preset_env(getenv("NOX_FOV_PRESET"));
    s_fov.filter   = parse_filter_env(getenv("NOX_SCALER_FILTER"));
    s_fov.vp_dirty = false;
    s_fov.inited   = true;

    recompute_viewport();
}

void nox_fov_scaler_set_logical_size(int logical_buf_w, int logical_buf_h)
{
    if (logical_buf_w <= 0 || logical_buf_h <= 0)
        return;
    if (s_fov.buf_w == logical_buf_w && s_fov.buf_h == logical_buf_h)
        return;
    s_fov.buf_w = logical_buf_w;
    s_fov.buf_h = logical_buf_h;
    recompute_viewport();
}

void nox_fov_scaler_set_wide_aspect(bool wide_5_3)
{
    if (s_fov.wide_5_3 == wide_5_3)
        return;
    s_fov.wide_5_3 = wide_5_3;
    recompute_viewport();
}

bool nox_fov_scaler_get_wide_aspect(void)
{
    return s_fov.wide_5_3;
}

void nox_fov_scaler_get_world_viewport(int *out_l, int *out_t,
                                       int *out_r, int *out_b,
                                       int *out_w, int *out_h)
{
    if (!s_fov.inited) {
        recompute_viewport();
        s_fov.inited = true;
    }
    if (out_l) *out_l = s_fov.vp_l;
    if (out_t) *out_t = s_fov.vp_t;
    if (out_r) *out_r = s_fov.vp_r;
    if (out_b) *out_b = s_fov.vp_b;
    if (out_w) *out_w = s_fov.vp_w;
    if (out_h) *out_h = s_fov.vp_h;
}

nox_fov_preset_t nox_fov_scaler_cycle_preset(void)
{
    s_fov.preset = (nox_fov_preset_t)(((int)s_fov.preset + 1) % (int)NOX_FOV_PRESET_COUNT);
    recompute_viewport();
    return s_fov.preset;
}

nox_fov_preset_t nox_fov_scaler_get_preset(void)
{
    return s_fov.preset;
}

void nox_fov_scaler_set_preset(nox_fov_preset_t preset)
{
    if (preset < NOX_FOV_PRESET_AUTO || preset >= NOX_FOV_PRESET_COUNT)
        preset = NOX_FOV_PRESET_AUTO;
    if (s_fov.preset != preset) {
        s_fov.preset = preset;
        recompute_viewport();
    }
}

nox_scaler_filter_t nox_fov_scaler_cycle_filter(void)
{
    s_fov.filter = (nox_scaler_filter_t)(((int)s_fov.filter + 1) % (int)NOX_SCALER_COUNT);
    return s_fov.filter;
}

nox_scaler_filter_t nox_fov_scaler_get_filter(void)
{
    return s_fov.filter;
}

void nox_fov_scaler_set_filter(nox_scaler_filter_t filter)
{
    if (filter < NOX_SCALER_AUTO || filter >= NOX_SCALER_COUNT)
        filter = NOX_SCALER_AUTO;
    s_fov.filter = filter;
}

const char *nox_fov_scaler_device_name(void)
{
    return s_fov.prof.name;
}

const char *nox_fov_scaler_status_label(char *buf, size_t cap)
{
    static const char *const k_names[NOX_FOV_PRESET_COUNT] = {
        "AUTO", "CRISP", "BAL", "WIDE"
    };
    if (!buf || cap == 0)
        return "";
    int idx = (int)s_fov.preset;
    if (idx < 0 || idx >= NOX_FOV_PRESET_COUNT) idx = 0;
    snprintf(buf, cap, "FOV: %s %dx%d (%s)",
             k_names[idx], s_fov.vp_w, s_fov.vp_h,
             s_fov.wide_5_3 ? "5:3" : "4:3");
    return buf;
}

const char *nox_fov_scaler_filter_label(void)
{
    switch (s_fov.filter) {
    case NOX_SCALER_NEAREST:        return "FILTER: NEAREST";
    case NOX_SCALER_BILINEAR:       return "FILTER: BILINEAR";
    case NOX_SCALER_SHARP_BILINEAR: return "FILTER: SHARP AA";
    case NOX_SCALER_AUTO:
    default:                        return "FILTER: AUTO";
    }
}

bool nox_fov_scaler_consume_viewport_dirty(void)
{
    bool d = s_fov.vp_dirty;
    s_fov.vp_dirty = false;
    return d;
}

float nox_fov_scaler_ui_default_font_arcmin(void)
{
    float bot_h_in = vertical_height_in(s_fov.prof.bot_diag_in, s_fov.prof.phys_bot_w, s_fov.prof.phys_bot_h);
    float fov_min  = vertical_fov_arcmin(bot_h_in, s_fov.prof.view_dist_in);
    float px_arcmin = fov_min / (float)((s_fov.prof.phys_bot_h > 0) ? s_fov.prof.phys_bot_h : 240);
    /* default.fnt has 8px logical cap height across a 480p UI canvas scaled to phys_bot_h. */
    float scale_y = (float)s_fov.prof.phys_bot_h / 480.0f;
    return 8.0f * scale_y * px_arcmin;
}

bool nox_fov_scaler_should_promote_small_ui_font(void)
{
#ifdef __3DS__
    return true;
#else
    return nox_fov_scaler_ui_default_font_arcmin() < NOX_FOV_UI_MIN_ARCMIN;
#endif
}

const nox_fov_device_profile_t *nox_fov_scaler_get_profile(void)
{
    return &s_fov.prof;
}

/*
 * Fast carry-free 2x2 SWAR box filter:
 * Reads 0RGB555 rows from cached RAM (masking bit-15 occlusion residual 0x8000),
 * averages 2x2 texels in integer registers with zero per-channel unpacking, and
 * emits RGBA5551 ((px << 1) | 1) in 32-bit paired pixel writes.
 */
void nox_fov_scaler_downsample_2x_box_555_to_5551(
    uint16_t * const *src_rows, int src_w, int src_h,
    uint16_t *dst_pixels, int dst_pitch_bytes)
{
    if (!src_rows || !dst_pixels || src_w < 2 || src_h < 2 || dst_pitch_bytes <= 0)
        return;

    const int dst_w = src_w >> 1;
    const int dst_h = src_h >> 1;
    uint8_t *dst_base = (uint8_t *)dst_pixels;

    for (int y = 0; y < dst_h; ++y) {
        const uint16_t *r0_16 = src_rows[y * 2];
        const uint16_t *r1_16 = src_rows[y * 2 + 1];
        uint16_t *d16 = (uint16_t *)(void *)(dst_base + (size_t)y * (size_t)dst_pitch_bytes);
        int x = 0;

        if ((((uintptr_t)r0_16 | (uintptr_t)r1_16 | (uintptr_t)d16) & 3u) == 0u) {
            const uint32_t *r0_32 = (const uint32_t *)(const void *)r0_16;
            const uint32_t *r1_32 = (const uint32_t *)(const void *)r1_16;
            uint32_t *d32 = (uint32_t *)(void *)d16;
            const int pairs = dst_w >> 1;

            for (int p = 0; p < pairs; ++p) {
                /* Even output pixel from 2x2 block 0 */
                uint32_t a0 = r0_32[0] & 0x7FFF7FFFu;
                uint32_t b0 = r1_32[0] & 0x7FFF7FFFu;
                uint32_t v0 = (a0 & b0) + (((a0 ^ b0) & 0x7BDE7BDEu) >> 1);
                uint32_t lo0 = v0 & 0x7FFFu;
                uint32_t hi0 = v0 >> 16;
                uint32_t px0 = (lo0 & hi0) + (((lo0 ^ hi0) & 0x7BDEu) >> 1);

                /* Odd output pixel from 2x2 block 1 */
                uint32_t a1 = r0_32[1] & 0x7FFF7FFFu;
                uint32_t b1 = r1_32[1] & 0x7FFF7FFFu;
                uint32_t v1 = (a1 & b1) + (((a1 ^ b1) & 0x7BDE7BDEu) >> 1);
                uint32_t lo1 = v1 & 0x7FFFu;
                uint32_t hi1 = v1 >> 16;
                uint32_t px1 = (lo1 & hi1) + (((lo1 ^ hi1) & 0x7BDEu) >> 1);

                uint32_t out0 = ((px0 & 0x7FFFu) << 1) | 1u;
                uint32_t out1 = ((px1 & 0x7FFFu) << 1) | 1u;
                *d32++ = out0 | (out1 << 16);

                r0_32 += 2;
                r1_32 += 2;
            }
            x = pairs * 2;
            d16 = (uint16_t *)(void *)d32;
        }

        for (; x < dst_w; ++x) {
            uint32_t p00 = (uint32_t)r0_16[x * 2]     & 0x7FFFu;
            uint32_t p01 = (uint32_t)r0_16[x * 2 + 1] & 0x7FFFu;
            uint32_t p10 = (uint32_t)r1_16[x * 2]     & 0x7FFFu;
            uint32_t p11 = (uint32_t)r1_16[x * 2 + 1] & 0x7FFFu;
            uint32_t h0  = (p00 & p01) + (((p00 ^ p01) & 0x7BDEu) >> 1);
            uint32_t h1  = (p10 & p11) + (((p10 ^ p11) & 0x7BDEu) >> 1);
            uint32_t px  = (h0 & h1) + (((h0 ^ h1) & 0x7BDEu) >> 1);
            *d16++ = (uint16_t)(((px & 0x7FFFu) << 1) | 1u);
        }
    }
}

static float lanczos2_weight(float x)
{
    float ax = fabsf(x);
    if (ax < 1e-6f)
        return 1.0f;
    if (ax >= 2.0f)
        return 0.0f;
    float pix = (float)M_PI * ax;
    float pix2 = pix * 0.5f;
    return (sinf(pix) / pix) * (sinf(pix2) / pix2);
}

static inline int clamp_5bit(int v)
{
    if (v < 0) return 0;
    if (v > 31) return 31;
    return v;
}

/*
 * One-shot / load-time Lanczos-2 (band-limited on downscale) pre-resampler
 * for static RGBA5551 artwork (e.g. Title Splash). Never invoked per-frame.
 */
void nox_fov_scaler_precompute_lanczos2_5551(
    const uint16_t *src_pixels, int src_pitch_bytes, int src_w, int src_h,
    uint16_t *dst_pixels, int dst_pitch_bytes, int dst_w, int dst_h)
{
    if (!src_pixels || !dst_pixels || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return;

    /* Intermediate horizontal pass buffer storing Q4 5-bit R, G, B per pixel. */
    int16_t *tmp = (int16_t *)malloc((size_t)src_h * (size_t)dst_w * 3u * sizeof(int16_t));
    if (!tmp)
        return;

    const float scale_x = (float)src_w / (float)dst_w;
    const float scale_y = (float)src_h / (float)dst_h;
    const float support_x = (scale_x > 1.0f) ? (2.0f * scale_x) : 2.0f;
    const float support_y = (scale_y > 1.0f) ? (2.0f * scale_y) : 2.0f;
    const float inv_sx = (scale_x > 1.0f) ? (1.0f / scale_x) : 1.0f;
    const float inv_sy = (scale_y > 1.0f) ? (1.0f / scale_y) : 1.0f;

    /* Precompute horizontal tap bounds and weights */
    const int max_taps_x = (int)ceilf(support_x * 2.0f) + 2;
    int *hx0 = (int *)malloc((size_t)dst_w * sizeof(int));
    int *hcnt = (int *)malloc((size_t)dst_w * sizeof(int));
    float *hw = (float *)malloc((size_t)dst_w * (size_t)max_taps_x * sizeof(float));
    if (!hx0 || !hcnt || !hw) {
        free(tmp); free(hx0); free(hcnt); free(hw);
        return;
    }

    for (int dx = 0; dx < dst_w; ++dx) {
        float center = ((float)dx + 0.5f) * scale_x - 0.5f;
        int x_start = (int)floorf(center - support_x);
        int x_end   = (int)ceilf(center + support_x);
        if (x_start < 0) x_start = 0;
        if (x_end >= src_w) x_end = src_w - 1;
        int cnt = x_end - x_start + 1;
        if (cnt > max_taps_x) cnt = max_taps_x;
        hx0[dx] = x_start;
        hcnt[dx] = cnt;
        float wsum = 0.0f;
        for (int k = 0; k < cnt; ++k) {
            float w = lanczos2_weight(((float)(x_start + k) - center) * inv_sx);
            hw[(size_t)dx * (size_t)max_taps_x + (size_t)k] = w;
            wsum += w;
        }
        if (wsum != 0.0f) {
            float inv = 1.0f / wsum;
            for (int k = 0; k < cnt; ++k)
                hw[(size_t)dx * (size_t)max_taps_x + (size_t)k] *= inv;
        }
    }

    const uint8_t *sbase = (const uint8_t *)src_pixels;
    for (int sy = 0; sy < src_h; ++sy) {
        const uint16_t *srow = (const uint16_t *)(const void *)(sbase + (size_t)sy * (size_t)src_pitch_bytes);
        int16_t *trow = tmp + (size_t)sy * (size_t)dst_w * 3u;
        for (int dx = 0; dx < dst_w; ++dx) {
            int x0 = hx0[dx];
            int cnt = hcnt[dx];
            const float *kw = &hw[(size_t)dx * (size_t)max_taps_x];
            float r = 0.0f, g = 0.0f, b = 0.0f;
            for (int k = 0; k < cnt; ++k) {
                uint16_t px = srow[x0 + k];
                float w = kw[k];
                r += (float)((px >> 11) & 31u) * w;
                g += (float)((px >> 6)  & 31u) * w;
                b += (float)((px >> 1)  & 31u) * w;
            }
            trow[dx * 3 + 0] = (int16_t)lrintf(r * 16.0f);
            trow[dx * 3 + 1] = (int16_t)lrintf(g * 16.0f);
            trow[dx * 3 + 2] = (int16_t)lrintf(b * 16.0f);
        }
    }

    free(hx0);
    free(hcnt);
    free(hw);

    /* Vertical pass */
    const int max_taps_y = (int)ceilf(support_y * 2.0f) + 2;
    float *vw = (float *)malloc((size_t)max_taps_y * sizeof(float));
    uint8_t *dbase = (uint8_t *)dst_pixels;

    if (vw) {
        for (int dy = 0; dy < dst_h; ++dy) {
            float center = ((float)dy + 0.5f) * scale_y - 0.5f;
            int y_start = (int)floorf(center - support_y);
            int y_end   = (int)ceilf(center + support_y);
            if (y_start < 0) y_start = 0;
            if (y_end >= src_h) y_end = src_h - 1;
            int cnt = y_end - y_start + 1;
            if (cnt > max_taps_y) cnt = max_taps_y;

            float wsum = 0.0f;
            for (int k = 0; k < cnt; ++k) {
                vw[k] = lanczos2_weight(((float)(y_start + k) - center) * inv_sy);
                wsum += vw[k];
            }
            if (wsum != 0.0f) {
                float inv = 1.0f / wsum;
                for (int k = 0; k < cnt; ++k)
                    vw[k] *= inv;
            }

            uint16_t *drow = (uint16_t *)(void *)(dbase + (size_t)dy * (size_t)dst_pitch_bytes);
            for (int dx = 0; dx < dst_w; ++dx) {
                float r = 0.0f, g = 0.0f, b = 0.0f;
                for (int k = 0; k < cnt; ++k) {
                    const int16_t *tp = tmp + ((size_t)(y_start + k) * (size_t)dst_w + (size_t)dx) * 3u;
                    float w = vw[k];
                    r += (float)tp[0] * w;
                    g += (float)tp[1] * w;
                    b += (float)tp[2] * w;
                }
                int r5 = clamp_5bit((int)lrintf(r * (1.0f / 16.0f)));
                int g5 = clamp_5bit((int)lrintf(g * (1.0f / 16.0f)));
                int b5 = clamp_5bit((int)lrintf(b * (1.0f / 16.0f)));
                drow[dx] = (uint16_t)((r5 << 11) | (g5 << 6) | (b5 << 1) | 1);
            }
        }
        free(vw);
    }

    free(tmp);
}
