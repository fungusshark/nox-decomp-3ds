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

/* Engine state queries (src/GAME1.c, src/GAME2.c, src/GAME_data.c) */
extern unsigned char byte_5D4594[];
extern int sub_43C700(void);           /* world/map loaded */
extern int sub_450560(void);           /* chat text entry active */
extern int sub_459DA0(void);           /* NPC dialog / shop modal active */
extern unsigned int sub_446360(void);  /* Quit / Pause menu active */
extern int sub_413A50(void);           /* Yes/No confirmation modal active */
extern char *sub_437250(void);         /* gameplay viewport rect [l,t,r,b,...] */

/* Input injectors (src/input.c) */
extern void nox_ctrl_inject_mouse_abs(int x, int y);
extern void nox_ctrl_inject_mouse_button(int button, int down);
extern void nox_ctrl_set_radial_aim(int active, int orientation, int screen_x, int screen_y);
extern void nox_ctrl_set_touch_move(int active, int orientation, int move_cmd);

enum {
    LMB_OWNER_NONE    = 0,
    LMB_OWNER_TOUCH   = 1,
    LMB_OWNER_WORLD_R = 2
};

enum {
    TOUCH_MODE_NONE = 0,
    TOUCH_MODE_UI   = 1,
    TOUCH_MODE_MOVE = 2
};

static nox_3ds_input_t g_in;
static bool  g_prev_touch          = false;
static bool  g_prev_l_held         = false;
static bool  g_cstick_or_r_active  = false;
static int   g_lmb_owner           = LMB_OWNER_NONE;
static int   g_touch_mode          = TOUCH_MODE_NONE;

/* Top-screen world aiming state (always kept in 640x480 coords clamped to 400x240 viewport) */
static float g_aim_dir_x        = 0.0f;
static float g_aim_dir_y        = 1.0f; /* default facing south */
static float g_free_aim_x       = (float)(NOX_3DS_LOGICAL_W / 2);
static float g_free_aim_y       = (float)(NOX_3DS_LOGICAL_H / 2) + NOX_3DS_AIM_CLOSE_RADIUS;
static int   g_world_aim_orient = 64;   /* south */

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

static int orientation_from_vec(float fx, float fy)
{
    /* Nox orientation: 0..255 with atan2(-fy, -fx) matching src/input.c & src/gamepad.c */
    double theta = atan2(-(double)fy, -(double)fx) / M_PI;
    return ((int)((theta + 1.0) * 128.0 + 0.5)) & 255;
}

static void get_top_viewport_bounds(int *out_l, int *out_t, int *out_r, int *out_b,
                                    int *out_cx, int *out_cy)
{
    int l = NOX_3DS_VP_LEFT;
    int t = NOX_3DS_VP_TOP;
    int r = NOX_3DS_VP_RIGHT;
    int b = NOX_3DS_VP_BOTTOM;

    int *vp = (int *)(void *)sub_437250();
    if (vp && (vp[2] - vp[0]) >= 64 && (vp[3] - vp[1]) >= 64) {
        l = vp[0];
        t = vp[1];
        r = vp[2];
        b = vp[3];
    }

    if (out_l)  *out_l  = l;
    if (out_t)  *out_t  = t;
    if (out_r)  *out_r  = r;
    if (out_b)  *out_b  = b;
    if (out_cx) *out_cx = l + (r - l + 1) / 2;
    if (out_cy) *out_cy = t + (b - t + 1) / 2;
}

static void map_touch_to_bottom_ui(uint16_t tx, uint16_t ty, int *ex, int *ey)
{
    /* Exact 2:1 scale: 320x240 bottom screen -> 640x480 GUI coordinate space */
    int x = ((int)tx * NOX_3DS_LOGICAL_W) / NOX_3DS_BOT_W;
    int y = ((int)ty * NOX_3DS_LOGICAL_H) / NOX_3DS_BOT_H;
    if (x < 0) x = 0;
    if (x >= NOX_3DS_LOGICAL_W) x = NOX_3DS_LOGICAL_W - 1;
    if (y < 0) y = 0;
    if (y >= NOX_3DS_LOGICAL_H) y = NOX_3DS_LOGICAL_H - 1;
    *ex = x;
    *ey = y;
}

static bool compute_cstick_curved(float *out_nx, float *out_ny, float *out_mag)
{
    /* New 3DS C-Stick nub active range is ~[-140..+140], +Y is up */
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

static void release_lmb_if_owned(int owner)
{
    if (g_lmb_owner == owner) {
        nox_ctrl_inject_mouse_button(0, 0);
        g_lmb_owner = LMB_OWNER_NONE;
    }
}

void nox_3ds_input_init(void)
{
    memset(&g_in, 0, sizeof(g_in));
    g_in.cstick_cfg.deadzone     = read_env_float("NOX_3DS_CSTICK_DEADZONE", 0.14f, 0.02f, 0.85f);
    g_in.cstick_cfg.curve_exp    = read_env_float("NOX_3DS_CSTICK_CURVE",    1.75f, 1.0f,  4.0f);
    g_in.cstick_cfg.sensitivity  = read_env_float("NOX_3DS_CSTICK_SENS",     1.0f,  0.2f,  4.0f);
    g_in.cstick_cfg.close_radius = read_env_float("NOX_3DS_AIM_RADIUS", NOX_3DS_AIM_CLOSE_RADIUS, 10.0f, 36.0f);
    g_in.cstick_cfg.free_speed   = read_env_float("NOX_3DS_FREE_AIM_SPEED", NOX_3DS_FREE_AIM_SPEED, 2.0f, 20.0f);

    g_in.world_aim_x    = NOX_3DS_LOGICAL_W / 2;
    g_in.world_aim_y    = (NOX_3DS_LOGICAL_H / 2) + (int)lrintf(g_in.cstick_cfg.close_radius);
    g_in.bottom_touch_x = NOX_3DS_LOGICAL_W / 2;
    g_in.bottom_touch_y = NOX_3DS_LOGICAL_H / 2;

    g_prev_touch       = false;
    g_prev_l_held      = false;
    g_lmb_owner        = LMB_OWNER_NONE;
    g_aim_dir_x        = 0.0f;
    g_aim_dir_y        = 1.0f;
    g_free_aim_x       = (float)g_in.world_aim_x;
    g_free_aim_y       = (float)g_in.world_aim_y;
    g_world_aim_orient = orientation_from_vec(g_aim_dir_x, g_aim_dir_y);

    fprintf(stderr, "[input3ds] Hybrid input ready (closeR=%.0fpx, L-freeSpeed=%.1f, dz=%.2f)\n",
            g_in.cstick_cfg.close_radius, g_in.cstick_cfg.free_speed, g_in.cstick_cfg.deadzone);
}

void nox_3ds_input_set_cstick_params(float deadzone, float curve_exp, float sensitivity)
{
    if (deadzone < 0.02f) deadzone = 0.02f;
    if (deadzone > 0.85f) deadzone = 0.85f;
    if (curve_exp < 1.0f) curve_exp = 1.0f;
    if (curve_exp > 4.0f) curve_exp = 4.0f;
    if (sensitivity < 0.2f) sensitivity = 0.2f;
    if (sensitivity > 4.0f) sensitivity = 4.0f;
    g_in.cstick_cfg.deadzone    = deadzone;
    g_in.cstick_cfg.curve_exp   = curve_exp;
    g_in.cstick_cfg.sensitivity = sensitivity;
}

/*
 * Dual-screen World Mode:
 * True whenever the Top Screen is displaying live gameplay (NOT Main Menu,
 * Pause/Quit menu, Yes/No popup, Chat input, or NPC Dialog/Shop modal).
 * Note: Inventory and Spellbook live on the Bottom Screen and intentionally
 * do NOT block Top-Screen movement, aiming, spells, or R-click combat!
 */
bool nox_3ds_input_is_world_mode(void)
{
    if (nox_read_s32(&byte_5D4594[815132]) != 0) /* Main Menu shell active */
        return false;
    if (!sub_43C700()) /* Map not loaded */
        return false;
    if (sub_446360() || sub_413A50() || sub_450560() || sub_459DA0())
        return false;
    return true;
}

bool nox_3ds_is_gameplay_active(void)
{
    return nox_3ds_input_is_world_mode();
}

bool nox_3ds_input_is_bottom_touch_frame(void)
{
    return g_in.bottom_touch_frame;
}

bool nox_3ds_input_is_ranged_aim_held(void)
{
    return g_in.ranged_aim_held;
}

bool nox_3ds_input_is_cstick_or_r_active(void)
{
    return g_cstick_or_r_active;
}

bool nox_3ds_input_is_touch_move_active(void)
{
    return (g_in.in_gameplay && g_in.touch_active && g_touch_mode == TOUCH_MODE_MOVE);
}

bool nox_3ds_input_is_cpad_move_active(void)
{
    if (!g_in.in_gameplay)
        return false;
    float lx = (float)g_in.cpad_x / 156.0f;
    float ly = (float)g_in.cpad_y / 156.0f;
    return (lx * lx + ly * ly) > (0.18f * 0.18f);
}

/*
 * Called from sub_46B740() after Bottom-Screen GUI hit-testing in World Mode:
 *  - Touching a UI widget locks this touch to UI interaction (buttons, inventory, weapon swap).
 *  - Touching empty space (not on any UI widget) displays the Bottom-Screen cursor and
 *    moves/runs the hero relative to the center of the Bottom Screen (320, 240).
 */
void nox_3ds_input_notify_bottom_ui_hit(bool hit_ui)
{
    if (!g_in.in_gameplay || !g_in.touch_active) {
        g_touch_mode = TOUCH_MODE_NONE;
        nox_ctrl_set_touch_move(0, 0, 0);
        return;
    }

    if (g_touch_mode == TOUCH_MODE_NONE) {
        g_touch_mode = hit_ui ? TOUCH_MODE_UI : TOUCH_MODE_MOVE;
    }

    if (g_touch_mode == TOUCH_MODE_MOVE) {
        float dx = (float)(g_in.bottom_touch_x - (NOX_3DS_LOGICAL_W / 2));
        float dy = (float)(g_in.bottom_touch_y - (NOX_3DS_LOGICAL_H / 2));
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist >= 16.0f) {
            int orient = orientation_from_vec(dx / dist, dy / dist);
            int cmd = (dist >= 85.0f) ? 3 : 1; /* close = walk, outer = run */
            nox_ctrl_set_touch_move(1, orient, cmd);
        } else {
            nox_ctrl_set_touch_move(0, 0, 0);
        }
    } else {
        nox_ctrl_set_touch_move(0, 0, 0);
    }
}

void nox_3ds_input_get_world_aim(int *out_x, int *out_y)
{
    if (out_x) *out_x = g_in.world_aim_x;
    if (out_y) *out_y = g_in.world_aim_y;
}

void nox_3ds_input_get_bottom_touch(int *out_x, int *out_y)
{
    if (out_x) *out_x = g_in.bottom_touch_x;
    if (out_y) *out_y = g_in.bottom_touch_y;
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

    /* Strict Bottom Screen touchscreen read (0..319, 0..239) */
    bool cur_touch = (g_in.keys_held & KEY_TOUCH) != 0;
    if (cur_touch) {
        hidTouchRead(&touch);
        g_in.touch_x = (touch.px < NOX_3DS_BOT_W) ? touch.px : (NOX_3DS_BOT_W - 1);
        g_in.touch_y = (touch.py < NOX_3DS_BOT_H) ? touch.py : (NOX_3DS_BOT_H - 1);
        map_touch_to_bottom_ui(g_in.touch_x, g_in.touch_y,
                               &g_in.bottom_touch_x, &g_in.bottom_touch_y);
    }
    g_in.touch_down   = (cur_touch && !g_prev_touch);
    g_in.touch_up     = (!cur_touch && g_prev_touch);
    g_in.touch_active = cur_touch;
    g_prev_touch      = cur_touch;

    g_in.in_gameplay = nox_3ds_input_is_world_mode();

    if (out) {
        *out = g_in;
    }
}

const nox_3ds_input_t *nox_3ds_input_get(void)
{
    return &g_in;
}

void nox_3ds_input_process_hybrid(void)
{
    const bool world_mode = g_in.in_gameplay;

    /* =====================================================================
     * 1. Top-Screen World Aiming: C-Stick + L Modifier
     *    - Default (L not held): Tight fixed radius (~18px) right next to the
     *      character so adjacent doors, items, and melee targets hover naturally.
     *    - Hold L: Detaches into free-aim cursor across the full 400x240 Top
     *      Screen for ranged spells; snaps cleanly back to tight ring on release.
     * ===================================================================== */
    if (world_mode) {
        int vp_l, vp_t, vp_r, vp_b, cx, cy;
        get_top_viewport_bounds(&vp_l, &vp_t, &vp_r, &vp_b, &cx, &cy);

        bool l_held = (g_in.keys_held & NOX_3DS_KEY_L) != 0;
        bool l_down = (l_held && !g_prev_l_held);
        bool l_up   = (!l_held && g_prev_l_held);
        g_prev_l_held = l_held;
        g_in.ranged_aim_held = l_held;

        float cnx = 0.0f, cny = 0.0f, cmag = 0.0f;
        bool cstick_active = compute_cstick_curved(&cnx, &cny, &cmag);
        bool r_held_now    = (g_in.keys_held & NOX_3DS_KEY_R) != 0;
        g_cstick_or_r_active = (cstick_active || l_held || r_held_now);

        if (l_held) {
            /* Entering L-ranged mode: start detached cursor from current close-aim point */
            if (l_down) {
                g_free_aim_x = (float)g_in.world_aim_x;
                g_free_aim_y = (float)g_in.world_aim_y;
            }

            if (cstick_active) {
                g_free_aim_x += cnx * cmag * g_in.cstick_cfg.free_speed;
                g_free_aim_y += cny * cmag * g_in.cstick_cfg.free_speed;
            }

            /* Clamp strictly to the 400x240 Top Screen viewport bounds */
            if (g_free_aim_x < (float)vp_l) g_free_aim_x = (float)vp_l;
            if (g_free_aim_x > (float)vp_r) g_free_aim_x = (float)vp_r;
            if (g_free_aim_y < (float)vp_t) g_free_aim_y = (float)vp_t;
            if (g_free_aim_y > (float)vp_b) g_free_aim_y = (float)vp_b;

            g_in.world_aim_x = (int)lrintf(g_free_aim_x);
            g_in.world_aim_y = (int)lrintf(g_free_aim_y);

            float dx = g_free_aim_x - (float)cx;
            float dy = g_free_aim_y - (float)cy;
            float dlen = sqrtf(dx * dx + dy * dy);
            if (dlen > 2.0f) {
                g_aim_dir_x = dx / dlen;
                g_aim_dir_y = dy / dlen;
                g_world_aim_orient = orientation_from_vec(g_aim_dir_x, g_aim_dir_y);
            }
        } else {
            /* Releasing L: snap back to tight fixed radius along last aim angle */
            if (l_up) {
                float dx = g_free_aim_x - (float)cx;
                float dy = g_free_aim_y - (float)cy;
                float dlen = sqrtf(dx * dx + dy * dy);
                if (dlen > 2.0f) {
                    g_aim_dir_x = dx / dlen;
                    g_aim_dir_y = dy / dlen;
                }
            }

            if (cstick_active) {
                /* Direct C-Stick aim on tight fixed radius (~18px) */
                g_aim_dir_x = cnx;
                g_aim_dir_y = cny;
                g_world_aim_orient = orientation_from_vec(g_aim_dir_x, g_aim_dir_y);
            } else {
                /* When walking with Circle Pad and not touching C-Stick, smoothly
                 * keep the ~18px close-aim probe directly ahead of the hero so
                 * adjacent doors, chests, and items highlight naturally! */
                float lx = (float)g_in.cpad_x / 156.0f;
                float ly = -(float)g_in.cpad_y / 156.0f;
                float lmag = sqrtf(lx * lx + ly * ly);
                if (lmag > 0.25f) {
                    g_aim_dir_x = lx / lmag;
                    g_aim_dir_y = ly / lmag;
                    g_world_aim_orient = orientation_from_vec(g_aim_dir_x, g_aim_dir_y);
                }
            }

            float r = g_in.cstick_cfg.close_radius;
            int ax = cx + (int)lrintf(g_aim_dir_x * r);
            int ay = cy + (int)lrintf(g_aim_dir_y * r);
            if (ax < vp_l) ax = vp_l;
            if (ax > vp_r) ax = vp_r;
            if (ay < vp_t) ay = vp_t;
            if (ay > vp_b) ay = vp_b;

            g_in.world_aim_x = ax;
            g_in.world_aim_y = ay;
            g_free_aim_x = (float)ax;
            g_free_aim_y = (float)ay;
        }

        nox_ctrl_set_radial_aim(1, g_world_aim_orient, g_in.world_aim_x, g_in.world_aim_y);
    } else {
        g_prev_l_held = false;
        g_cstick_or_r_active = false;
        g_touch_mode = TOUCH_MODE_NONE;
        g_in.ranged_aim_held = false;
        nox_ctrl_set_radial_aim(0, 0, 0, 0);
        nox_ctrl_set_touch_move(0, 0, 0);
    }

    /* =====================================================================
     * 2. Strict Screen Input Arbitration:
     *    - Bottom Screen Touch (hidTouchRead): 100% of menu, inventory,
     *      spellbook, and UI absolute Left-Click tapping.
     *    - Top Screen R Button: Left-Click at C-Stick world cursor location
     *      (both close melee/interact ring and L-held ranged free-aim).
     * ===================================================================== */
    if (g_in.touch_active || g_in.touch_up || !world_mode) {
        g_in.bottom_touch_frame = true;

        /* Ensure world R-click never stays latched while interacting with Bottom UI */
        release_lmb_if_owned(LMB_OWNER_WORLD_R);

        if (g_in.touch_active) {
            nox_ctrl_inject_mouse_abs(g_in.bottom_touch_x, g_in.bottom_touch_y);
            if (g_lmb_owner != LMB_OWNER_TOUCH) {
                nox_ctrl_inject_mouse_button(0, 1);
                g_lmb_owner = LMB_OWNER_TOUCH;
            }
        } else if (g_in.touch_up) {
            nox_ctrl_inject_mouse_abs(g_in.bottom_touch_x, g_in.bottom_touch_y);
            release_lmb_if_owned(LMB_OWNER_TOUCH);
            g_touch_mode = TOUCH_MODE_NONE;
            nox_ctrl_set_touch_move(0, 0, 0);
        } else {
            release_lmb_if_owned(LMB_OWNER_TOUCH);
            g_touch_mode = TOUCH_MODE_NONE;
            nox_ctrl_set_touch_move(0, 0, 0);
        }
        return;
    }

    /* World interaction frame (no bottom touch active): route pointer & R-click to Top Screen */
    g_in.bottom_touch_frame = false;
    g_touch_mode = TOUCH_MODE_NONE;
    nox_ctrl_set_touch_move(0, 0, 0);
    release_lmb_if_owned(LMB_OWNER_TOUCH);

    nox_ctrl_inject_mouse_abs(g_in.world_aim_x, g_in.world_aim_y);

    bool r_held = (g_in.keys_held & NOX_3DS_KEY_R) != 0;
    if (r_held && g_lmb_owner != LMB_OWNER_WORLD_R) {
        nox_ctrl_inject_mouse_button(0, 1);
        g_lmb_owner = LMB_OWNER_WORLD_R;
    } else if (!r_held && g_lmb_owner == LMB_OWNER_WORLD_R) {
        nox_ctrl_inject_mouse_button(0, 0);
        g_lmb_owner = LMB_OWNER_NONE;
    }
}

void nox_3ds_input_process_twin_stick_and_touch(void)
{
    nox_3ds_input_process_hybrid();
}
