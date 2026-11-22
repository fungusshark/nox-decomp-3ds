#include "platform_3ds.h"

#include <3ds.h>
#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    int field_0;
    int field_4;
} nox_int2_t;

/* Engine state queries (src/GAME1.c, src/GAME2.c) */
extern int sub_43C700(void);
extern int sub_450560(void);
extern int sub_477600(void);
extern int sub_45D9C0(void);
extern int sub_467C80(void);
extern int sub_459DA0(void);
extern unsigned int sub_446360(void);
extern int sub_413A50(void);
extern nox_int2_t *sub_4309F0(void);
extern char *sub_437250(void);

/* Input injectors (src/input.c) */
extern void nox_ctrl_inject_mouse_move(int dx, int dy, int wheel);
extern void nox_ctrl_inject_mouse_abs(int x, int y);
extern void nox_ctrl_inject_mouse_button(int button, int down);
extern void nox_ctrl_inject_key_scancode(int sdl_scancode, int down);
extern void nox_ctrl_set_thumbstick_move(int active, int orientation, int move_cmd);
extern void nox_ctrl_set_thumbstick_jump(int jump);
extern void nox_ctrl_set_radial_aim(int active, int orientation, int screen_x, int screen_y);

/* Gameplay bottom-screen touch hotkeys (1-5 spell/ability bar strip at bottom edge) */
#define TOUCH_SPELLBAR_Y_MIN  196
#define TOUCH_SPELLBAR_SLOTS  5

static nox_3ds_input_t g_in;
static bool g_prev_touch = false;
static int  g_active_spell_hotkey = -1;
static bool g_touch_aim_was_active = false;
static bool g_cstick_aim_latched = false;
static int  g_last_aim_x = NOX_3DS_LOGICAL_W / 2;
static int  g_last_aim_y = NOX_3DS_LOGICAL_H / 2;
static int  g_last_aim_orient = 0;

static float read_env_float(const char *name, float defv, float lo, float hi)
{
    const char *s = getenv(name);
    if (!s || !*s)
        return defv;
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == s)
        return defv;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

void nox_3ds_input_init(void)
{
    memset(&g_in, 0, sizeof(g_in));
    g_in.aim_mode = NOX_3DS_AIM_RADIAL;
    g_in.cstick_mode = NOX_3DS_CSTICK_AIM_CAMERA;
    g_in.touch_to_aim = true;
    g_in.cstick_cfg.deadzone    = read_env_float("NOX_3DS_CSTICK_DEADZONE", 0.15f, 0.02f, 0.85f);
    g_in.cstick_cfg.curve_exp   = read_env_float("NOX_3DS_CSTICK_CURVE",    1.85f, 1.0f,  4.0f);
    g_in.cstick_cfg.sensitivity = read_env_float("NOX_3DS_CSTICK_SENS",     1.0f,  0.2f,  4.0f);
    g_in.cstick_cfg.min_radius  = 56.0f;
    g_in.cstick_cfg.max_radius  = 210.0f; /* wider radius gives camera look-ahead + spell range */

    g_prev_touch = false;
    g_active_spell_hotkey = -1;
    g_touch_aim_was_active = false;
    g_cstick_aim_latched = false;
    g_last_aim_x = NOX_3DS_LOGICAL_W / 2;
    g_last_aim_y = NOX_3DS_LOGICAL_H / 2;
    g_last_aim_orient = 0;

    printf("[input3ds] New 3DS scheme ready (CStick dz=%.2f curve=%.2f)\n",
           g_in.cstick_cfg.deadzone, g_in.cstick_cfg.curve_exp);
    printf("[input3ds] Combos: SEL+L=Aspect SEL+R=TouchAim SEL+ZL=CursorMode SEL+X=CStickMode\n");
}

void nox_3ds_input_set_cstick_params(float deadzone, float curve_exp, float sensitivity)
{
    if (deadzone < 0.02f) deadzone = 0.02f;
    if (deadzone > 0.85f) deadzone = 0.85f;
    if (curve_exp < 1.0f) curve_exp = 1.0f;
    if (curve_exp > 4.0f) curve_exp = 4.0f;
    if (sensitivity < 0.2f) sensitivity = 0.2f;
    if (sensitivity > 4.0f) sensitivity = 4.0f;
    g_in.cstick_cfg.deadzone = deadzone;
    g_in.cstick_cfg.curve_exp = curve_exp;
    g_in.cstick_cfg.sensitivity = sensitivity;
}

void nox_3ds_input_toggle_aim_mode(void)
{
    nox_3ds_state_t *st = nox_3ds_state_mut();
    if (g_in.aim_mode == NOX_3DS_AIM_RADIAL) {
        g_in.aim_mode = NOX_3DS_AIM_FREE_CURSOR;
        g_cstick_aim_latched = false;
        nox_ctrl_set_thumbstick_move(0, 0, 0);
        nox_ctrl_set_radial_aim(0, 0, 0, 0);
    } else {
        g_in.aim_mode = NOX_3DS_AIM_RADIAL;
    }
    st->aim_mode = g_in.aim_mode;
    printf("[input3ds] Control Mode: %s\n",
           (g_in.aim_mode == NOX_3DS_AIM_RADIAL) ? "Radial Twin-Stick" : "Free-Cursor");
}

void nox_3ds_input_toggle_cstick_mode(void)
{
    nox_3ds_state_t *st = nox_3ds_state_mut();
    if (g_in.cstick_mode == NOX_3DS_CSTICK_AIM_CAMERA) {
        g_in.cstick_mode = NOX_3DS_CSTICK_SPELL_CAST;
        g_cstick_aim_latched = false;
        if (!g_touch_aim_was_active) {
            nox_ctrl_set_radial_aim(0, 0, 0, 0);
        }
    } else {
        g_in.cstick_mode = NOX_3DS_CSTICK_AIM_CAMERA;
    }
    st->cstick_mode = g_in.cstick_mode;
    printf("[input3ds] C-Stick: %s\n",
           (g_in.cstick_mode == NOX_3DS_CSTICK_AIM_CAMERA) ? "Aim/Camera" : "Quick-Spell (A/S/D/F)");
}

void nox_3ds_input_toggle_touch_aim(void)
{
    nox_3ds_state_t *st = nox_3ds_state_mut();
    g_in.touch_to_aim = !g_in.touch_to_aim;
    st->touch_to_aim = g_in.touch_to_aim;
    printf("[input3ds] Touch-to-Aim: %s\n", g_in.touch_to_aim ? "ON" : "OFF");
}

bool nox_3ds_is_gameplay_active(void)
{
    if (!sub_43C700())
        return false;
    if (sub_450560() || sub_477600() || sub_45D9C0())
        return false;
    if (sub_467C80() || sub_459DA0() || sub_446360() || sub_413A50())
        return false;
    return true;
}

void nox_3ds_input_poll(nox_3ds_input_t *out)
{
    circlePosition cpad;
    circlePosition cstick;
    touchPosition touch;

    hidScanInput();
    g_in.keys_held = hidKeysHeld();
    g_in.keys_down = hidKeysDown();
    g_in.keys_up   = hidKeysUp();

    memset(&cpad, 0, sizeof(cpad));
    memset(&cstick, 0, sizeof(cstick));
    memset(&touch, 0, sizeof(touch));
    hidCircleRead(&cpad);
    hidCstickRead(&cstick);

    g_in.cpad_x   = cpad.dx;
    g_in.cpad_y   = cpad.dy;
    g_in.cstick_x = cstick.dx;
    g_in.cstick_y = cstick.dy;

    bool cur_touch = (g_in.keys_held & KEY_TOUCH) != 0;
    if (cur_touch) {
        hidTouchRead(&touch);
        g_in.touch_x = (touch.px < NOX_3DS_BOT_W) ? touch.px : (NOX_3DS_BOT_W - 1);
        g_in.touch_y = (touch.py < NOX_3DS_BOT_H) ? touch.py : (NOX_3DS_BOT_H - 1);
    }
    g_in.touch_down   = (cur_touch && !g_prev_touch);
    g_in.touch_up     = (!cur_touch && g_prev_touch);
    g_in.touch_active = cur_touch;
    g_prev_touch      = cur_touch;

    g_in.in_gameplay = nox_3ds_is_gameplay_active();

    /* Reset SELECT combo latch when SELECT starts a new press. */
    if (g_in.keys_down & KEY_SELECT) {
        g_in.select_combo_used = false;
    }

    /* System hot-toggles: chorded with SELECT so L+ZL (mana potion) and bare ZL never conflict. */
    if (g_in.keys_held & KEY_SELECT) {
        if (g_in.keys_down & KEY_L) {
            nox_3ds_video_toggle_wide();
            g_in.select_combo_used = true;
        }
        if (g_in.keys_down & KEY_R) {
            nox_3ds_input_toggle_touch_aim();
            g_in.select_combo_used = true;
        }
        if (g_in.keys_down & KEY_ZL) {
            nox_3ds_input_toggle_aim_mode();
            g_in.select_combo_used = true;
        }
        if (g_in.keys_down & KEY_X) {
            nox_3ds_input_toggle_cstick_mode();
            g_in.select_combo_used = true;
        }
    }

    if (out) {
        *out = g_in;
    }
}

const nox_3ds_input_t *nox_3ds_input_get(void)
{
    return &g_in;
}

static void get_viewport_center(int *out_cx, int *out_cy)
{
    int *vp = (int *)(void *)sub_437250();
    if (vp && (vp[2] - vp[0]) > 0 && (vp[3] - vp[1]) > 0) {
        *out_cx = vp[0] + (vp[2] - vp[0]) / 2;
        *out_cy = vp[1] + (vp[3] - vp[1]) / 2;
    } else {
        *out_cx = NOX_3DS_LOGICAL_W / 2;
        *out_cy = NOX_3DS_LOGICAL_H / 2;
    }
}

static int orientation_from_vec(float fx, float fy)
{
    /* Nox orientation: 0..255 with atan2(-fy, -fx) matching input.c / gamepad.c */
    double theta = atan2(-(double)fy, -(double)fx) / M_PI;
    return ((int)((theta + 1.0) * 128.0 + 0.5)) & 255;
}

static void map_touch_to_engine(uint16_t tx, uint16_t ty, int *ex, int *ey)
{
    int x = ((int)tx * NOX_3DS_LOGICAL_W) / NOX_3DS_BOT_W;
    int y = ((int)ty * NOX_3DS_LOGICAL_H) / NOX_3DS_BOT_H;
    if (x < 0) x = 0;
    if (x >= NOX_3DS_LOGICAL_W) x = NOX_3DS_LOGICAL_W - 1;
    if (y < 0) y = 0;
    if (y >= NOX_3DS_LOGICAL_H) y = NOX_3DS_LOGICAL_H - 1;
    *ex = x;
    *ey = y;
}

static int spell_scancode_for_slot(int slot_idx)
{
    static const int k_spell_keys[TOUCH_SPELLBAR_SLOTS] = {
        SDL_SCANCODE_A,
        SDL_SCANCODE_S,
        SDL_SCANCODE_D,
        SDL_SCANCODE_F,
        SDL_SCANCODE_G
    };
    if (slot_idx < 0 || slot_idx >= TOUCH_SPELLBAR_SLOTS)
        return -1;
    return k_spell_keys[slot_idx];
}

static void release_active_spell_hotkey(void)
{
    if (g_active_spell_hotkey >= 0) {
        int sc = spell_scancode_for_slot(g_active_spell_hotkey);
        if (sc >= 0)
            nox_ctrl_inject_key_scancode(sc, 0);
        g_active_spell_hotkey = -1;
    }
}

static bool compute_cstick_curved(float *out_nx, float *out_ny, float *out_mag)
{
    /* New 3DS C-Stick nub typical active range is ~[-140..+140], +Y is up */
    float fx = (float)g_in.cstick_x / 140.0f;
    float fy = -(float)g_in.cstick_y / 140.0f;
    float mag = sqrtf(fx * fx + fy * fy);
    if (mag > 1.0f) {
        fx /= mag;
        fy /= mag;
        mag = 1.0f;
    }

    float dz = g_in.cstick_cfg.deadzone;
    if (mag <= dz) {
        *out_nx = 0.0f;
        *out_ny = 0.0f;
        *out_mag = 0.0f;
        return false;
    }

    float u = (mag - dz) / (1.0f - dz);
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;

    float curved = powf(u, g_in.cstick_cfg.curve_exp) * g_in.cstick_cfg.sensitivity;
    if (curved > 1.0f) curved = 1.0f;

    *out_nx = fx / mag;
    *out_ny = fy / mag;
    *out_mag = curved;
    return true;
}

void nox_3ds_input_process_twin_stick_and_touch(void)
{
    /*
     * 1. Touchscreen handling:
     *    - Menus / Inventory / Spellbook / Dialogs / Free-Cursor mode:
     *      Scaled 1:1 cursor + left click mapping (320x240 -> 640x480).
     *    - Gameplay (Radial Twin-Stick mode):
     *      Bottom strip (y >= 196) maps to spell/ability bar hotkeys 1-5 (A/S/D/F/G).
     *      Upper area (y < 196) performs optional touch-to-aim (or click if disabled).
     */
    if (!g_in.in_gameplay || g_in.aim_mode == NOX_3DS_AIM_FREE_CURSOR) {
        release_active_spell_hotkey();
        g_touch_aim_was_active = false;

        if (g_in.touch_active) {
            int ex = 0, ey = 0;
            map_touch_to_engine(g_in.touch_x, g_in.touch_y, &ex, &ey);
            nox_ctrl_inject_mouse_abs(ex, ey);
            if (g_in.touch_down) {
                nox_ctrl_inject_mouse_button(0, 1);
            }
        } else if (g_in.touch_up) {
            nox_ctrl_inject_mouse_button(0, 0);
        }

        /* In Free-Cursor / menus (when START isn't held for wordsets), C-Stick nudges cursor/camera */
        if (!(g_in.keys_held & KEY_START)) {
            float nx = 0.0f, ny = 0.0f, curved = 0.0f;
            if (compute_cstick_curved(&nx, &ny, &curved)) {
                int dx = (int)lrintf(nx * curved * 10.0f);
                int dy = (int)lrintf(ny * curved * 10.0f);
                if (dx != 0 || dy != 0) {
                    nox_ctrl_inject_mouse_move(dx, dy, 0);
                }
            }
        }
        nox_ctrl_set_radial_aim(0, 0, 0, 0);
        return;
    }

    /* Gameplay in Radial Twin-Stick mode */
    if (g_in.touch_active) {
        if (g_in.touch_y >= TOUCH_SPELLBAR_Y_MIN) {
            int slot = ((int)g_in.touch_x * TOUCH_SPELLBAR_SLOTS) / NOX_3DS_BOT_W;
            if (slot < 0) slot = 0;
            if (slot >= TOUCH_SPELLBAR_SLOTS) slot = TOUCH_SPELLBAR_SLOTS - 1;

            if (g_active_spell_hotkey != slot) {
                release_active_spell_hotkey();
                int sc = spell_scancode_for_slot(slot);
                if (sc >= 0) {
                    nox_ctrl_inject_key_scancode(sc, 1);
                    g_active_spell_hotkey = slot;
                }
            }
        } else {
            release_active_spell_hotkey();
            int ex = 0, ey = 0;
            map_touch_to_engine(g_in.touch_x, g_in.touch_y, &ex, &ey);
            if (g_in.touch_to_aim) {
                int cx = 0, cy = 0;
                get_viewport_center(&cx, &cy);
                float dx = (float)(ex - cx);
                float dy = (float)(ey - cy);
                g_last_aim_x = ex;
                g_last_aim_y = ey;
                if (dx * dx + dy * dy > 4.0f) {
                    g_last_aim_orient = orientation_from_vec(dx, dy);
                }
                g_cstick_aim_latched = true;
                g_touch_aim_was_active = true;
            } else {
                nox_ctrl_inject_mouse_abs(ex, ey);
                if (g_in.touch_down) {
                    nox_ctrl_inject_mouse_button(0, 1);
                }
            }
        }
    } else {
        release_active_spell_hotkey();
        if (g_in.touch_up && !g_in.touch_to_aim) {
            nox_ctrl_inject_mouse_button(0, 0);
        }
        g_touch_aim_was_active = false;
    }

    /*
     * 2. C-Stick handling in gameplay:
     *    - Aim/Camera mode (default, unless START utility layer is held):
     *      Projects aiming reticle & camera look-ahead around viewport center (cx, cy).
     *    - Quick-Spell mode (toggled via SELECT+X):
     *      gamepad.c dispatches C-Stick Up/Right/Down/Left to spell slots A/S/D/F.
     */
    if (g_in.cstick_mode == NOX_3DS_CSTICK_AIM_CAMERA && !(g_in.keys_held & KEY_START)) {
        float cnx = 0.0f, cny = 0.0f, cmag = 0.0f;
        if (compute_cstick_curved(&cnx, &cny, &cmag)) {
            int cx = 0, cy = 0;
            get_viewport_center(&cx, &cy);
            float radius = g_in.cstick_cfg.min_radius +
                           cmag * (g_in.cstick_cfg.max_radius - g_in.cstick_cfg.min_radius);
            int ax = cx + (int)lrintf(cnx * radius);
            int ay = cy + (int)lrintf(cny * radius);
            if (ax < 0) ax = 0;
            if (ax >= NOX_3DS_LOGICAL_W) ax = NOX_3DS_LOGICAL_W - 1;
            if (ay < 0) ay = 0;
            if (ay >= NOX_3DS_LOGICAL_H) ay = NOX_3DS_LOGICAL_H - 1;

            g_last_aim_x = ax;
            g_last_aim_y = ay;
            g_last_aim_orient = orientation_from_vec(cnx, cny);
            g_cstick_aim_latched = true;

            nox_ctrl_set_radial_aim(1, g_last_aim_orient, ax, ay);
            return;
        }
    }

    /* Retain aim reticle during touch-to-aim or stationary C-Stick aim; release when moving */
    bool cpad_moving = (abs(g_in.cpad_x) > 34 || abs(g_in.cpad_y) > 34);
    if (g_touch_aim_was_active ||
        (g_in.cstick_mode == NOX_3DS_CSTICK_AIM_CAMERA && g_cstick_aim_latched && !cpad_moving)) {
        nox_ctrl_set_radial_aim(1, g_last_aim_orient, g_last_aim_x, g_last_aim_y);
    } else {
        g_cstick_aim_latched = false;
        nox_ctrl_set_radial_aim(0, 0, 0, 0);
    }
}
