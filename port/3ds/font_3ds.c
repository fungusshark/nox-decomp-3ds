/*
 * Exact-Parity Native 1:1 PICA200 / citro2d GPU Font Renderer for Nox 3DS
 *
 * Bakes all 4 of Nox's loaded .fnt files (default.fnt, large.fnt, number.fnt,
 * small.fnt) into a single 256x512 GPU_RGBA8 VRAM atlas using exact area-coverage
 * supersampling (0.5x for 320x240 Bottom Screen, 0.625x for 400x240 Top Screen)
 * with stem-preserving contrast-boosted alpha.
 *
 * Keeps 100% exact width & height parity with stock Nox (zero layout overruns,
 * zero extra line wraps, authentic medieval typography) while eliminating:
 *   1. Nox's 3x3 software smoothing smudge (sub_43F670)
 *   2. Framebuffer bilinear background-color bleed into glyph strokes
 *   3. Half-pixel downscale phase jitter between adjacent glyphs
 *   4. Thin 1px stem washout (alpha contrast boost preserves ~91% stem brightness)
 *
 * Uses Nox's unused RGB555 bit 15 (0x8000) as a pixel-exact occlusion stencil
 * against overlapping windows, stone frames, dragged items, and tooltips.
 *
 * Live toggle: L + R + SELECT (hold L + R, tap SELECT) to A/B compare vs software.
 */

#include "platform_3ds.h"

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ATLAS_W           256
#define ATLAS_H           512
#define ATLAS_COLS        16
#define FONT_SLOTS        4
#define FONT_GLYPH_COUNT  95   /* ASCII 0x20..0x7E */
#define FONT_SCREENS      2
#define FONT_MAX_GLYPHS   1800
#define TOAST_MS          2000u

/* Atlas layout inside 256x512 GPU_RGBA8 (16 cols x 6 rows = 96 cells per band,
 * with >= 1px transparent gutter around every glyph cell):
 *   y =   0.. 59 : slot 0 (default.fnt) Bot [0..127, 8x8]  & Top [128..255, 8x10]
 *   y =  64..111 : slot 3 (small.fnt)   Bot [0..127, 8x8]  & Top [128..255, 8x8]
 *   y = 116..175 : slot 2 (number.fnt)  Bot [0..191, 12x10]
 *   y = 180..251 : slot 2 (number.fnt)  Top [0..191, 12x12]
 *   y = 256..339 : slot 1 (large.fnt)   Bot [0..255, 16x14]
 *   y = 344..439 : slot 1 (large.fnt)   Top [0..255, 16x16]
 *   y = 448..495 : embedded 3x5 toast/fallback font [0..127, 8x8]
 */
#define TOAST_BASE_X      0
#define TOAST_BASE_Y      448
#define TOAST_CELL_W      8
#define TOAST_CELL_H      8

typedef struct {
    int base_x;
    int base_y;
    int cell_w;
    int cell_h;
} atlas_band_t;

/* [font_slot][screen: 0=Top, 1=Bottom] */
static const atlas_band_t s_slot_bands[FONT_SLOTS][FONT_SCREENS] = {
    /* slot 0: default.fnt */
    { { 128,   0,  8, 10 }, {   0,   0,  8,  8 } },
    /* slot 1: large.fnt   */
    { {   0, 344, 16, 16 }, {   0, 256, 16, 14 } },
    /* slot 2: number.fnt  */
    { {   0, 180, 12, 12 }, {   0, 116, 12, 10 } },
    /* slot 3: small.fnt   */
    { { 128,  64,  8,  8 }, {   0,  64,  8,  8 } }
};

typedef struct {
    uint8_t w;       /* glyph bitmap width in physical pixels (0..5) */
    uint8_t rows[6]; /* MSB-first in bits 7..3 (bit 7 = leftmost pixel) */
} toast_glyph_t;

typedef struct {
    bool    loaded;
    int     logical_h;
    uint8_t logical_w[FONT_GLYPH_COUNT];
    uint8_t phys_w[FONT_SCREENS][FONT_GLYPH_COUNT];
    uint8_t phys_h[FONT_SCREENS][FONT_GLYPH_COUNT];
} baked_fnt_slot_t;

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
    uint8_t  slot;       /* 0..3 = baked .fnt slot, 4 = toast fallback */
    uint8_t  clip_en;
    uint8_t  visible;    /* set during bit-15 occlusion resolve */
    uint8_t  _pad;
} queued_glyph_t;

/* Crisp 3x5 embedded pixel font for L+SELECT status toast & pre-load fallback */
static const toast_glyph_t s_toast_font[FONT_GLYPH_COUNT] = {
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

static C3D_Tex          s_atlas_tex;
static uint32_t        *s_atlas_staging = NULL;
static bool             s_staging_baked = false;
static bool             s_atlas_inited = false;
static bool             s_atlas_uploaded = false;
static bool             s_native_enabled = false;
static uint32_t         s_toast_until_ms = 0;

static baked_fnt_slot_t s_fnt[FONT_SLOTS];
static queued_glyph_t   s_queues[FONT_SCREENS][FONT_MAX_GLYPHS];
static int              s_counts[FONT_SCREENS] = { 0, 0 };

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

    /* Bake embedded 3x5 toast/fallback glyphs into rows 448..495 */
    for (int i = 0; i < FONT_GLYPH_COUNT; ++i) {
        int col = i % ATLAS_COLS;
        int row = i / ATLAS_COLS;
        int bx = TOAST_BASE_X + col * TOAST_CELL_W;
        int by = TOAST_BASE_Y + row * TOAST_CELL_H;
        const toast_glyph_t *g = &s_toast_font[i];
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

static void clear_band(const atlas_band_t *b)
{
    int band_w = ATLAS_COLS * b->cell_w;
    int band_h = 6 * b->cell_h;
    if (b->base_x + band_w > ATLAS_W) band_w = ATLAS_W - b->base_x;
    if (b->base_y + band_h > ATLAS_H) band_h = ATLAS_H - b->base_y;
    for (int y = 0; y < band_h; ++y) {
        memset(s_atlas_staging + (size_t)(b->base_y + y) * ATLAS_W + (size_t)b->base_x,
               0, (size_t)band_w * sizeof(uint32_t));
    }
}

void nox_3ds_font_begin_slot(int font_slot, int src_h, int line_pitch)
{
    if (font_slot < 0 || font_slot >= FONT_SLOTS)
        return;
    if (!ensure_staging_baked())
        return;

    clear_band(&s_slot_bands[font_slot][NOX_3DS_SWBUF_TOP]);
    clear_band(&s_slot_bands[font_slot][NOX_3DS_SWBUF_BOTTOM]);

    memset(&s_fnt[font_slot], 0, sizeof(s_fnt[font_slot]));
    s_fnt[font_slot].logical_h = (line_pitch > 0) ? line_pitch : ((src_h > 0) ? src_h : 10);
    s_fnt[font_slot].loaded = false;
}

void nox_3ds_font_add_glyph(int font_slot, uint16_t ch, int src_w,
                            const uint8_t *bits, int row_stride, int src_h)
{
    if (font_slot < 0 || font_slot >= FONT_SLOTS)
        return;
    if (!s_atlas_staging || !bits || ch < 0x20 || ch > 0x7E)
        return;
    if (src_w < 0 || src_h <= 0 || row_stride <= 0)
        return;

    int idx = (int)(ch - 0x20);
    int col = idx % ATLAS_COLS;
    int row = idx / ATLAS_COLS;
    baked_fnt_slot_t *slot = &s_fnt[font_slot];

    slot->logical_w[idx] = (uint8_t)((src_w > 255) ? 255 : src_w);
    if (src_w == 0)
        return;

    /* Thin-stroke fonts (default.fnt, small.fnt) get stronger stem-preserving
     * alpha boost so 1px stems stay ~91% bright instead of washing out to 50%. */
    bool thin_strokes = (font_slot == 0 || font_slot == 3);

    /* 1) Bottom Screen (0.5x = exact 2x2 area box filter) */
    {
        static const uint8_t k_alpha4_thin[5]  = { 0, 145, 232, 250, 255 };
        static const uint8_t k_alpha4_bold[5]  = { 0, 115, 200, 242, 255 };
        const uint8_t *lut = thin_strokes ? k_alpha4_thin : k_alpha4_bold;
        const atlas_band_t *b = &s_slot_bands[font_slot][NOX_3DS_SWBUF_BOTTOM];

        int dw = (src_w + 1) / 2;
        int dh = (src_h + 1) / 2;
        if (dw > b->cell_w - 1) dw = b->cell_w - 1;
        if (dh > b->cell_h - 1) dh = b->cell_h - 1;
        slot->phys_w[NOX_3DS_SWBUF_BOTTOM][idx] = (uint8_t)dw;
        slot->phys_h[NOX_3DS_SWBUF_BOTTOM][idx] = (uint8_t)dh;

        int bx = b->base_x + col * b->cell_w;
        int by = b->base_y + row * b->cell_h;
        for (int dy = 0; dy < dh; ++dy) {
            for (int dx = 0; dx < dw; ++dx) {
                int hits = sample_fnt_bit(bits, row_stride, src_w, src_h, dx * 2,     dy * 2)
                         + sample_fnt_bit(bits, row_stride, src_w, src_h, dx * 2 + 1, dy * 2)
                         + sample_fnt_bit(bits, row_stride, src_w, src_h, dx * 2,     dy * 2 + 1)
                         + sample_fnt_bit(bits, row_stride, src_w, src_h, dx * 2 + 1, dy * 2 + 1);
                uint8_t a = lut[hits];
                if (a > 0) {
                    /* PICA200 GPU_RGBA8: R[31:24] G[23:16] B[15:8] A[7:0] */
                    s_atlas_staging[(size_t)(by + dy) * ATLAS_W + (size_t)(bx + dx)] = 0xFFFFFF00u | (uint32_t)a;
                }
            }
        }
    }

    /* 2) Top Screen (0.625x = exact 1.6x1.6 area-coverage resample) */
    {
        const atlas_band_t *b = &s_slot_bands[font_slot][NOX_3DS_SWBUF_TOP];
        int dw = (src_w * 5 + 4) / 8;
        int dh = (src_h * 5 + 4) / 8;
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        if (dw > b->cell_w - 1) dw = b->cell_w - 1;
        if (dh > b->cell_h - 1) dh = b->cell_h - 1;
        slot->phys_w[NOX_3DS_SWBUF_TOP][idx] = (uint8_t)dw;
        slot->phys_h[NOX_3DS_SWBUF_TOP][idx] = (uint8_t)dh;

        int bx = b->base_x + col * b->cell_w;
        int by = b->base_y + row * b->cell_h;
        const float step = 1.6f;
        const float inv_area = 1.0f / (step * step);
        const float gamma_exp = thin_strokes ? 0.58f : 0.72f;

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
                    int ai = (int)(powf(cov, gamma_exp) * 255.0f + 0.5f);
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
    if (font_slot < 0 || font_slot >= FONT_SLOTS || !s_atlas_staging)
        return;
    size_t staging_bytes = (size_t)ATLAS_W * (size_t)ATLAS_H * 4u;
    GSPGPU_FlushDataCache(s_atlas_staging, (u32)staging_bytes);
    s_fnt[font_slot].loaded = true;
    s_atlas_uploaded = false;
    fprintf(stderr, "[font3ds] Baked exact-parity GPU .fnt slot %d (logical_h=%d)\n",
            font_slot, s_fnt[font_slot].logical_h);
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
    fprintf(stderr, "[font3ds] Exact-parity GPU font atlas prepared (%dx%d %s, deferred GX upload)\n",
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
    memset(s_fnt, 0, sizeof(s_fnt));
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
    return s_native_enabled && s_atlas_inited;
}

void nox_3ds_font_set_enabled(bool enabled)
{
    s_native_enabled = enabled;
    s_counts[0] = 0;
    s_counts[1] = 0;
}

void nox_3ds_font_toggle(void)
{
    s_native_enabled = !s_native_enabled;
    s_counts[0] = 0;
    s_counts[1] = 0;
    nox_3ds_swbuf_invalidate_bottom();
    s_toast_until_ms = nox_3ds_get_ticks_ms() + TOAST_MS;
    fprintf(stderr, "[font3ds] Native GPU font: %s\n",
            s_native_enabled ? "ON" : "OFF");
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

int nox_3ds_font_glyph_width_logical(int font_slot, uint16_t ch, bool is_bottom)
{
    (void)is_bottom;
    if (!nox_3ds_font_is_enabled())
        return -1;
    if (ch < 0x20 || ch > 0x7E)
        return -1;
    if (font_slot < 0 || font_slot >= FONT_SLOTS || !s_fnt[font_slot].loaded)
        return -1;
    /* Exact 100% width parity with Nox's stock .fnt glyph metrics. */
    return (int)s_fnt[font_slot].logical_w[ch - 0x20];
}

int nox_3ds_font_line_height_logical(int font_slot, bool is_bottom)
{
    (void)is_bottom;
    if (!nox_3ds_font_is_enabled())
        return -1;
    if (font_slot < 0 || font_slot >= FONT_SLOTS || !s_fnt[font_slot].loaded)
        return -1;
    /* Exact 100% height parity with Nox's stock .fnt line pitch. */
    return s_fnt[font_slot].logical_h;
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
    if (font_slot < 0 || font_slot >= FONT_SLOTS || !s_fnt[font_slot].loaded)
        return false;
    if (ch < 0x20 || ch > 0x7E)
        return false;

    int w_logical = (int)s_fnt[font_slot].logical_w[ch - 0x20];
    if (out_logical_advance)
        *out_logical_advance = w_logical;

    /* Space has no visible ink; just advance cursor. */
    if (ch == 0x20 || w_logical <= 0)
        return true;

    int h_logical = s_fnt[font_slot].logical_h;
    if (h_logical <= 0)
        h_logical = 10;

    int gl = x;
    int gt = y;
    int gr = x + w_logical;
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
        return true; /* clipped out cleanly */

    int cnt = s_counts[screen];
    if (cnt >= FONT_MAX_GLYPHS)
        return true;

    /* Pick two interior probe pixels in the clipped glyph rect and set unused
     * RGB555 bit 15 (0x8000). Any later opaque software draw clears bit 15;
     * translucent overlays (sub_49D4A0 / sub_49D6F0) preserve bit 15. */
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
    q->slot     = (uint8_t)font_slot;
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

static void draw_baked_glyph_clipped(int font_slot, int screen, uint16_t ch,
                                     int px, int py,
                                     int clip_x0, int clip_y0, int clip_x1, int clip_y1,
                                     u32 rgba32)
{
    if (ch < 0x20 || ch > 0x7E || font_slot < 0 || font_slot >= FONT_SLOTS)
        return;
    int idx = (int)(ch - 0x20);
    int gw = (int)s_fnt[font_slot].phys_w[screen][idx];
    int gh = (int)s_fnt[font_slot].phys_h[screen][idx];
    if (gw <= 0 || gh <= 0)
        return;

    const atlas_band_t *b = &s_slot_bands[font_slot][screen];
    int col = idx % ATLAS_COLS;
    int row = idx / ATLAS_COLS;
    draw_subrect_clipped(b->base_x + col * b->cell_w,
                         b->base_y + row * b->cell_h,
                         gw, gh, px, py,
                         clip_x0, clip_y0, clip_x1, clip_y1, rgba32);
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

    const char *msg = s_native_enabled ? "NATIVE FONT: ON" : "NATIVE FONT: OFF";
    int total_w = 0;
    for (const char *p = msg; *p; ++p) {
        uint16_t c = (uint16_t)(unsigned char)*p;
        if (c >= 0x20 && c <= 0x7E)
            total_w += (int)s_toast_font[c - 0x20].w + 1;
    }
    int pad_x = 4, pad_y = 3;
    int box_w = total_w + pad_x * 2;
    int box_h = 6 + pad_y * 2;
    int bx = target_w - box_w - 4;
    int by = 4;

    C2D_DrawRectSolid((float)bx, (float)by, 0.0f, (float)box_w, (float)box_h,
                      C2D_Color32(10, 10, 16, 210));
    C2D_SetTintMode(C2D_TintMult);
    int cx = bx + pad_x;
    int cy = by + pad_y;
    u32 fg = s_native_enabled ? C2D_Color32(120, 255, 140, 255) : C2D_Color32(255, 210, 110, 255);
    for (const char *p = msg; *p; ++p) {
        uint16_t c = (uint16_t)(unsigned char)*p;
        if (c >= 0x20 && c <= 0x7E) {
            int idx = (int)(c - 0x20);
            int w = (int)s_toast_font[idx].w;
            int col = idx % ATLAS_COLS;
            int row = idx / ATLAS_COLS;
            draw_subrect_clipped(TOAST_BASE_X + col * TOAST_CELL_W,
                                 TOAST_BASE_Y + row * TOAST_CELL_H,
                                 w, 6, cx, cy, 0, 0, target_w, target_h, fg);
            cx += w + 1;
        }
    }
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

        /* Pass 1: subtle 1px physical black drop-shadow behind non-dark glyphs
         * (gentler on tight Bottom default/small text so chars stay airy). */
        u32 shadow_soft = C2D_Color32(0, 0, 0, (u8)(125.0f * g_mul));
        u32 shadow_bold = C2D_Color32(0, 0, 0, (u8)(175.0f * g_mul));
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
                continue; /* skip shadow for dark ink (books/parchment/manual shadow) */

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

            u32 sh = (screen == NOX_3DS_SWBUF_BOTTOM && (q->slot == 0 || q->slot == 3))
                     ? shadow_soft : shadow_bold;
            draw_baked_glyph_clipped((int)q->slot, screen, q->ch,
                                     px + 1, py + 1,
                                     cx0, cy0, cx1, cy1, sh);
        }

        /* Pass 2: crisp 1:1 exact-parity foreground glyphs */
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

            draw_baked_glyph_clipped((int)q->slot, screen, q->ch,
                                     px, py,
                                     cx0, cy0, cx1, cy1, fg);
        }
    }

    /* Show L+SELECT toggle toast on Bottom Screen */
    if (screen == NOX_3DS_SWBUF_BOTTOM) {
        draw_toast_overlay(target_w, target_h);
    }
}
