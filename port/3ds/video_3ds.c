#include "platform_3ds.h"

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDL_FMT_RGB565   0x15151002u
#define SDL_FMT_RGBA5551 0x15421002u

static bool              s_video_inited = false;
static C3D_RenderTarget *s_top_target = NULL;
static C3D_Tex           s_tex;
static bool              s_tex_inited = false;
static bool              s_tex_in_vram = false;
static GPU_TEXCOLOR      s_tex_fmt = GPU_RGBA5551;
static Tex3DS_SubTexture s_subtex;
static uint16_t         *s_staging = NULL;
static float             s_gamma = 1.0f;

static uint32_t          s_frame_count = 0;
static uint32_t          s_fps_last_ms = 0;
static uint32_t          s_diag_last_ms = 0;

static GPU_TEXCOLOR gpu_fmt_for_sdl(uint32_t sdl_format)
{
    if (sdl_format == SDL_FMT_RGB565)
        return GPU_RGB565;
    return GPU_RGBA5551;
}

static GX_TRANSFER_FORMAT gx_fmt_for_sdl(uint32_t sdl_format)
{
    if (sdl_format == SDL_FMT_RGB565)
        return GX_TRANSFER_FMT_RGB565;
    return GX_TRANSFER_FMT_RGB5A1;
}

static bool ensure_texture(GPU_TEXCOLOR fmt)
{
    if (s_tex_inited && s_tex_fmt == fmt)
        return true;

    if (s_tex_inited) {
        C3D_TexDelete(&s_tex);
        s_tex_inited = false;
    }

    s_tex_in_vram = C3D_TexInitVRAM(&s_tex, NOX_3DS_TEX_W, NOX_3DS_TEX_H, fmt);
    if (!s_tex_in_vram) {
        if (!C3D_TexInit(&s_tex, NOX_3DS_TEX_W, NOX_3DS_TEX_H, fmt)) {
            fprintf(stderr, "[video3ds] ERROR: C3D_TexInit(%ux%u, fmt=%d) failed\n",
                    NOX_3DS_TEX_W, NOX_3DS_TEX_H, (int)fmt);
            return false;
        }
    }

    C3D_TexSetFilter(&s_tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&s_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    s_tex_fmt = fmt;
    s_tex_inited = true;

    printf("[video3ds] Texture %ux%u allocated (%s, fmt=%s)\n",
           NOX_3DS_TEX_W, NOX_3DS_TEX_H,
           s_tex_in_vram ? "VRAM" : "FCRAM",
           (fmt == GPU_RGB565) ? "RGB565" : "RGBA5551");
    return true;
}

bool nox_3ds_video_init(int logical_w, int logical_h)
{
    (void)logical_w;
    (void)logical_h;

    if (s_video_inited)
        return true;

    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        fprintf(stderr, "[video3ds] ERROR: C3D_Init failed\n");
        return false;
    }
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) {
        fprintf(stderr, "[video3ds] ERROR: C2D_Init failed\n");
        C3D_Fini();
        return false;
    }
    C2D_Prepare();

    s_top_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    if (!s_top_target) {
        fprintf(stderr, "[video3ds] ERROR: C2D_CreateScreenTarget failed\n");
        C2D_Fini();
        C3D_Fini();
        return false;
    }

    if (!ensure_texture(GPU_RGBA5551)) {
        C2D_Fini();
        C3D_Fini();
        s_top_target = NULL;
        return false;
    }

    /*
     * PICA200 / citro2d maps texture memory row 0 to V = 1.0 (top) and row
     * (TEX_H - 1) to V = 0.0 (bottom). With GX_TRANSFER_FLIP_VERT(0), source
     * row 0 lands at V = 1.0 and source row (LOGICAL_H - 1) lands at
     * V = (TEX_H - LOGICAL_H) / TEX_H = 32/512.
     * Keeping top = 1.0f > bottom keeps Tex3DS_SubTextureRotated() == false.
     */
    s_subtex.width  = NOX_3DS_LOGICAL_W;
    s_subtex.height = NOX_3DS_LOGICAL_H;
    s_subtex.left   = 0.0f;
    s_subtex.right  = (float)NOX_3DS_LOGICAL_W / (float)NOX_3DS_TEX_W;
    s_subtex.top    = 1.0f;
    s_subtex.bottom = (float)(NOX_3DS_TEX_H - NOX_3DS_LOGICAL_H) / (float)NOX_3DS_TEX_H;

    s_fps_last_ms = nox_3ds_get_ticks_ms();
    s_diag_last_ms = s_fps_last_ms;
    s_frame_count = 0;
    s_video_inited = true;

    printf("[video3ds] Top target 400x240 active (60Hz VSync, bottom console diagnostics)\n");
    return true;
}

void nox_3ds_video_shutdown(void)
{
    if (!s_video_inited)
        return;

    if (s_staging) {
        linearFree(s_staging);
        s_staging = NULL;
    }
    if (s_tex_inited) {
        C3D_TexDelete(&s_tex);
        s_tex_inited = false;
    }
    s_top_target = NULL;
    C2D_Fini();
    C3D_Fini();
    s_video_inited = false;
}

bool nox_3ds_video_is_active(void)
{
    return s_video_inited;
}

void *nox_3ds_linear_alloc(size_t size)
{
    if (size == 0)
        return NULL;
    void *p = linearAlloc(size);
    if (p) {
        memset(p, 0, size);
    }
    return p;
}

void nox_3ds_linear_free(void *ptr)
{
    if (ptr) {
        linearFree(ptr);
    }
}

void nox_3ds_video_set_gamma(float gamma)
{
    if (gamma < 0.2f) gamma = 0.2f;
    if (gamma > 2.5f) gamma = 2.5f;
    s_gamma = gamma;
}

void nox_3ds_video_toggle_wide(void)
{
    nox_3ds_state_t *st = nox_3ds_state_mut();
    st->wide_stretch = !st->wide_stretch;
    printf("[video3ds] Aspect: %s\n", st->wide_stretch ? "400x240 Wide" : "320x240 (4:3)");
}

void nox_3ds_video_present(const void *pixels, int w, int h, int pitch,
                            uint32_t sdl_format, bool is_linear)
{
    if (!pixels || w <= 0 || h <= 0 || pitch <= 0)
        return;

    if (!s_video_inited) {
        if (!nox_3ds_video_init(NOX_3DS_LOGICAL_W, NOX_3DS_LOGICAL_H))
            return;
    }

    GPU_TEXCOLOR gpu_fmt = gpu_fmt_for_sdl(sdl_format);
    GX_TRANSFER_FORMAT gx_fmt = gx_fmt_for_sdl(sdl_format);
    if (!ensure_texture(gpu_fmt))
        return;

    if (w > NOX_3DS_TEX_W) w = NOX_3DS_TEX_W;
    if (h > NOX_3DS_TEX_H) h = NOX_3DS_TEX_H;

    s_subtex.width  = (u16)w;
    s_subtex.height = (u16)h;
    s_subtex.left   = 0.0f;
    s_subtex.right  = (float)w / (float)NOX_3DS_TEX_W;
    s_subtex.top    = 1.0f;
    s_subtex.bottom = (float)(NOX_3DS_TEX_H - h) / (float)NOX_3DS_TEX_H;

    const void *xfer_src = NULL;
    if (is_linear && pitch == NOX_3DS_TEX_W * 2) {
        GSPGPU_FlushDataCache((void *)pixels, (u32)(pitch * h));
        xfer_src = pixels;
    } else {
        if (!s_staging) {
            s_staging = (uint16_t *)linearAlloc((size_t)NOX_3DS_TEX_W * (size_t)NOX_3DS_TEX_H * 2u);
            if (!s_staging)
                return;
            memset(s_staging, 0, (size_t)NOX_3DS_TEX_W * (size_t)NOX_3DS_TEX_H * 2u);
        }
        const uint8_t *src_row = (const uint8_t *)pixels;
        uint8_t *dst_row = (uint8_t *)s_staging;
        const size_t copy_bytes = (size_t)w * 2u;
        for (int y = 0; y < h; ++y) {
            memcpy(dst_row, src_row, copy_bytes);
            src_row += pitch;
            dst_row += NOX_3DS_TEX_W * 2;
        }
        GSPGPU_FlushDataCache(s_staging, (u32)(NOX_3DS_TEX_W * h * 2));
        xfer_src = s_staging;
    }

    const u32 xfer_flags =
        GX_TRANSFER_FLIP_VERT(0) |
        GX_TRANSFER_OUT_TILED(1) |
        GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(gx_fmt) |
        GX_TRANSFER_OUT_FORMAT(gx_fmt) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

    C3D_SyncDisplayTransfer(
        (u32 *)xfer_src,
        GX_BUFFER_DIM(NOX_3DS_TEX_W, NOX_3DS_TEX_H),
        (u32 *)s_tex.data,
        GX_BUFFER_DIM(NOX_3DS_TEX_W, NOX_3DS_TEX_H),
        xfer_flags
    );

    if (C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) {
        const nox_3ds_state_t *st = nox_3ds_get_state();
        C2D_TargetClear(s_top_target, C2D_Color32(0, 0, 0, 255));
        C2D_SceneBegin(s_top_target);

        C2D_Image img;
        img.tex = &s_tex;
        img.subtex = &s_subtex;

        float dst_w = st->wide_stretch ? (float)NOX_3DS_TOP_W : 320.0f;
        float dst_h = (float)NOX_3DS_TOP_H;
        float draw_x = ((float)NOX_3DS_TOP_W - dst_w) * 0.5f;
        float draw_y = 0.0f;
        float scale_x = dst_w / (float)w;
        float scale_y = dst_h / (float)h;

        if (s_gamma < 0.98f) {
            C2D_ImageTint tint;
            C2D_SetTintMode(C2D_TintMult);
            C2D_PlainImageTint(&tint, C2D_Color32f(s_gamma, s_gamma, s_gamma, 1.0f), 1.0f);
            C2D_DrawImageAt(img, draw_x, draw_y, 0.0f, &tint, scale_x, scale_y);
        } else {
            C2D_DrawImageAt(img, draw_x, draw_y, 0.0f, NULL, scale_x, scale_y);
        }

        C3D_FrameEnd(0);
    }

    s_frame_count++;
    uint32_t now_ms = nox_3ds_get_ticks_ms();
    if (now_ms - s_fps_last_ms >= 1000u) {
        uint32_t elapsed = now_ms - s_fps_last_ms;
        nox_3ds_state_t *st = nox_3ds_state_mut();
        st->fps = (elapsed > 0) ? (s_frame_count * 1000u) / elapsed : 0;
        s_frame_count = 0;
        s_fps_last_ms = now_ms;
    }

    if (now_ms - s_diag_last_ms >= 5000u) {
        const nox_3ds_state_t *st = nox_3ds_get_state();
        s_diag_last_ms = now_ms;
        printf("[3ds] %u FPS | aim=%s cstk=%s | %s | linFree=%uKB | heap=%.1fMB\n",
               (unsigned)st->fps,
               (st->aim_mode == NOX_3DS_AIM_RADIAL) ? "Radial" : "Cursor",
               (st->cstick_mode == NOX_3DS_CSTICK_AIM_CAMERA) ? "Aim" : "Spell",
               st->wide_stretch ? "400x240" : "4:3",
               (unsigned)(st->linear_free_bytes / 1024u),
               (double)st->heap_used_bytes / (1024.0 * 1024.0));
    }
}

int nox_3ds_blit_scaled_16(const void *src_pixels, int sw_surf, int sh_surf, int spitch,
                             bool has_ckey, uint16_t ckey,
                             int sx, int sy, int sw, int sh,
                             void *dst_pixels, int dw_surf, int dh_surf, int dpitch,
                             int dx, int dy, int dw, int dh)
{
    if (!src_pixels || !dst_pixels || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return 0;

    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sx + sw > sw_surf) sw = sw_surf - sx;
    if (sy + sh > sh_surf) sh = sh_surf - sy;
    if (sw <= 0 || sh <= 0) return 0;

    int x0 = dx;
    int y0 = dy;
    int x1 = dx + dw;
    int y1 = dy + dh;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > dw_surf) x1 = dw_surf;
    if (y1 > dh_surf) y1 = dh_surf;
    if (x0 >= x1 || y0 >= y1) return 0;

    const uint8_t *sbase = (const uint8_t *)src_pixels;
    uint8_t *dbase = (uint8_t *)dst_pixels;

    if (sw == dw && sh == dh) {
        int s_off_x = sx + (x0 - dx);
        int s_off_y = sy + (y0 - dy);
        int copy_w = x1 - x0;
        int copy_h = y1 - y0;

        for (int row = 0; row < copy_h; ++row) {
            const uint16_t *sp = (const uint16_t *)(const void *)(sbase + (s_off_y + row) * spitch) + s_off_x;
            uint16_t *dp = (uint16_t *)(void *)(dbase + (y0 + row) * dpitch) + x0;
            if (!has_ckey) {
                memcpy(dp, sp, (size_t)copy_w * 2u);
            } else {
                for (int col = 0; col < copy_w; ++col) {
                    uint16_t px = sp[col];
                    if (px != ckey)
                        dp[col] = px;
                }
            }
        }
        return 0;
    }

    for (int dst_y = y0; dst_y < y1; ++dst_y) {
        int src_y = sy + ((dst_y - dy) * sh) / dh;
        const uint16_t *sp = (const uint16_t *)(const void *)(sbase + src_y * spitch);
        uint16_t *dp = (uint16_t *)(void *)(dbase + dst_y * dpitch);
        for (int dst_x = x0; dst_x < x1; ++dst_x) {
            int src_x = sx + ((dst_x - dx) * sw) / dw;
            uint16_t px = sp[src_x];
            if (!has_ckey || px != ckey) {
                dp[dst_x] = px;
            }
        }
    }
    return 0;
}
