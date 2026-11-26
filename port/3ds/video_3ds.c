#include "platform_3ds.h"
#include "perf_3ds.h"

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDL_FMT_RGB565   0x15151002u
#define SDL_FMT_RGBA5551 0x15421002u

typedef struct {
    C3D_Tex                  tex;
    uint16_t                *staging;
    bool                     inited;
    bool                     in_vram;
    bool                     has_content;
    GPU_TEXCOLOR             fmt;
    GPU_TEXTURE_FILTER_PARAM filter;
} nox_3ds_tex_slot_t;

static bool               s_video_inited = false;
static C3D_RenderTarget  *s_top_target = NULL;
static C3D_RenderTarget  *s_bot_target = NULL;
static nox_3ds_tex_slot_t s_top_slot;
static nox_3ds_tex_slot_t s_bot_slot;
static float              s_gamma = 1.0f;

static u64               s_post_present_tick = 0;
static uint32_t          s_perf_window_ms = 0;
static uint32_t          s_perf_accum_frames = 0;
static float             s_perf_accum_cpu_ms = 0.0f;
static float             s_perf_accum_gpu_ms = 0.0f;

static void perf_sample_before_sync(void)
{
    u64 pre_sync_tick = svcGetSystemTick();
    u64 sleep_ticks = nox_3ds_perf_consume_main_sleep_ticks();
    uint32_t now_ms = nox_3ds_get_ticks_ms();

    if (s_perf_window_ms == 0)
        s_perf_window_ms = now_ms;

    if (s_post_present_tick != 0) {
        u64 wall_ticks = (pre_sync_tick > s_post_present_tick) ? (pre_sync_tick - s_post_present_tick) : 0;
        u64 active_ticks = (wall_ticks > sleep_ticks) ? (wall_ticks - sleep_ticks) : 0;
        float sw_cpu_ms = (float)((double)active_ticks / CPU_TICKS_PER_MSEC);
        float c3d_cpu_ms = C3D_GetProcessingTime();
        float gpu_ms = C3D_GetDrawingTime();
        s_perf_accum_cpu_ms += sw_cpu_ms + c3d_cpu_ms;
        s_perf_accum_gpu_ms += gpu_ms;
        s_perf_accum_frames++;
    }

    uint32_t elapsed = now_ms - s_perf_window_ms;
    if (elapsed >= 500u && s_perf_accum_frames > 0) {
        float fps = ((float)s_perf_accum_frames * 1000.0f) / (float)elapsed;
        float avg_cpu = s_perf_accum_cpu_ms / (float)s_perf_accum_frames;
        float avg_gpu = s_perf_accum_gpu_ms / (float)s_perf_accum_frames;
        nox_3ds_perf_hud_update(fps, avg_cpu, avg_gpu);
        nox_3ds_state_mut()->fps = (uint32_t)(fps + 0.5f);
        s_perf_window_ms = now_ms;
        s_perf_accum_frames = 0;
        s_perf_accum_cpu_ms = 0.0f;
        s_perf_accum_gpu_ms = 0.0f;
    }
}

static void perf_mark_after_present(void)
{
    (void)nox_3ds_perf_consume_main_sleep_ticks();
    s_post_present_tick = svcGetSystemTick();
}

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

static bool ensure_tex_slot(nox_3ds_tex_slot_t *slot, GPU_TEXCOLOR fmt, const char *label)
{
    if (slot->inited && slot->fmt == fmt)
        return true;

    if (slot->inited) {
        C3D_TexDelete(&slot->tex);
        slot->inited = false;
        slot->has_content = false;
    }

    slot->in_vram = C3D_TexInitVRAM(&slot->tex, NOX_3DS_TEX_W, NOX_3DS_TEX_H, fmt);
    if (!slot->in_vram) {
        if (!C3D_TexInit(&slot->tex, NOX_3DS_TEX_W, NOX_3DS_TEX_H, fmt)) {
            fprintf(stderr, "[video3ds] ERROR: C3D_TexInit(%s %ux%u, fmt=%d) failed\n",
                    label, NOX_3DS_TEX_W, NOX_3DS_TEX_H, (int)fmt);
            return false;
        }
    }

    slot->filter = GPU_LINEAR;
    C3D_TexSetFilter(&slot->tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&slot->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    slot->fmt = fmt;
    slot->inited = true;
    slot->has_content = false;

    fprintf(stderr, "[video3ds] %s tex %ux%u (%s, %s)\n",
            label, NOX_3DS_TEX_W, NOX_3DS_TEX_H,
            slot->in_vram ? "VRAM" : "FCRAM",
            (fmt == GPU_RGB565) ? "RGB565" : "RGBA5551");
    return true;
}

static bool prepare_frame_slot(nox_3ds_tex_slot_t *slot,
                               const nox_3ds_screen_frame_t *frame,
                               const char *label,
                               const void **out_xfer_src,
                               GX_TRANSFER_FORMAT *out_gx_fmt)
{
    if (!frame || !frame->pixels || frame->w <= 0 || frame->h <= 0 || frame->pitch <= 0)
        return false;

    GPU_TEXCOLOR gpu_fmt = gpu_fmt_for_sdl(frame->sdl_format);
    GX_TRANSFER_FORMAT gx_fmt = gx_fmt_for_sdl(frame->sdl_format);
    if (!ensure_tex_slot(slot, gpu_fmt, label))
        return false;

    int w = frame->w;
    int h = frame->h;
    if (w > NOX_3DS_TEX_W) w = NOX_3DS_TEX_W;
    if (h > NOX_3DS_TEX_H) h = NOX_3DS_TEX_H;

    const void *xfer_src = NULL;
    if (frame->is_linear && frame->pitch == NOX_3DS_TEX_W * 2) {
        GSPGPU_FlushDataCache((void *)frame->pixels, (u32)(frame->pitch * h));
        xfer_src = frame->pixels;
    } else {
        if (!slot->staging) {
            slot->staging = (uint16_t *)linearAlloc((size_t)NOX_3DS_TEX_W * (size_t)NOX_3DS_TEX_H * 2u);
            if (!slot->staging)
                return false;
            memset(slot->staging, 0, (size_t)NOX_3DS_TEX_W * (size_t)NOX_3DS_TEX_H * 2u);
        }
        const uint8_t *src_row = (const uint8_t *)frame->pixels;
        uint8_t *dst_row = (uint8_t *)slot->staging;
        const size_t copy_bytes = (size_t)w * 2u;
        for (int y = 0; y < h; ++y) {
            memcpy(dst_row, src_row, copy_bytes);
            src_row += frame->pitch;
            dst_row += NOX_3DS_TEX_W * 2;
        }
        GSPGPU_FlushDataCache(slot->staging, (u32)(NOX_3DS_TEX_W * h * 2));
        xfer_src = slot->staging;
    }

    *out_xfer_src = xfer_src;
    *out_gx_fmt = gx_fmt;
    return true;
}

static void enqueue_frame_upload(nox_3ds_tex_slot_t *slot,
                                 const void *xfer_src,
                                 GX_TRANSFER_FORMAT gx_fmt)
{
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
        (u32 *)slot->tex.data,
        GX_BUFFER_DIM(NOX_3DS_TEX_W, NOX_3DS_TEX_H),
        xfer_flags
    );
    slot->has_content = true;
}

static void build_subtex(Tex3DS_SubTexture *subtex, const nox_3ds_screen_frame_t *frame)
{
    int cx = frame->crop_x;
    int cy = frame->crop_y;
    int cw = (frame->crop_w > 0) ? frame->crop_w : frame->w;
    int ch = (frame->crop_h > 0) ? frame->crop_h : frame->h;

    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx + cw > NOX_3DS_TEX_W) cw = NOX_3DS_TEX_W - cx;
    if (cy + ch > NOX_3DS_TEX_H) ch = NOX_3DS_TEX_H - cy;
    if (cw <= 0) cw = 1;
    if (ch <= 0) ch = 1;

    /*
     * PICA200 / citro2d maps texture row 0 to V = 1.0 (top) and row (TEX_H - 1)
     * to V = 0.0 (bottom). With GX_TRANSFER_FLIP_VERT(0), source row y lands at
     * V = (TEX_H - y) / TEX_H. Keeping top > bottom ensures subtex is unrotated.
     */
    subtex->width  = (u16)cw;
    subtex->height = (u16)ch;
    subtex->left   = (float)cx / (float)NOX_3DS_TEX_W;
    subtex->right  = (float)(cx + cw) / (float)NOX_3DS_TEX_W;
    subtex->top    = (float)(NOX_3DS_TEX_H - cy) / (float)NOX_3DS_TEX_H;
    subtex->bottom = (float)(NOX_3DS_TEX_H - (cy + ch)) / (float)NOX_3DS_TEX_H;
}

static void draw_screen_pass(C3D_RenderTarget *target,
                             nox_3ds_tex_slot_t *slot,
                             const nox_3ds_screen_frame_t *frame,
                             int target_w, int target_h,
                             u32 clear_color,
                             int screen_id)
{
    C2D_TargetClear(target, clear_color);
    C2D_SceneBegin(target);

    if (!frame || !slot->inited) {
        if (screen_id == NOX_3DS_SWBUF_BOTTOM) {
            nox_3ds_font_render_screen(screen_id, 0, 0, NOX_3DS_LOGICAL_W, NOX_3DS_LOGICAL_H,
                                       0.0f, 0.0f, (float)target_w, (float)target_h, s_gamma);
        }
        return;
    }

    Tex3DS_SubTexture subtex;
    build_subtex(&subtex, frame);

    /* Use crisp nearest-neighbor when rendering native 1:1 400x240 gameplay,
     * and smooth bilinear filtering when scaling 640x480 -> 400x240 or 320x240. */
    GPU_TEXTURE_FILTER_PARAM want_filter =
        (subtex.width == (u16)target_w && subtex.height == (u16)target_h)
            ? GPU_NEAREST
            : GPU_LINEAR;
    if (slot->filter != want_filter) {
        C3D_TexSetFilter(&slot->tex, want_filter, want_filter);
        slot->filter = want_filter;
    }

    float dst_w = (float)target_w;
    float dst_h = (float)target_h;
    const nox_3ds_state_t *st = nox_3ds_state_mut();
    bool fill_screen = frame->fill_screen && (target != s_top_target || st->wide_stretch);
    if (!fill_screen && subtex.height > 0) {
        float src_aspect = (float)subtex.width / (float)subtex.height;
        float dst_aspect = dst_w / dst_h;
        if (src_aspect < dst_aspect) {
            dst_w = dst_h * src_aspect;
        } else if (src_aspect > dst_aspect) {
            dst_h = dst_w / src_aspect;
        }
    }

    float draw_x  = ((float)target_w - dst_w) * 0.5f;
    float draw_y  = ((float)target_h - dst_h) * 0.5f;
    float scale_x = dst_w / (float)subtex.width;
    float scale_y = dst_h / (float)subtex.height;

    C2D_Image img;
    img.tex    = &slot->tex;
    img.subtex = &subtex;

    if (s_gamma < 0.98f) {
        C2D_ImageTint tint;
        C2D_SetTintMode(C2D_TintMult);
        C2D_PlainImageTint(&tint, C2D_Color32f(s_gamma, s_gamma, s_gamma, 1.0f), 1.0f);
        C2D_DrawImageAt(img, draw_x, draw_y, 0.0f, &tint, scale_x, scale_y);
    } else {
        C2D_DrawImageAt(img, draw_x, draw_y, 0.0f, NULL, scale_x, scale_y);
    }

    if (screen_id >= 0) {
        int cx = (frame->crop_x >= 0) ? frame->crop_x : 0;
        int cy = (frame->crop_y >= 0) ? frame->crop_y : 0;
        int cw = (frame->crop_w > 0) ? frame->crop_w : frame->w;
        int ch = (frame->crop_h > 0) ? frame->crop_h : frame->h;
        nox_3ds_font_render_screen(screen_id, cx, cy, cw, ch,
                                   draw_x, draw_y, dst_w, dst_h, s_gamma);
    } else if (target == s_top_target) {
        nox_3ds_perf_hud_draw_top(target_w, target_h);
    }
}

bool nox_3ds_video_init(int logical_w, int logical_h)
{
    (void)logical_w;
    (void)logical_h;

    if (s_video_inited)
        return true;

    /* Hand GFX_BOTTOM cleanly from boot PrintConsole to citro2d BGR8 double-buffer. */
    nox_3ds_console_detach();

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
    s_bot_target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    if (!s_top_target || !s_bot_target) {
        fprintf(stderr, "[video3ds] ERROR: C2D_CreateScreenTarget failed (top=%p bot=%p)\n",
                (void *)s_top_target, (void *)s_bot_target);
        C2D_Fini();
        C3D_Fini();
        s_top_target = NULL;
        s_bot_target = NULL;
        return false;
    }

    memset(&s_top_slot, 0, sizeof(s_top_slot));
    memset(&s_bot_slot, 0, sizeof(s_bot_slot));

    if (!ensure_tex_slot(&s_top_slot, GPU_RGBA5551, "Top") ||
        !ensure_tex_slot(&s_bot_slot, GPU_RGBA5551, "Bottom")) {
        nox_3ds_video_shutdown();
        return false;
    }

    nox_3ds_font_init();

    s_post_present_tick = 0;
    s_perf_window_ms = nox_3ds_get_ticks_ms();
    s_perf_accum_frames = 0;
    s_perf_accum_cpu_ms = 0.0f;
    s_perf_accum_gpu_ms = 0.0f;
    s_video_inited = true;

    fprintf(stderr, "[video3ds] Dual-screen citro2d active: Top 400x240 + Bottom 320x240\n");
    return true;
}

void nox_3ds_video_shutdown(void)
{
    if (!s_video_inited && !s_top_target && !s_bot_target)
        return;

    nox_3ds_font_shutdown();

    if (s_top_slot.staging) {
        linearFree(s_top_slot.staging);
        s_top_slot.staging = NULL;
    }
    if (s_bot_slot.staging) {
        linearFree(s_bot_slot.staging);
        s_bot_slot.staging = NULL;
    }
    if (s_top_slot.inited) {
        C3D_TexDelete(&s_top_slot.tex);
        s_top_slot.inited = false;
        s_top_slot.has_content = false;
    }
    if (s_bot_slot.inited) {
        C3D_TexDelete(&s_bot_slot.tex);
        s_bot_slot.inited = false;
        s_bot_slot.has_content = false;
    }
    s_top_target = NULL;
    s_bot_target = NULL;
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
}

void nox_3ds_video_present_dual(const nox_3ds_screen_frame_t *top,
                                const nox_3ds_screen_frame_t *bottom)
{
    if (!s_video_inited) {
        if (!nox_3ds_video_init(NOX_3DS_LOGICAL_W, NOX_3DS_LOGICAL_H))
            return;
    }

    const void *top_src = NULL;
    const void *bot_src = NULL;
    GX_TRANSFER_FORMAT top_gx_fmt = GX_TRANSFER_FMT_RGB5A1;
    GX_TRANSFER_FORMAT bot_gx_fmt = GX_TRANSFER_FMT_RGB5A1;
    bool top_ready = prepare_frame_slot(&s_top_slot, top, "Top", &top_src, &top_gx_fmt);
    bool bot_reuse = (bottom && bottom->reuse_tex && s_bot_slot.inited && s_bot_slot.has_content);
    bool bot_upload = bot_reuse
        ? false
        : prepare_frame_slot(&s_bot_slot, bottom, "Bottom", &bot_src, &bot_gx_fmt);
    bool bot_draw = bot_reuse || bot_upload;

    perf_sample_before_sync();

    if (C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) {
        nox_3ds_font_upload_pending();
        if (top_ready)
            enqueue_frame_upload(&s_top_slot, top_src, top_gx_fmt);
        if (bot_upload)
            enqueue_frame_upload(&s_bot_slot, bot_src, bot_gx_fmt);

        draw_screen_pass(
            s_top_target, &s_top_slot,
            top_ready ? top : NULL,
            NOX_3DS_TOP_W, NOX_3DS_TOP_H,
            C2D_Color32(0, 0, 0, 255),
            NOX_3DS_SWBUF_TOP
        );

        draw_screen_pass(
            s_bot_target, &s_bot_slot,
            bot_draw ? bottom : NULL,
            NOX_3DS_BOT_W, NOX_3DS_BOT_H,
            C2D_Color32(16, 16, 20, 255),
            NOX_3DS_SWBUF_BOTTOM
        );

        C3D_FrameEnd(0);
    }

    perf_mark_after_present();
}

void nox_3ds_video_present(const void *pixels, int w, int h, int pitch,
                           uint32_t sdl_format, bool is_linear)
{
    nox_3ds_screen_frame_t top;
    memset(&top, 0, sizeof(top));
    top.pixels      = pixels;
    top.w           = w;
    top.h           = h;
    top.pitch       = pitch;
    top.sdl_format  = sdl_format;
    top.is_linear   = is_linear;
    top.crop_x      = 0;
    top.crop_y      = 0;
    top.crop_w      = w;
    top.crop_h      = h;
    top.fill_screen = true;
    nox_3ds_video_present_dual(&top, NULL);
}

/*
 * Dedicated hardware-scaled movie frame bridge:
 * Uploads the engine's 640x480 software-decoded linear buffer directly to the
 * 1024x512 PICA200 VRAM texture via GX/C3D_SyncDisplayTransfer (zero CPU scale
 * and zero CPU copy when pitch == 2048 & linearAlloc), then hardware-scales
 * the texture down to 400x240 on the Top Screen via citro2d GPU_LINEAR.
 * Bottom Screen is cleared to black with zero texture upload overhead.
 */
void nox_3ds_video_present_movie(const void *pixels, int w, int h, int pitch,
                                 uint32_t sdl_format, bool is_linear,
                                 int crop_x, int crop_y, int crop_w, int crop_h)
{
    if (!s_video_inited) {
        if (!nox_3ds_video_init(NOX_3DS_LOGICAL_W, NOX_3DS_LOGICAL_H))
            return;
    }

    nox_3ds_screen_frame_t top;
    memset(&top, 0, sizeof(top));
    top.pixels      = pixels;
    top.w           = w;
    top.h           = h;
    top.pitch       = pitch;
    top.sdl_format  = sdl_format;
    top.is_linear   = is_linear;
    top.crop_x      = (crop_x >= 0) ? crop_x : 0;
    top.crop_y      = (crop_y >= 0) ? crop_y : 0;
    top.crop_w      = (crop_w > 0) ? crop_w : w;
    top.crop_h      = (crop_h > 0) ? crop_h : h;
    top.fill_screen = true;

    const void *top_src = NULL;
    GX_TRANSFER_FORMAT top_gx_fmt = GX_TRANSFER_FMT_RGB5A1;
    bool top_ready = prepare_frame_slot(&s_top_slot, &top, "MovieTop", &top_src, &top_gx_fmt);

    /* Ensure full brightness during cutscenes regardless of prior menu fade gamma. */
    float prev_gamma = s_gamma;
    s_gamma = 1.0f;

    perf_sample_before_sync();

    if (C3D_FrameBegin(C3D_FRAME_SYNCDRAW)) {
        nox_3ds_font_upload_pending();
        if (top_ready)
            enqueue_frame_upload(&s_top_slot, top_src, top_gx_fmt);

        draw_screen_pass(
            s_top_target, &s_top_slot,
            top_ready ? &top : NULL,
            NOX_3DS_TOP_W, NOX_3DS_TOP_H,
            C2D_Color32(0, 0, 0, 255),
            -1
        );

        C2D_TargetClear(s_bot_target, C2D_Color32(0, 0, 0, 255));
        C2D_SceneBegin(s_bot_target);

        C3D_FrameEnd(0);
    }

    s_gamma = prev_gamma;

    perf_mark_after_present();
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
