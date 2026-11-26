/*
 * Smart Auto-Fit Native 1:1 PICA200 / citro2d Pixel Font Renderer for Nox 3DS
 *
 * - large.fnt (slot 1): Authentic supersampled Nox medieval serif glyphs baked
 *   directly from large.fnt at 0.5x (Bottom) and 0.625x (Top) with 100% exact
 *   stock metrics (zero layout shift).
 * - default.fnt / number.fnt / small.fnt (slots 0, 2, 3): Razor-sharp 1:1
 *   proportional 3x5 pixel font (GBA/NDS RPG style) with per-widget smart
 *   auto-fit: bounded Bottom Screen widgets that would wrap or overflow in 3x5
 *   automatically fall back to stock software text (with per-list uniformity
 *   for listboxes), while Top Screen text, scrollable Quest/NPC dialogs,
 *   buttons, tooltips, speech bubbles, and headers get crisp 1:1 GPU glyphs.
 *
 * Uses Nox's unused RGB555 bit 15 (0x8000) as a pixel-exact occlusion stencil.
 * Default: OFF (stock software). Live toggle: L + R + SELECT.
 */

#include "platform_3ds.h"
#include "nox_3ds_version.h"

#ifndef NOX_3DS_VERSION_STR
#define NOX_3DS_VERSION_STR "3ds-dev"
#endif

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ATLAS_W           256
#define ATLAS_H           256
#define ATLAS_COLS        16
#define FONT_SLOTS        4
#define FONT_GLYPH_COUNT  95   /* ASCII 0x20..0x7E */
#define FONT_SCREENS      2
#define FONT_MAX_GLYPHS   1800
#define TOAST_MS          2000u

/* Atlas layout inside 256x256 GPU_RGBA8 (256 KB VRAM):
 *   y =   0.. 47, x = 0..127 : crisp 3x5 proportional pixel font (8x8 cells)
 *   y =  64..147, x = 0..255 : authentic large.fnt Bottom 0.5x   (16x14 cells)
 *   y = 152..247, x = 0..255 : authentic large.fnt Top    0.625x (16x16 cells)
 */
#define CRISP_BASE_X      0
#define CRISP_BASE_Y      0
#define CRISP_CELL_W      8
#define CRISP_CELL_H      8
#define LARGE_BOT_BASE_X  0
#define LARGE_BOT_BASE_Y  64
#define LARGE_BOT_CELL_W  16
#define LARGE_BOT_CELL_H  14
#define LARGE_TOP_BASE_X  0
#define LARGE_TOP_BASE_Y  152
#define LARGE_TOP_CELL_W  16
#define LARGE_TOP_CELL_H  16

typedef enum {
    TIER_CRISP     = 0, /* 3x5 cap + 1px descender 1:1 proportional pixel font */
    TIER_LARGE_BOT = 1, /* Nox's real large.fnt supersampled 0.5x for Bottom 320x240 */
    TIER_LARGE_TOP = 2  /* Nox's real large.fnt supersampled 0.625x for Top 400x240 */
} font_tier_t;

typedef struct {
    uint8_t w;       /* glyph bitmap width in physical pixels (0..5) */
    uint8_t rows[6]; /* MSB-first in bits 7..3 (bit 7 = leftmost pixel) */
} crisp_glyph_t;

typedef struct {
    uint16_t ch;
    int16_t  x;          /* logical 640x480 X */
    int16_t  y;          /* logical 640x480 Y */
    int16_t  clip_l;
    int16_t  clip_t;
    int16_t  clip_r;
    int16_t  clip_b;
    int16_t  probe0_x;   /* bit-15 occlusion probe 0 in 640x480 SW buffer */
    int16_t  probe0_y;
    int16_t  probe1_x;   /* bit-15 occlusion probe 1 in 640x480 SW buffer */
    int16_t  probe1_y;
    uint16_t rgb555;
    uint8_t  tier;       /* font_tier_t */
    uint8_t  clip_en;
    uint8_t  visible;    /* set during bit-15 occlusion resolve */
    uint8_t  _pad;
} queued_glyph_t;

/* Crisp hand-tuned proportional 3x5 (+1px descender) pixel font (ASCII 0x20..0x7E) */
static const crisp_glyph_t s_crisp_font[FONT_GLYPH_COUNT] = {
    /* 0x20 ' ' */ { 1, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
    /* 0x21 '!' */ { 1, { 0x80, 0x80, 0x80, 0x00, 0x80, 0x00 } },
    /* 0x22 '"' */ { 3, { 0xA0, 0xA0, 0x00, 0x00, 0x00, 0x00 } },
    /* 0x23 '#' */ { 5, { 0x50, 0xF8, 0x50, 0xF8, 0x50, 0x00 } },
    /* 0x24 '$' */ { 4, { 0x60, 0xD0, 0x60, 0xB0, 0x60, 0x00 } },
    /* 0x25 '%' */ { 3, { 0xA0, 0x20, 0x40, 0x80, 0xA0, 0x00 } },
    /* 0x26 '&' */ { 4, { 0x60, 0x90, 0x60, 0xA0, 0x50, 0x00 } },
    /* 0x27 ''' */ { 1, { 0x80, 0x80, 0x00, 0x00, 0x00, 0x00 } },
    /* 0x28 '(' */ { 2, { 0x40, 0x80, 0x80, 0x80, 0x40, 0x00 } },
    /* 0x29 ')' */ { 2, { 0x80, 0x40, 0x40, 0x40, 0x80, 0x00 } },
    /* 0x2A '*' */ { 3, { 0xA0, 0x40, 0xA0, 0x00, 0x00, 0x00 } },
    /* 0x2B '+' */ { 3, { 0x00, 0x40, 0xE0, 0x40, 0x00, 0x00 } },
    /* 0x2C ',' */ { 2, { 0x00, 0x00, 0x00, 0x40, 0x40, 0x80 } },
    /* 0x2D '-' */ { 2, { 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00 } },
    /* 0x2E '.' */ { 1, { 0x00, 0x00, 0x00, 0x00, 0x80, 0x00 } },
    /* 0x2F '/' */ { 3, { 0x20, 0x20, 0x40, 0x80, 0x80, 0x00 } },
    /* 0x30 '0' */ { 3, { 0xE0, 0xA0, 0xA0, 0xA0, 0xE0, 0x00 } },
    /* 0x31 '1' */ { 2, { 0x40, 0xC0, 0x40, 0x40, 0x40, 0x00 } },
    /* 0x32 '2' */ { 3, { 0xE0, 0x20, 0xE0, 0x80, 0xE0, 0x00 } },
    /* 0x33 '3' */ { 3, { 0xE0, 0x20, 0x60, 0x20, 0xE0, 0x00 } },
    /* 0x34 '4' */ { 3, { 0xA0, 0xA0, 0xE0, 0x20, 0x20, 0x00 } },
    /* 0x35 '5' */ { 3, { 0xE0, 0x80, 0xE0, 0x20, 0xE0, 0x00 } },
    /* 0x36 '6' */ { 3, { 0xE0, 0x80, 0xE0, 0xA0, 0xE0, 0x00 } },
    /* 0x37 '7' */ { 3, { 0xE0, 0x20, 0x20, 0x40, 0x40, 0x00 } },
    /* 0x38 '8' */ { 3, { 0xE0, 0xA0, 0xE0, 0xA0, 0xE0, 0x00 } },
    /* 0x39 '9' */ { 3, { 0xE0, 0xA0, 0xE0, 0x20, 0xE0, 0x00 } },
    /* 0x3A ':' */ { 1, { 0x00, 0x80, 0x00, 0x80, 0x00, 0x00 } },
    /* 0x3B ';' */ { 2, { 0x00, 0x40, 0x00, 0x40, 0x40, 0x80 } },
    /* 0x3C '<' */ { 3, { 0x20, 0x40, 0x80, 0x40, 0x20, 0x00 } },
    /* 0x3D '=' */ { 3, { 0x00, 0xE0, 0x00, 0xE0, 0x00, 0x00 } },
    /* 0x3E '>' */ { 3, { 0x80, 0x40, 0x20, 0x40, 0x80, 0x00 } },
    /* 0x3F '?' */ { 3, { 0xE0, 0x20, 0x60, 0x00, 0x40, 0x00 } },
    /* 0x40 '@' */ { 5, { 0x70, 0x88, 0xB8, 0x80, 0x70, 0x00 } },
    /* 0x41 'A' */ { 3, { 0x40, 0xA0, 0xE0, 0xA0, 0xA0, 0x00 } },
    /* 0x42 'B' */ { 3, { 0xC0, 0xA0, 0xC0, 0xA0, 0xC0, 0x00 } },
    /* 0x43 'C' */ { 3, { 0x60, 0x80, 0x80, 0x80, 0x60, 0x00 } },
    /* 0x44 'D' */ { 3, { 0xC0, 0xA0, 0xA0, 0xA0, 0xC0, 0x00 } },
    /* 0x45 'E' */ { 3, { 0xE0, 0x80, 0xC0, 0x80, 0xE0, 0x00 } },
    /* 0x46 'F' */ { 3, { 0xE0, 0x80, 0xC0, 0x80, 0x80, 0x00 } },
    /* 0x47 'G' */ { 4, { 0x70, 0x80, 0xB0, 0x90, 0x70, 0x00 } },
    /* 0x48 'H' */ { 3, { 0xA0, 0xA0, 0xE0, 0xA0, 0xA0, 0x00 } },
    /* 0x49 'I' */ { 3, { 0xE0, 0x40, 0x40, 0x40, 0xE0, 0x00 } },
    /* 0x4A 'J' */ { 3, { 0x20, 0x20, 0x20, 0xA0, 0x40, 0x00 } },
    /* 0x4B 'K' */ { 3, { 0xA0, 0xA0, 0xC0, 0xA0, 0xA0, 0x00 } },
    /* 0x4C 'L' */ { 3, { 0x80, 0x80, 0x80, 0x80, 0xE0, 0x00 } },
    /* 0x4D 'M' */ { 5, { 0x88, 0xD8, 0xA8, 0x88, 0x88, 0x00 } },
    /* 0x4E 'N' */ { 4, { 0x90, 0xD0, 0xB0, 0x90, 0x90, 0x00 } },
    /* 0x4F 'O' */ { 3, { 0x40, 0xA0, 0xA0, 0xA0, 0x40, 0x00 } },
    /* 0x50 'P' */ { 3, { 0xC0, 0xA0, 0xC0, 0x80, 0x80, 0x00 } },
    /* 0x51 'Q' */ { 4, { 0x60, 0x90, 0x90, 0xB0, 0x70, 0x00 } },
    /* 0x52 'R' */ { 3, { 0xC0, 0xA0, 0xC0, 0xA0, 0xA0, 0x00 } },
    /* 0x53 'S' */ { 3, { 0x60, 0x80, 0x40, 0x20, 0xC0, 0x00 } },
    /* 0x54 'T' */ { 3, { 0xE0, 0x40, 0x40, 0x40, 0x40, 0x00 } },
    /* 0x55 'U' */ { 3, { 0xA0, 0xA0, 0xA0, 0xA0, 0xE0, 0x00 } },
    /* 0x56 'V' */ { 3, { 0xA0, 0xA0, 0xA0, 0xA0, 0x40, 0x00 } },
    /* 0x57 'W' */ { 5, { 0x88, 0x88, 0xA8, 0xD8, 0x88, 0x00 } },
    /* 0x58 'X' */ { 3, { 0xA0, 0xA0, 0x40, 0xA0, 0xA0, 0x00 } },
    /* 0x59 'Y' */ { 3, { 0xA0, 0xA0, 0x40, 0x40, 0x40, 0x00 } },
    /* 0x5A 'Z' */ { 3, { 0xE0, 0x20, 0x40, 0x80, 0xE0, 0x00 } },
    /* 0x5B '[' */ { 2, { 0xC0, 0x80, 0x80, 0x80, 0xC0, 0x00 } },
    /* 0x5C '\' */ { 3, { 0x80, 0x80, 0x40, 0x20, 0x20, 0x00 } },
    /* 0x5D ']' */ { 2, { 0xC0, 0x40, 0x40, 0x40, 0xC0, 0x00 } },
    /* 0x5E '^' */ { 3, { 0x40, 0xA0, 0x00, 0x00, 0x00, 0x00 } },
    /* 0x5F '_' */ { 3, { 0x00, 0x00, 0x00, 0x00, 0xE0, 0x00 } },
    /* 0x60 '`' */ { 2, { 0x80, 0x40, 0x00, 0x00, 0x00, 0x00 } },
    /* 0x61 'a' */ { 3, { 0x00, 0x60, 0x20, 0xA0, 0xE0, 0x00 } },
    /* 0x62 'b' */ { 3, { 0x80, 0xC0, 0xA0, 0xA0, 0xC0, 0x00 } },
    /* 0x63 'c' */ { 3, { 0x00, 0x60, 0x80, 0x80, 0x60, 0x00 } },
    /* 0x64 'd' */ { 3, { 0x20, 0x60, 0xA0, 0xA0, 0x60, 0x00 } },
    /* 0x65 'e' */ { 3, { 0x00, 0x40, 0xE0, 0x80, 0x60, 0x00 } },
    /* 0x66 'f' */ { 2, { 0x40, 0x80, 0xC0, 0x80, 0x80, 0x00 } },
    /* 0x67 'g' */ { 3, { 0x00, 0x60, 0xA0, 0x60, 0x20, 0xC0 } },
    /* 0x68 'h' */ { 3, { 0x80, 0xC0, 0xA0, 0xA0, 0xA0, 0x00 } },
    /* 0x69 'i' */ { 1, { 0x80, 0x00, 0x80, 0x80, 0x80, 0x00 } },
    /* 0x6A 'j' */ { 2, { 0x40, 0x00, 0x40, 0x40, 0x40, 0x80 } },
    /* 0x6B 'k' */ { 3, { 0x80, 0xA0, 0xC0, 0xA0, 0xA0, 0x00 } },
    /* 0x6C 'l' */ { 1, { 0x80, 0x80, 0x80, 0x80, 0x80, 0x00 } },
    /* 0x6D 'm' */ { 5, { 0x00, 0xD0, 0xA8, 0xA8, 0xA8, 0x00 } },
    /* 0x6E 'n' */ { 3, { 0x00, 0xC0, 0xA0, 0xA0, 0xA0, 0x00 } },
    /* 0x6F 'o' */ { 3, { 0x00, 0x40, 0xA0, 0xA0, 0x40, 0x00 } },
    /* 0x70 'p' */ { 3, { 0x00, 0xC0, 0xA0, 0xC0, 0x80, 0x80 } },
    /* 0x71 'q' */ { 3, { 0x00, 0x60, 0xA0, 0x60, 0x20, 0x20 } },
    /* 0x72 'r' */ { 2, { 0x00, 0xC0, 0x80, 0x80, 0x80, 0x00 } },
    /* 0x73 's' */ { 3, { 0x00, 0x60, 0xC0, 0x20, 0xC0, 0x00 } },
    /* 0x74 't' */ { 2, { 0x80, 0xC0, 0x80, 0x80, 0x40, 0x00 } },
    /* 0x75 'u' */ { 3, { 0x00, 0xA0, 0xA0, 0xA0, 0x60, 0x00 } },
    /* 0x76 'v' */ { 3, { 0x00, 0xA0, 0xA0, 0xA0, 0x40, 0x00 } },
    /* 0x77 'w' */ { 5, { 0x00, 0xA8, 0xA8, 0xA8, 0x50, 0x00 } },
    /* 0x78 'x' */ { 3, { 0x00, 0xA0, 0x40, 0x40, 0xA0, 0x00 } },
    /* 0x79 'y' */ { 3, { 0x00, 0xA0, 0xA0, 0x60, 0x20, 0xC0 } },
    /* 0x7A 'z' */ { 3, { 0x00, 0xE0, 0x20, 0x80, 0xE0, 0x00 } },
    /* 0x7B '{' */ { 3, { 0x60, 0x40, 0x80, 0x40, 0x60, 0x00 } },
    /* 0x7C '|' */ { 1, { 0x80, 0x80, 0x80, 0x80, 0x80, 0x00 } },
    /* 0x7D '}' */ { 3, { 0xC0, 0x40, 0x20, 0x40, 0xC0, 0x00 } },
    /* 0x7E '~' */ { 4, { 0x00, 0x50, 0xA0, 0x00, 0x00, 0x00 } }
};

static C3D_Tex        s_atlas_tex;
static uint32_t      *s_atlas_staging = NULL;
static bool           s_staging_baked = false;
static bool           s_atlas_inited = false;
static bool           s_atlas_uploaded = false;
static bool           s_native_enabled = false;
static int            s_fallback_depth = 0;
static uint32_t       s_toast_until_ms = 0;
static bool           s_show_version_overlay = false;

/* Stock .fnt metrics recorded from all 4 slots for exact auto-fit comparison */
static bool           s_stock_loaded[FONT_SLOTS];
static int            s_stock_h[FONT_SLOTS] = { 10, 18, 12, 8 };
static uint8_t        s_stock_w[FONT_SLOTS][FONT_GLYPH_COUNT];

/* Supersampled authentic large.fnt (slot 1) physical dimensions */
static bool           s_large_loaded = false;
static uint8_t        s_large_bot_w[FONT_GLYPH_COUNT];
static uint8_t        s_large_bot_h[FONT_GLYPH_COUNT];
static uint8_t        s_large_top_w[FONT_GLYPH_COUNT];
static uint8_t        s_large_top_h[FONT_GLYPH_COUNT];

static queued_glyph_t s_queues[FONT_SCREENS][FONT_MAX_GLYPHS];
static int            s_counts[FONT_SCREENS] = { 0, 0 };

static font_tier_t tier_for_slot(int font_slot, bool is_bottom)
{
    if (font_slot == 1 && s_large_loaded)
        return is_bottom ? TIER_LARGE_BOT : TIER_LARGE_TOP;
    return TIER_CRISP;
}

static bool ensure_staging_baked(void)
{
    if (s_staging_baked && s_atlas_staging)
        return true;

    size_t staging_bytes = (size_t)ATLAS_W * (size_t)ATLAS_H * 4u;
    if (!s_atlas_staging) {
        s_atlas_staging = (uint32_t *)linearAlloc(staging_bytes);
        if (!s_atlas_staging)
            return false;
        memset(s_atlas_staging, 0, staging_bytes);
    }

    /* Bake crisp 3x5 proportional pixel glyphs into x=0..127, y=0..47 */
    for (int i = 0; i < FONT_GLYPH_COUNT; ++i) {
        int col = i % ATLAS_COLS;
        int row = i / ATLAS_COLS;
        int bx = CRISP_BASE_X + col * CRISP_CELL_W;
        int by = CRISP_BASE_Y + row * CRISP_CELL_H;
        const crisp_glyph_t *g = &s_crisp_font[i];
        for (int y = 0; y < 6; ++y) {
            uint8_t bits = g->rows[y];
            for (int x = 0; x < g->w; ++x) {
                if (bits & (uint8_t)(0x80u >> x)) {
                    s_atlas_staging[(size_t)(by + y) * ATLAS_W + (size_t)(bx + x)] = 0xFFFFFFFFu;
                }
            }
        }
    }

    s_staging_baked = true;
    return true;
}

static inline int sample_fnt_bit(const uint8_t *bits, int row_stride, int src_w, int src_h, int x, int y)
{
    if (x < 0 || x >= src_w || y < 0 || y >= src_h)
        return 0;
    return (bits[y * row_stride + (x >> 3)] & (uint8_t)(0x80u >> (x & 7))) ? 1 : 0;
}

void nox_3ds_font_begin_slot(int font_slot, int src_h, int line_pitch)
{
    if (font_slot < 0 || font_slot >= FONT_SLOTS)
        return;

    s_stock_loaded[font_slot] = false;
    s_stock_h[font_slot] = (line_pitch > 0) ? line_pitch : ((src_h > 0) ? src_h : 10);
    memset(s_stock_w[font_slot], 0, sizeof(s_stock_w[font_slot]));

    if (font_slot == 1) {
        if (!ensure_staging_baked())
            return;
        memset(s_atlas_staging + (size_t)LARGE_BOT_BASE_Y * ATLAS_W, 0,
               (size_t)(ATLAS_H - LARGE_BOT_BASE_Y) * ATLAS_W * sizeof(uint32_t));
        memset(s_large_bot_w, 0, sizeof(s_large_bot_w));
        memset(s_large_bot_h, 0, sizeof(s_large_bot_h));
        memset(s_large_top_w, 0, sizeof(s_large_top_w));
        memset(s_large_top_h, 0, sizeof(s_large_top_h));
        s_large_loaded = false;
    }
}

void nox_3ds_font_add_glyph(int font_slot, uint16_t ch, int src_w,
                            const uint8_t *bits, int row_stride, int src_h)
{
    if (font_slot < 0 || font_slot >= FONT_SLOTS)
        return;
    if (ch < 0x20 || ch > 0x7E || src_w < 0)
        return;

    int idx = (int)(ch - 0x20);
    s_stock_w[font_slot][idx] = (uint8_t)((src_w > 255) ? 255 : src_w);

    /* Only slot 1 (large.fnt) is supersampled into the atlas; slots 0, 2, 3
     * use the crisp 3x5 proportional pixel font with smart auto-fit. */
    if (font_slot != 1 || !s_atlas_staging || !bits || src_w == 0 || src_h <= 0 || row_stride <= 0)
        return;

    int col = idx % ATLAS_COLS;
    int row = idx / ATLAS_COLS;

    /* 1) Bottom Screen (0.5x = exact 2x2 area box filter) */
    {
        static const uint8_t k_alpha4[5] = { 0, 115, 200, 242, 255 };
        int dw = (src_w + 1) / 2;
        int dh = (src_h + 1) / 2;
        if (dw > LARGE_BOT_CELL_W - 1) dw = LARGE_BOT_CELL_W - 1;
        if (dh > LARGE_BOT_CELL_H - 1) dh = LARGE_BOT_CELL_H - 1;
        s_large_bot_w[idx] = (uint8_t)dw;
        s_large_bot_h[idx] = (uint8_t)dh;

        int bx = LARGE_BOT_BASE_X + col * LARGE_BOT_CELL_W;
        int by = LARGE_BOT_BASE_Y + row * LARGE_BOT_CELL_H;
        for (int dy = 0; dy < dh; ++dy) {
            for (int dx = 0; dx < dw; ++dx) {
                int hits = sample_fnt_bit(bits, row_stride, src_w, src_h, dx * 2,     dy * 2)
                         + sample_fnt_bit(bits, row_stride, src_w, src_h, dx * 2 + 1, dy * 2)
                         + sample_fnt_bit(bits, row_stride, src_w, src_h, dx * 2,     dy * 2 + 1)
                         + sample_fnt_bit(bits, row_stride, src_w, src_h, dx * 2 + 1, dy * 2 + 1);
                uint8_t a = k_alpha4[hits];
                if (a > 0) {
                    s_atlas_staging[(size_t)(by + dy) * ATLAS_W + (size_t)(bx + dx)] = 0xFFFFFF00u | (uint32_t)a;
                }
            }
        }
    }

    /* 2) Top Screen (0.625x = exact 1.6x1.6 area-coverage resample) */
    {
        int dw = (src_w * 5 + 4) / 8;
        int dh = (src_h * 5 + 4) / 8;
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        if (dw > LARGE_TOP_CELL_W - 1) dw = LARGE_TOP_CELL_W - 1;
        if (dh > LARGE_TOP_CELL_H - 1) dh = LARGE_TOP_CELL_H - 1;
        s_large_top_w[idx] = (uint8_t)dw;
        s_large_top_h[idx] = (uint8_t)dh;

        int bx = LARGE_TOP_BASE_X + col * LARGE_TOP_CELL_W;
        int by = LARGE_TOP_BASE_Y + row * LARGE_TOP_CELL_H;
        const float step = 1.6f;
        const float inv_area = 1.0f / (step * step);

        for (int dy = 0; dy < dh; ++dy) {
            float sy0 = (float)dy * step;
            float sy1 = sy0 + step;
            int iy0 = (int)floorf(sy0);
            int iy1 = (int)ceilf(sy1);
            if (iy1 > src_h) iy1 = src_h;

            for (int dx = 0; dx < dw; ++dx) {
                float sx0 = (float)dx * step;
                float sx1 = sx0 + step;
                int ix0 = (int)floorf(sx0);
                int ix1 = (int)ceilf(sx1);
                if (ix1 > src_w) ix1 = src_w;

                float area = 0.0f;
                for (int sy = iy0; sy < iy1; ++sy) {
                    float wy = fminf(sy1, (float)(sy + 1)) - fmaxf(sy0, (float)sy);
                    if (wy <= 0.0f) continue;
                    for (int sx = ix0; sx < ix1; ++sx) {
                        if (sample_fnt_bit(bits, row_stride, src_w, src_h, sx, sy)) {
                            float wx = fminf(sx1, (float)(sx + 1)) - fmaxf(sx0, (float)sx);
                            if (wx > 0.0f)
                                area += wx * wy;
                        }
                    }
                }
                if (area > 0.0f) {
                    float cov = area * inv_area;
                    if (cov > 1.0f) cov = 1.0f;
                    int ai = (int)(powf(cov, 0.72f) * 255.0f + 0.5f);
                    if (ai > 255) ai = 255;
                    if (ai > 0) {
                        s_atlas_staging[(size_t)(by + dy) * ATLAS_W + (size_t)(bx + dx)] = 0xFFFFFF00u | (uint32_t)ai;
                    }
                }
            }
        }
    }
}

void nox_3ds_font_finish_slot(int font_slot)
{
    if (font_slot < 0 || font_slot >= FONT_SLOTS)
        return;
    s_stock_loaded[font_slot] = true;

    if (font_slot == 1 && s_atlas_staging) {
        size_t staging_bytes = (size_t)ATLAS_W * (size_t)ATLAS_H * 4u;
        GSPGPU_FlushDataCache(s_atlas_staging, (u32)staging_bytes);
        s_large_loaded = true;
        s_atlas_uploaded = false;
        fprintf(stderr, "[font3ds] Baked authentic large.fnt into smart auto-fit atlas (h=%d)\n",
                s_stock_h[1]);
    }
}

bool nox_3ds_font_init(void)
{
    if (s_atlas_inited)
        return true;

    bool in_vram = C3D_TexInitVRAM(&s_atlas_tex, ATLAS_W, ATLAS_H, GPU_RGBA8);
    if (!in_vram) {
        if (!C3D_TexInit(&s_atlas_tex, ATLAS_W, ATLAS_H, GPU_RGBA8)) {
            fprintf(stderr, "[font3ds] ERROR: C3D_TexInit(%dx%d RGBA8) failed\n", ATLAS_W, ATLAS_H);
            return false;
        }
    }

    if (!ensure_staging_baked()) {
        C3D_TexDelete(&s_atlas_tex);
        return false;
    }

    size_t staging_bytes = (size_t)ATLAS_W * (size_t)ATLAS_H * 4u;
    GSPGPU_FlushDataCache(s_atlas_staging, (u32)staging_bytes);
    s_atlas_uploaded = false;

    C3D_TexSetFilter(&s_atlas_tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&s_atlas_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    s_atlas_inited = true;
    s_counts[0] = 0;
    s_counts[1] = 0;
    fprintf(stderr, "[font3ds] Smart auto-fit GPU font atlas prepared (%dx%d %s, default OFF)\n",
            ATLAS_W, ATLAS_H, in_vram ? "VRAM" : "FCRAM");
    return true;
}

void nox_3ds_font_upload_pending(void)
{
    if (!s_atlas_inited || s_atlas_uploaded || !s_atlas_staging)
        return;

    const u32 xfer_flags =
        GX_TRANSFER_FLIP_VERT(0) |
        GX_TRANSFER_OUT_TILED(1) |
        GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

    C3D_SyncDisplayTransfer(
        (u32 *)s_atlas_staging,
        GX_BUFFER_DIM(ATLAS_W, ATLAS_H),
        (u32 *)s_atlas_tex.data,
        GX_BUFFER_DIM(ATLAS_W, ATLAS_H),
        xfer_flags
    );
    s_atlas_uploaded = true;
}

void nox_3ds_font_shutdown(void)
{
    if (s_atlas_staging) {
        linearFree(s_atlas_staging);
        s_atlas_staging = NULL;
    }
    s_staging_baked = false;
    s_large_loaded = false;
    s_fallback_depth = 0;
    memset(s_stock_loaded, 0, sizeof(s_stock_loaded));
    if (!s_atlas_inited)
        return;
    C3D_TexDelete(&s_atlas_tex);
    s_atlas_inited = false;
    s_atlas_uploaded = false;
    s_counts[0] = 0;
    s_counts[1] = 0;
}

bool nox_3ds_font_is_enabled(void)
{
    return s_native_enabled && s_atlas_inited && (s_fallback_depth == 0);
}

void nox_3ds_font_set_enabled(bool enabled)
{
    s_native_enabled = enabled;
    s_fallback_depth = 0;
    s_counts[0] = 0;
    s_counts[1] = 0;
}

void nox_3ds_font_toggle(void)
{
    s_native_enabled = !s_native_enabled;
    s_fallback_depth = 0;
    s_counts[0] = 0;
    s_counts[1] = 0;
    nox_3ds_swbuf_invalidate_bottom();
    s_toast_until_ms = nox_3ds_get_ticks_ms() + TOAST_MS;
    fprintf(stderr, "[font3ds] Native Auto-Fit GPU font: %s\n",
            s_native_enabled ? "ON" : "OFF");
}

void nox_3ds_font_push_fallback(bool force_software)
{
    if (force_software)
        ++s_fallback_depth;
}

void nox_3ds_font_pop_fallback(bool force_software)
{
    if (force_software && s_fallback_depth > 0)
        --s_fallback_depth;
}

void nox_3ds_font_clear_screen(int screen)
{
    if (screen >= 0 && screen < FONT_SCREENS)
        s_counts[screen] = 0;
}

void nox_3ds_font_clear_all(void)
{
    s_counts[0] = 0;
    s_counts[1] = 0;
}

static int crisp_glyph_width_logical(uint16_t ch, bool is_bottom)
{
    if (ch < 0x20 || ch > 0x7E)
        return -1;
    int pw = (int)s_crisp_font[ch - 0x20].w;
    int adv_phys = pw + 1;
    int adv_logical = is_bottom ? (adv_phys * 2) : ((adv_phys * 8 + 2) / 5);
    return (adv_logical > 1) ? (adv_logical - 1) : 1;
}

int nox_3ds_font_glyph_width_logical(int font_slot, uint16_t ch, bool is_bottom)
{
    if (!nox_3ds_font_is_enabled())
        return -1;
    if (ch < 0x20 || ch > 0x7E)
        return -1;
    if (font_slot == 1 && s_large_loaded)
        return (int)s_stock_w[1][ch - 0x20];
    return crisp_glyph_width_logical(ch, is_bottom);
}

int nox_3ds_font_line_height_logical(int font_slot, bool is_bottom)
{
    if (!nox_3ds_font_is_enabled())
        return -1;
    if (font_slot == 1 && s_large_loaded)
        return s_stock_h[1];
    if (font_slot == 3)
        return is_bottom ? 10 : 9;
    return 10;
}

/* Measure wrapped string dimensions using either crisp 3x5 or stock .fnt metrics */
static void measure_string_wrapped(int font_slot, const uint16_t *wstr, int wrap_w, int tab_w,
                                   bool use_crisp, bool is_bottom,
                                   int *out_max_w, int *out_lines, int *out_line_h)
{
    int line_h = use_crisp
        ? ((font_slot == 3 && !is_bottom) ? 9 : 10)
        : ((font_slot >= 0 && font_slot < FONT_SLOTS && s_stock_h[font_slot] > 0) ? s_stock_h[font_slot] : 10);
    if (tab_w <= 0)
        tab_w = 64;

    int max_w = 0;
    int cur_w = 0;
    int lines = 1;
    int word_w = 0;
    bool has_space = false;
    uint16_t prev = 0;

    for (const uint16_t *p = wstr; p && *p; ++p) {
        uint16_t ch = *p;
        if (ch == 9u) {
            cur_w = cur_w + tab_w - (cur_w + tab_w) % tab_w;
            has_space = true;
            word_w = 0;
            if (cur_w > max_w) max_w = cur_w;
        } else if (ch == 10u || ch == 13u) {
            if (prev != 13u) {
                ++lines;
                cur_w = 0;
                word_w = 0;
                has_space = false;
            }
        } else {
            int gw = -1;
            if (use_crisp) {
                gw = crisp_glyph_width_logical(ch, is_bottom);
            }
            if (gw < 0) {
                if (font_slot >= 0 && font_slot < FONT_SLOTS && ch >= 0x20 && ch <= 0x7E && s_stock_loaded[font_slot])
                    gw = (int)s_stock_w[font_slot][ch - 0x20];
                else
                    gw = 5;
            }
            int adv = gw + 1;
            if (ch == 0x20u) {
                cur_w += adv;
                has_space = true;
                word_w = 0;
            } else {
                cur_w += adv;
                word_w += adv;
            }
            if (wrap_w > 0 && cur_w > wrap_w) {
                ++lines;
                if (has_space && word_w <= wrap_w) {
                    cur_w = word_w;
                } else {
                    cur_w = adv;
                    word_w = adv;
                }
                has_space = false;
            }
            if (cur_w > max_w) max_w = cur_w;
        }
        prev = ch;
    }

    if (out_max_w) *out_max_w = max_w;
    if (out_lines) *out_lines = lines;
    if (out_line_h) *out_line_h = line_h;
}

bool nox_3ds_font_fits_single(int font_slot, const uint16_t *wstr, int max_w, int tab_w)
{
    if (!nox_3ds_font_is_enabled() || !wstr || !*wstr)
        return true;
    /* Top Screen (0.625x) and authentic large.fnt (slot 1) always fit cleanly. */
    if (!nox_3ds_is_bottom_swbuf_active())
        return true;
    if (font_slot == 1 && s_large_loaded)
        return true;
    if (max_w <= 0)
        return true;

    int cw = 0, clines = 1, clh = 10;
    measure_string_wrapped(font_slot, wstr, 0, tab_w, true, true, &cw, &clines, &clh);
    return (clines == 1 && cw <= max_w);
}

bool nox_3ds_font_fits_multiline(int font_slot, const uint16_t *wstr, int max_w, int max_h, int tab_w)
{
    if (!nox_3ds_font_is_enabled() || !wstr || !*wstr)
        return true;
    if (!nox_3ds_is_bottom_swbuf_active())
        return true;
    if (font_slot == 1 && s_large_loaded)
        return true;
    if (max_w <= 0)
        return true;

    int sw = 0, slines = 1, slh = 10;
    int cw = 0, clines = 1, clh = 10;
    measure_string_wrapped(font_slot, wstr, max_w, tab_w, false, true, &sw, &slines, &slh);
    measure_string_wrapped(font_slot, wstr, max_w, tab_w, true,  true, &cw, &clines, &clh);

    /* If stock renders on 1 line, crisp must also stay on 1 line inside max_w
     * so labels never wrap into sibling controls below them. */
    if (slines == 1) {
        int raw_cw = 0;
        measure_string_wrapped(font_slot, wstr, 0, tab_w, true, true, &raw_cw, NULL, NULL);
        return (clines == 1 && raw_cw <= max_w);
    }

    /* Multi-line static box: allow at most 1 extra wrapped line over stock
     * (and stay inside max_h when max_h is taller than a single line). */
    int crisp_h = clines * (clh + 1);
    if (clines > slines + 1)
        return false;
    if (max_h >= slh * 2 && crisp_h > max_h)
        return false;
    return true;
}

bool nox_3ds_font_enqueue_glyph(int screen, int font_slot, uint16_t ch,
                                int x, int y, uint16_t rgb555,
                                bool clip_enabled, int clip_l, int clip_t, int clip_r, int clip_b,
                                uint16_t **sw_rows, int sw_w, int sw_h,
                                int *out_logical_advance)
{
    if (!nox_3ds_font_is_enabled())
        return false;
    if (screen < 0 || screen >= FONT_SCREENS)
        return false;
    if (ch < 0x20 || ch > 0x7E)
        return false;

    bool is_bottom = (screen == NOX_3DS_SWBUF_BOTTOM);
    int w_logical = nox_3ds_font_glyph_width_logical(font_slot, ch, is_bottom);
    if (w_logical < 0)
        return false;

    if (out_logical_advance)
        *out_logical_advance = w_logical;

    if (ch == 0x20 || w_logical <= 0)
        return true;

    int h_logical = nox_3ds_font_line_height_logical(font_slot, is_bottom);
    if (h_logical <= 0)
        h_logical = 10;

    int gl = x;
    int gt = y;
    int gr = x + w_logical + 1;
    int gb = y + h_logical;

    if (clip_enabled) {
        if (gl < clip_l) gl = clip_l;
        if (gt < clip_t) gt = clip_t;
        if (gr > clip_r) gr = clip_r;
        if (gb > clip_b) gb = clip_b;
    }
    if (gl < 0) gl = 0;
    if (gt < 0) gt = 0;
    if (gr > sw_w) gr = sw_w;
    if (gb > sw_h) gb = sw_h;
    if (gl >= gr || gt >= gb)
        return true;

    int cnt = s_counts[screen];
    if (cnt >= FONT_MAX_GLYPHS)
        return true;

    int p0x = gl + (gr - gl) / 2;
    int p0y = gt + (gb - gt) / 2;
    int p1x = gl + (gr - gl) / 4;
    int p1y = gt + (gb - gt) / 4;
    if (p0x >= sw_w) p0x = sw_w - 1;
    if (p0y >= sw_h) p0y = sw_h - 1;
    if (p1x >= sw_w) p1x = sw_w - 1;
    if (p1y >= sw_h) p1y = sw_h - 1;

    if (sw_rows && p0y >= 0 && p0x >= 0 && p1y >= 0 && p1x >= 0) {
        sw_rows[p0y][p0x] |= 0x8000u;
        sw_rows[p1y][p1x] |= 0x8000u;
    }

    queued_glyph_t *q = &s_queues[screen][cnt];
    q->ch       = ch;
    q->x        = (int16_t)x;
    q->y        = (int16_t)y;
    q->clip_l   = (int16_t)clip_l;
    q->clip_t   = (int16_t)clip_t;
    q->clip_r   = (int16_t)clip_r;
    q->clip_b   = (int16_t)clip_b;
    q->probe0_x = (int16_t)p0x;
    q->probe0_y = (int16_t)p0y;
    q->probe1_x = (int16_t)p1x;
    q->probe1_y = (int16_t)p1y;
    q->rgb555   = rgb555;
    q->tier     = (uint8_t)tier_for_slot(font_slot, is_bottom);
    q->clip_en  = clip_enabled ? 1u : 0u;
    q->visible  = 1u;
    q->_pad     = 0u;

    s_counts[screen] = cnt + 1;
    return true;
}

void nox_3ds_font_resolve_occlusion(int screen, uint16_t **sw_rows, int sw_w, int sw_h)
{
    if (screen < 0 || screen >= FONT_SCREENS || !sw_rows)
        return;
    int cnt = s_counts[screen];
    queued_glyph_t *arr = s_queues[screen];

    for (int i = 0; i < cnt; ++i) {
        queued_glyph_t *q = &arr[i];
        bool ok = false;
        if (q->probe0_x >= 0 && q->probe0_x < sw_w && q->probe0_y >= 0 && q->probe0_y < sw_h) {
            if (sw_rows[q->probe0_y][q->probe0_x] & 0x8000u)
                ok = true;
        }
        if (!ok && q->probe1_x >= 0 && q->probe1_x < sw_w && q->probe1_y >= 0 && q->probe1_y < sw_h) {
            if (sw_rows[q->probe1_y][q->probe1_x] & 0x8000u)
                ok = true;
        }
        q->visible = ok ? 1u : 0u;
    }

    for (int i = 0; i < cnt; ++i) {
        queued_glyph_t *q = &arr[i];
        if (q->probe0_x >= 0 && q->probe0_x < sw_w && q->probe0_y >= 0 && q->probe0_y < sw_h)
            sw_rows[q->probe0_y][q->probe0_x] &= 0x7FFFu;
        if (q->probe1_x >= 0 && q->probe1_x < sw_w && q->probe1_y >= 0 && q->probe1_y < sw_h)
            sw_rows[q->probe1_y][q->probe1_x] &= 0x7FFFu;
    }
}

static void draw_subrect_clipped(int ax_base, int ay_base, int gw, int gh,
                                 int px, int py,
                                 int clip_x0, int clip_y0, int clip_x1, int clip_y1,
                                 u32 rgba32)
{
    if (gw <= 0 || gh <= 0)
        return;

    int gx0 = px;
    int gy0 = py;
    int gx1 = px + gw;
    int gy1 = py + gh;
    if (gx1 <= clip_x0 || gx0 >= clip_x1 || gy1 <= clip_y0 || gy0 >= clip_y1)
        return;

    int trim_l = (gx0 < clip_x0) ? (clip_x0 - gx0) : 0;
    int trim_r = (gx1 > clip_x1) ? (gx1 - clip_x1) : 0;
    int trim_t = (gy0 < clip_y0) ? (clip_y0 - gy0) : 0;
    int trim_b = (gy1 > clip_y1) ? (gy1 - clip_y1) : 0;
    int dw = gw - trim_l - trim_r;
    int dh = gh - trim_t - trim_b;
    if (dw <= 0 || dh <= 0)
        return;

    int ax = ax_base + trim_l;
    int ay = ay_base + trim_t;

    Tex3DS_SubTexture st;
    st.width  = (u16)dw;
    st.height = (u16)dh;
    st.left   = (float)ax / (float)ATLAS_W;
    st.right  = (float)(ax + dw) / (float)ATLAS_W;
    st.top    = (float)(ATLAS_H - ay) / (float)ATLAS_H;
    st.bottom = (float)(ATLAS_H - (ay + dh)) / (float)ATLAS_H;

    C2D_Image img;
    img.tex    = &s_atlas_tex;
    img.subtex = &st;

    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, rgba32, 1.0f);
    C2D_DrawImageAt(img, (float)(gx0 + trim_l), (float)(gy0 + trim_t), 0.0f, &tint, 1.0f, 1.0f);
}

static void draw_tier_glyph_clipped(font_tier_t tier, uint16_t ch,
                                    int px, int py,
                                    int clip_x0, int clip_y0, int clip_x1, int clip_y1,
                                    u32 rgba32)
{
    if (ch < 0x20 || ch > 0x7E)
        return;
    int idx = (int)(ch - 0x20);
    int col = idx % ATLAS_COLS;
    int row = idx / ATLAS_COLS;
    int ax = 0, ay = 0, gw = 0, gh = 0;

    switch (tier) {
        case TIER_CRISP:
            ax = CRISP_BASE_X + col * CRISP_CELL_W;
            ay = CRISP_BASE_Y + row * CRISP_CELL_H;
            gw = (int)s_crisp_font[idx].w;
            gh = 6;
            break;
        case TIER_LARGE_BOT:
            ax = LARGE_BOT_BASE_X + col * LARGE_BOT_CELL_W;
            ay = LARGE_BOT_BASE_Y + row * LARGE_BOT_CELL_H;
            gw = (int)s_large_bot_w[idx];
            gh = (int)s_large_bot_h[idx];
            break;
        case TIER_LARGE_TOP:
            ax = LARGE_TOP_BASE_X + col * LARGE_TOP_CELL_W;
            ay = LARGE_TOP_BASE_Y + row * LARGE_TOP_CELL_H;
            gw = (int)s_large_top_w[idx];
            gh = (int)s_large_top_h[idx];
            break;
    }

    draw_subrect_clipped(ax, ay, gw, gh, px, py, clip_x0, clip_y0, clip_x1, clip_y1, rgba32);
}

void nox_3ds_font_set_show_version(bool show)
{
    s_show_version_overlay = show;
}

static void draw_crisp_pill_overlay(int target_w, int target_h,
                                    const char *msg, bool bottom_right,
                                    u32 bg, u32 fg)
{
    if (!s_atlas_inited || !msg || !*msg)
        return;

    int total_w = 0;
    for (const char *p = msg; *p; ++p) {
        uint16_t c = (uint16_t)(unsigned char)*p;
        if (c >= 0x20 && c <= 0x7E)
            total_w += (int)s_crisp_font[c - 0x20].w + 1;
    }
    int pad_x = 4, pad_y = 3;
    int box_w = total_w + pad_x * 2;
    int box_h = 6 + pad_y * 2;
    int bx = target_w - box_w - 4;
    int by = bottom_right ? (target_h - box_h - 4) : 4;

    C2D_DrawRectSolid((float)bx, (float)by, 0.0f, (float)box_w, (float)box_h, bg);
    C2D_SetTintMode(C2D_TintMult);
    int cx = bx + pad_x;
    int cy = by + pad_y;
    for (const char *p = msg; *p; ++p) {
        uint16_t c = (uint16_t)(unsigned char)*p;
        if (c >= 0x20 && c <= 0x7E) {
            draw_tier_glyph_clipped(TIER_CRISP, c, cx, cy, 0, 0, target_w, target_h, fg);
            cx += (int)s_crisp_font[c - 0x20].w + 1;
        }
    }
}

static void draw_toast_overlay(int target_w, int target_h)
{
    if (!s_atlas_inited || s_toast_until_ms == 0)
        return;
    uint32_t now = nox_3ds_get_ticks_ms();
    if ((int32_t)(s_toast_until_ms - now) <= 0) {
        s_toast_until_ms = 0;
        return;
    }

    const char *msg = s_native_enabled ? "NATIVE FONT: AUTO" : "NATIVE FONT: OFF";
    u32 fg = s_native_enabled ? C2D_Color32(120, 255, 140, 255) : C2D_Color32(255, 210, 110, 255);
    draw_crisp_pill_overlay(target_w, target_h, msg, false,
                            C2D_Color32(10, 10, 16, 210), fg);
}

static void draw_version_overlay(int target_w, int target_h)
{
    if (!s_show_version_overlay)
        return;
    draw_crisp_pill_overlay(target_w, target_h, NOX_3DS_VERSION_STR, true,
                            C2D_Color32(8, 8, 14, 195),
                            C2D_Color32(235, 205, 130, 245));
}

void nox_3ds_font_render_screen(int screen,
                                int crop_x, int crop_y, int crop_w, int crop_h,
                                float draw_x, float draw_y, float dst_w, float dst_h,
                                float gamma)
{
    if (!s_atlas_inited || screen < 0 || screen >= FONT_SCREENS)
        return;

    int target_w = (screen == NOX_3DS_SWBUF_TOP) ? NOX_3DS_TOP_W : NOX_3DS_BOT_W;
    int target_h = (screen == NOX_3DS_SWBUF_TOP) ? NOX_3DS_TOP_H : NOX_3DS_BOT_H;

    if (s_native_enabled && s_counts[screen] > 0 && crop_w > 0 && crop_h > 0) {
        float scale_x = dst_w / (float)crop_w;
        float scale_y = dst_h / (float)crop_h;
        float g_mul = (gamma < 0.98f) ? ((gamma < 0.0f) ? 0.0f : gamma) : 1.0f;

        C2D_SetTintMode(C2D_TintMult);

        int cnt = s_counts[screen];
        const queued_glyph_t *arr = s_queues[screen];

        /* Pass 1: crisp 1px physical black drop-shadow behind non-dark glyphs */
        u32 shadow_rgba = C2D_Color32(0, 0, 0, (u8)(180.0f * g_mul));
        for (int i = 0; i < cnt; ++i) {
            const queued_glyph_t *q = &arr[i];
            if (!q->visible)
                continue;

            int r5 = (q->rgb555 >> 10) & 31;
            int g5 = (q->rgb555 >> 5) & 31;
            int b5 = q->rgb555 & 31;
            int r8 = (r5 << 3) | (r5 >> 2);
            int g8 = (g5 << 3) | (g5 >> 2);
            int b8 = (b5 << 3) | (b5 >> 2);
            if (r8 + g8 + b8 < 96)
                continue;

            int px = (int)floorf(draw_x + (float)(q->x - crop_x) * scale_x + 0.5f);
            int py = (int)floorf(draw_y + (float)(q->y - crop_y) * scale_y + 0.5f);

            int cx0 = 0, cy0 = 0, cx1 = target_w, cy1 = target_h;
            if (q->clip_en) {
                cx0 = (int)floorf(draw_x + (float)(q->clip_l - crop_x) * scale_x);
                cy0 = (int)floorf(draw_y + (float)(q->clip_t - crop_y) * scale_y);
                cx1 = (int)ceilf(draw_x + (float)(q->clip_r - crop_x) * scale_x);
                cy1 = (int)ceilf(draw_y + (float)(q->clip_b - crop_y) * scale_y);
                if (cx0 < 0) cx0 = 0;
                if (cy0 < 0) cy0 = 0;
                if (cx1 > target_w) cx1 = target_w;
                if (cy1 > target_h) cy1 = target_h;
            }

            draw_tier_glyph_clipped((font_tier_t)q->tier, q->ch,
                                    px + 1, py + 1,
                                    cx0, cy0, cx1, cy1, shadow_rgba);
        }

        /* Pass 2: crisp 1:1 foreground glyphs */
        for (int i = 0; i < cnt; ++i) {
            const queued_glyph_t *q = &arr[i];
            if (!q->visible)
                continue;

            int r5 = (q->rgb555 >> 10) & 31;
            int g5 = (q->rgb555 >> 5) & 31;
            int b5 = q->rgb555 & 31;
            u8 r8 = (u8)(((r5 << 3) | (r5 >> 2)) * g_mul);
            u8 g8 = (u8)(((g5 << 3) | (g5 >> 2)) * g_mul);
            u8 b8 = (u8)(((b5 << 3) | (b5 >> 2)) * g_mul);
            u32 fg = C2D_Color32(r8, g8, b8, 255);

            int px = (int)floorf(draw_x + (float)(q->x - crop_x) * scale_x + 0.5f);
            int py = (int)floorf(draw_y + (float)(q->y - crop_y) * scale_y + 0.5f);

            int cx0 = 0, cy0 = 0, cx1 = target_w, cy1 = target_h;
            if (q->clip_en) {
                cx0 = (int)floorf(draw_x + (float)(q->clip_l - crop_x) * scale_x);
                cy0 = (int)floorf(draw_y + (float)(q->clip_t - crop_y) * scale_y);
                cx1 = (int)ceilf(draw_x + (float)(q->clip_r - crop_x) * scale_x);
                cy1 = (int)ceilf(draw_y + (float)(q->clip_b - crop_y) * scale_y);
                if (cx0 < 0) cx0 = 0;
                if (cy0 < 0) cy0 = 0;
                if (cx1 > target_w) cx1 = target_w;
                if (cy1 > target_h) cy1 = target_h;
            }

            draw_tier_glyph_clipped((font_tier_t)q->tier, q->ch,
                                    px, py,
                                    cx0, cy0, cx1, cy1, fg);
        }
    }

    if (screen == NOX_3DS_SWBUF_BOTTOM) {
        draw_toast_overlay(target_w, target_h);
    } else if (screen == NOX_3DS_SWBUF_TOP) {
        draw_version_overlay(target_w, target_h);
    }
}
