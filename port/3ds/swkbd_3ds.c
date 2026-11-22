#include "platform_3ds.h"

#include <3ds.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* Engine globals & GUI dispatchers (src/GAME2.c, src/GAME_data.c) */
extern unsigned char byte_5D4594[];
extern int sub_46A4A0(void);                            /* multiplayer chat active */
extern int sub_46A6A0(void);                            /* close multiplayer chat */
extern int sub_46B490(int a1, int a2, int a3, int a4);  /* GUI window message dispatch */

/* Nox ENTRYFIELD widget offsets (verified against sub_488500 / sub_488710 / sub_487D70) */
#define NOX_WIN_OFF_ID           0
#define NOX_WIN_OFF_DATA         32
#define NOX_WIN_OFF_PARENT       52

#define NOX_EF_OFF_TEXT          0     /* wchar_t[256] (512 bytes, -fshort-wchar) */
#define NOX_EF_OFF_IME_COMP      512   /* wchar_t[256] */
#define NOX_EF_OFF_IS_PASSWORD   1024  /* int32_t */
#define NOX_EF_OFF_DIGITS_ONLY   1028  /* int32_t */
#define NOX_EF_OFF_ALNUM_ONLY    1032  /* int32_t */
#define NOX_EF_OFF_MAX_LEN       1040  /* int16_t (buffer capacity incl. NUL, <= 256) */
#define NOX_EF_OFF_TEXT_LEN      1052  /* uint16_t */
#define NOX_EF_OFF_IME_LEN       1054  /* uint16_t */

#define NOX_MSG_EF_SET_TEXT      16414 /* 0x401E */
#define NOX_MSG_EF_SUBMIT        16415 /* 0x401F */

/* Known Nox ENTRYFIELD widget IDs */
#define NOX_WID_CHAR_CREATE_NAME 751
#define NOX_WID_MP_CHAT_INPUT    9201

#define NOX_SWKBD_DEFAULT_MAX    32
#define NOX_SWKBD_CHARNAME_MAX   24
#define NOX_SWKBD_CHAT_MAX       128

static void    *g_pending_win      = NULL;
static bool     g_pending_submit   = false;
static bool     g_swkbd_active     = false;
static uint32_t g_swkbd_cooldown_ms = 0;

/* Convert Nox UTF-16LE (uint16_t) buffer to UTF-8 for swkbdSetInitialText(). */
static void nox_utf16_to_utf8(const uint8_t *src_bytes, int max_u16, char *dst, size_t dst_sz)
{
    if (!dst || dst_sz == 0)
        return;
    size_t out = 0;
    for (int i = 0; i < max_u16; ++i) {
        uint16_t wc = nox_read_u16(src_bytes + (size_t)i * 2u);
        if (wc == 0)
            break;
        if (wc < 0x80u) {
            if (out + 1 >= dst_sz) break;
            dst[out++] = (char)wc;
        } else if (wc < 0x800u) {
            if (out + 2 >= dst_sz) break;
            dst[out++] = (char)(0xC0u | (wc >> 6));
            dst[out++] = (char)(0x80u | (wc & 0x3Fu));
        } else {
            if (out + 3 >= dst_sz) break;
            dst[out++] = (char)(0xE0u | (wc >> 12));
            dst[out++] = (char)(0x80u | ((wc >> 6) & 0x3Fu));
            dst[out++] = (char)(0x80u | (wc & 0x3Fu));
        }
    }
    dst[out] = '\0';
}

/* Convert UTF-8 output from swkbdInputText() to Nox UTF-16LE (wchar_t = uint16_t),
 * honoring the ENTRYFIELD's character limit and digits/alnum filters. */
static int utf8_to_utf16_filtered(const char *src, wchar_t *dst, int max_chars,
                                  bool digits_only, bool alnum_only)
{
    if (!dst || max_chars <= 0)
        return 0;

    const unsigned char *p = (const unsigned char *)src;
    int out = 0;

    while (p && *p && out < max_chars) {
        uint32_t cp = 0;
        if (*p < 0x80u) {
            cp = *p++;
        } else if ((*p & 0xE0u) == 0xC0u && (p[1] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(p[0] & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
            p += 2;
        } else if ((*p & 0xF0u) == 0xE0u && (p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(p[0] & 0x0Fu) << 12) |
                 ((uint32_t)(p[1] & 0x3Fu) << 6) |
                 (uint32_t)(p[2] & 0x3Fu);
            p += 3;
        } else if ((*p & 0xF8u) == 0xF0u && p[1] && p[2] && p[3]) {
            /* Skip non-BMP characters (> U+FFFF) since Nox uses UCS-2 */
            p += 4;
            continue;
        } else {
            p++;
            continue;
        }

        /* Ignore control characters (CR/LF/TAB/etc.) */
        if (cp < 0x20u || cp > 0xFFFFu)
            continue;

        if (digits_only) {
            if (cp >= 0x80u || !isdigit((int)cp))
                continue;
        } else if (alnum_only) {
            if (cp >= 0x80u || !isalnum((int)cp))
                continue;
        }

        dst[out++] = (wchar_t)(uint16_t)cp;
    }

    dst[out] = 0;
    return out;
}

void nox_3ds_swkbd_request(void *entry_win, bool auto_submit)
{
    if (!entry_win || g_swkbd_active)
        return;

    /* Ignore bounce/release events immediately after closing the keyboard */
    if (!auto_submit && g_swkbd_cooldown_ms != 0) {
        uint32_t now = nox_3ds_get_ticks_ms();
        if ((uint32_t)(now - g_swkbd_cooldown_ms) < 300u)
            return;
    }

    /* Avoid clobbering an already-queued request for the same frame unless upgrading to submit */
    if (g_pending_win == entry_win) {
        if (auto_submit)
            g_pending_submit = true;
        return;
    }

    g_pending_win    = entry_win;
    g_pending_submit = auto_submit;
}

void nox_3ds_swkbd_cancel_pending(void *entry_win)
{
    if (!entry_win || g_pending_win == entry_win) {
        g_pending_win    = NULL;
        g_pending_submit = false;
    }
}

bool nox_3ds_swkbd_is_active(void)
{
    return g_swkbd_active;
}

bool nox_3ds_swkbd_pump(void)
{
    if (!g_pending_win || g_swkbd_active)
        return false;

    void *entry_win   = g_pending_win;
    bool  auto_submit = g_pending_submit;
    g_pending_win     = NULL;
    g_pending_submit  = false;

    const uint8_t *win_bytes = (const uint8_t *)entry_win;
    int32_t widget_id = nox_read_s32(win_bytes + NOX_WIN_OFF_ID);
    void   *data      = (void *)(uintptr_t)nox_read_u32(win_bytes + NOX_WIN_OFF_DATA);
    void   *parent    = (void *)(uintptr_t)nox_read_u32(win_bytes + NOX_WIN_OFF_PARENT);

    if (!data)
        return false;

    const uint8_t *data_bytes = (const uint8_t *)data;
    int32_t is_password = nox_read_s32(data_bytes + NOX_EF_OFF_IS_PASSWORD);
    int32_t digits_only = nox_read_s32(data_bytes + NOX_EF_OFF_DIGITS_ONLY);
    int32_t alnum_only  = nox_read_s32(data_bytes + NOX_EF_OFF_ALNUM_ONLY);
    int16_t field_cap   = nox_read_s16(data_bytes + NOX_EF_OFF_MAX_LEN);

    /* Match the engine's ENTRYFIELD buffer limit (max_len includes NUL terminator) */
    int max_chars = (field_cap > 1 && field_cap <= 256) ? (field_cap - 1) : NOX_SWKBD_DEFAULT_MAX;
    const char *hint = "Enter text...";

    if (widget_id == NOX_WID_CHAR_CREATE_NAME) {
        if (max_chars > NOX_SWKBD_CHARNAME_MAX)
            max_chars = NOX_SWKBD_CHARNAME_MAX;
        hint = "Enter character name";
    } else if (widget_id == NOX_WID_MP_CHAT_INPUT || auto_submit) {
        if (max_chars > NOX_SWKBD_CHAT_MAX)
            max_chars = NOX_SWKBD_CHAT_MAX;
        hint = "Enter chat message";
    } else if (max_chars > 128) {
        max_chars = 128;
    }

    if (max_chars < 1)
        max_chars = 1;

    char initial_utf8[512];
    nox_utf16_to_utf8(data_bytes + NOX_EF_OFF_TEXT, max_chars, initial_utf8, sizeof(initial_utf8));

    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, max_chars);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT,  "Cancel", false);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, "OK",     true);
    swkbdSetFeatures(&swkbd, SWKBD_DEFAULT_QWERTY | SWKBD_PREDICTIVE_INPUT | SWKBD_DARKEN_TOP_SCREEN);
    swkbdSetValidation(&swkbd, SWKBD_ANYTHING, 0, 0);
    swkbdSetHintText(&swkbd, hint);

    if (is_password) {
        swkbdSetPasswordMode(&swkbd, SWKBD_PASSWORD_HIDE_DELAY);
    }
    if (initial_utf8[0] != '\0') {
        swkbdSetInitialText(&swkbd, initial_utf8);
    }

    char out_utf8[512];
    memset(out_utf8, 0, sizeof(out_utf8));

    /* Blocking APT library applet call: pauses the Nox game loop safely outside C3D_FrameBegin. */
    g_swkbd_active = true;
    SwkbdButton button = swkbdInputText(&swkbd, out_utf8, sizeof(out_utf8));
    g_swkbd_active = false;
    g_swkbd_cooldown_ms = nox_3ds_get_ticks_ms();

    /* Drain touch/button state and clear any captured GUI widget so tapping
     * OK/Cancel on swkbd never leaks a click or stuck capture into Nox UI. */
    nox_write_u32(&byte_5D4594[1064916], 0);
    nox_3ds_input_reset_after_applet();

    if (button == SWKBD_BUTTON_CONFIRM) {
        wchar_t wbuf[256];
        int wlen = utf8_to_utf16_filtered(out_utf8, wbuf, max_chars,
                                          digits_only != 0, alnum_only != 0);

        /* Directly inject into the active ENTRYFIELD buffer via NOX_MSG_EF_SET_TEXT (16414),
         * updating text, text_len (+1052), and clearing IME composition state without
         * simulating individual keystrokes. */
        sub_46B490((int)(uintptr_t)entry_win, NOX_MSG_EF_SET_TEXT, (int)(uintptr_t)wbuf, 0);

        fprintf(stderr, "[swkbd3ds] Injected %d chars into ENTRYFIELD id=%d\n", wlen, (int)widget_id);

        if (auto_submit && parent) {
            sub_46B490((int)(uintptr_t)parent, NOX_MSG_EF_SUBMIT, (int)(uintptr_t)entry_win, 0);
        }
    } else {
        /* User cancelled: if multiplayer chat overlay was opened, close it cleanly. */
        void *chat_win = (void *)(uintptr_t)nox_read_u32(&byte_5D4594[1064860]);
        if ((auto_submit || entry_win == chat_win) && sub_46A4A0()) {
            sub_46A6A0();
        }
    }

    return true;
}
