#include "platform_3ds.h"
#include "perf_3ds.h"

#include <3ds.h>
#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Engine state queries (src/GAME1.c, src/GAME2.c, src/GAME3.c, src/GAME_data.c) */
typedef struct { int x; int y; } nox_3ds_i2_t;
typedef struct { int x0; int y0; int x1; int y1; } nox_3ds_i4_t;

extern unsigned char byte_5D4594[];
extern int sub_43C700(void);           /* world/map loaded */
extern int sub_450560(void);           /* console text entry active */
extern int sub_46A4A0(void);           /* multiplayer chat text entry active */
extern int sub_46A430(int a1);         /* open multiplayer chat + swkbd */
extern int sub_459DA0(void);           /* NPC dialog / shop modal active */
extern unsigned int sub_446360(void);  /* Quit / Pause menu active */
extern int sub_413A50(void);           /* Yes/No confirmation modal active */
extern char *sub_437250(void);         /* gameplay viewport rect [l,t,r,b,...] */
extern int sub_46AAB0(void *a1, int a2, int a3); /* GUI widget point-in-rect hit test */
extern int sub_46ACC0(int a1);                   /* GUI widget hidden check */
extern int sub_477600(void);                     /* dragging inventory item / spell icon */
extern void sub_473970(nox_3ds_i2_t *dst, nox_3ds_i2_t *src); /* screen -> world coords */
extern void sub_435670(nox_3ds_i2_t *dst);                    /* local hero world coords */
extern void sub_49AB00(nox_3ds_i4_t *box, void (*cb)(int, int), int userdata); /* client drawable spatial query */
extern int sub_4984B0(int dr);                   /* client drawable visible / LOS check */
extern int sub_4356C0(int dr, char enchant);     /* client drawable enchant check (0=invis, 21=infravision) */
extern int sub_419150(int team_a, int team_b);   /* same-team check */
extern int sub_4C3220(int dr);                   /* owned conjurer summon check */
extern char *sub_417040(int net_code);           /* client player_info lookup by net_code */

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
    TOUCH_MODE_NONE     = 0,
    TOUCH_MODE_UI       = 1,
    TOUCH_MODE_MOVE     = 2,
    TOUCH_MODE_CTX_MENU = 3
};

static nox_3ds_input_t g_in;
static bool  g_prev_touch          = false;
static bool  g_wait_touch_release  = false;
static bool  g_prev_l_held         = false;
static bool  g_cstick_or_r_active  = false;
static bool  g_pending_chat        = false;
static bool  g_pending_direct_ip   = false;
static int   g_lmb_owner           = LMB_OWNER_NONE;
static int   g_touch_mode          = TOUCH_MODE_NONE;

/* Top-screen world aiming state (always kept in 640x480 coords clamped to 640x384 viewport) */
static float g_aim_dir_x        = 0.0f;
static float g_aim_dir_y        = 1.0f; /* default facing south */
static float g_free_aim_x       = (float)(NOX_3DS_LOGICAL_W / 2);
static float g_free_aim_y       = (float)(NOX_3DS_LOGICAL_H / 2) + NOX_3DS_AIM_CLOSE_RADIUS;
static int   g_world_aim_orient = 64;   /* south */

/* Auto-flick attack/cast aim-override lock state (snaps orientation + exact 0xAC
 * to the aim reticle when attacking/casting while moving with the Circle Pad). */
static bool     g_aim_lock_active   = false;
static uint32_t g_aim_lock_until_ms = 0;
static int      g_aim_lock_orient   = 64;
static int      g_aim_lock_x        = NOX_3DS_LOGICAL_W / 2;
static int      g_aim_lock_y        = NOX_3DS_LOGICAL_H / 2;

/* World-anchored L-reticle camera scroll tracking */
static bool     g_prev_cam_valid    = false;
static int      g_prev_cam_ox       = 0;
static int      g_prev_cam_oy       = 0;

/* World-pinned spell cast target ("Cast Anchor"):
 * Holds 0xAC on the cast world spot throughout the server's phoneme chant
 * (replicated player anim == 21) so dodging early with the Circle Pad never
 * drags Meteor / Fist / Ring of Fire / Poison Cloud / Wall in front of you. */
static bool     g_spell_pin_active      = false;
static bool     g_spell_pin_saw_casting = false;
static uint32_t g_spell_pin_start_ms    = 0;
static int      g_spell_pin_wx          = 0;
static int      g_spell_pin_wy          = 0;

typedef struct {
    int   reticle_wx;
    int   reticle_wy;
    int   local_dr;
    int   local_infravision;
    float best_dist_sq;
    float best_dx;
    float best_dy;
    bool  found;
} aim_friction_ctx_t;

static void aim_friction_cb(int dr, int userdata)
{
    aim_friction_ctx_t *ctx = (aim_friction_ctx_t *)(uintptr_t)userdata;
    const uint8_t *base;
    uint32_t obj_flags, cls, subcls;
    int tx, ty;
    float dx, dy, d2;

    if (!dr || dr == ctx->local_dr)
        return;

    base = (const uint8_t *)(uintptr_t)dr;

    /* Fast integer pre-filter: live players (0x4) and monsters (0x2) only */
    cls = nox_read_u32(base + 112);
    if (!(cls & 0x6u))
        return;

    obj_flags = nox_read_u32(base + 120);
    if (obj_flags & 0x8000u)
        return;

    /* Reject out-of-range drawables BEFORE calling any engine helpers (LOS/team/invis) */
    tx = nox_read_s32(base + 12);
    ty = nox_read_s32(base + 16) - (int)nox_read_s16(base + 104) - 12;
    dx = (float)(tx - ctx->reticle_wx);
    dy = (float)(ty - ctx->reticle_wy);
    d2 = dx * dx + dy * dy;
    if (d2 >= ctx->best_dist_sq)
        return;

    /* Respect invisibility & infravision (never leak hidden enemies) */
    if (sub_4356C0(dr, 0) && !ctx->local_infravision)
        return;

    if (cls & 0x2u) {
        /* Skip friendly NPCs/shopkeepers, corpses (state 10), and own summons */
        subcls = nox_read_u32(base + 116);
        if (subcls & 0x4008u)
            return;
        if (nox_read_u32(base + 276) == 10u)
            return;
        if (sub_4C3220(dr))
            return;
    }

    if (cls & 0x4u) {
        /* Skip observers/spectators */
        int net_code = nox_read_s32(base + 128);
        char *pinfo = sub_417040(net_code);
        if (!pinfo || (pinfo[3680] & 1))
            return;
    }

    /* Skip same-team allies */
    if (ctx->local_dr && sub_419150(ctx->local_dr + 24, dr + 24))
        return;

    /* Respect fog-of-war / line-of-sight visibility */
    if (!sub_4984B0(dr))
        return;

    ctx->best_dist_sq = d2;
    ctx->best_dx = dx;
    ctx->best_dy = dy;
    ctx->found = true;
}

static float compute_aim_friction_scale(float cnx, float cny)
{
    float fmin = g_in.cstick_cfg.friction_min;
    float frad = g_in.cstick_cfg.friction_radius;
    nox_3ds_i2_t wpos;
    nox_3ds_i4_t box;
    aim_friction_ctx_t ctx;
    int local_dr, qpad;
    float dist, inner_r, t, scale;

    if (fmin >= 0.99f || frad <= 1.0f)
        return 1.0f;

    wpos.x = (int)lrintf(g_free_aim_x);
    wpos.y = (int)lrintf(g_free_aim_y);
    sub_473970(&wpos, &wpos);

    local_dr = (int)nox_read_u32(&byte_5D4594[2614252]);

    ctx.reticle_wx        = wpos.x;
    ctx.reticle_wy        = wpos.y;
    ctx.local_dr          = local_dr;
    ctx.local_infravision = (local_dr && sub_4356C0(local_dr, 21)) ? 1 : 0;
    ctx.best_dist_sq      = frad * frad;
    ctx.best_dx           = 0.0f;
    ctx.best_dy           = 0.0f;
    ctx.found             = false;

    qpad = (int)ceilf(frad) + 24;
    box.x0 = wpos.x - qpad;
    box.y0 = wpos.y - qpad;
    box.x1 = wpos.x + qpad;
    box.y1 = wpos.y + qpad;
    sub_49AB00(&box, aim_friction_cb, (int)(uintptr_t)&ctx);

    if (!ctx.found)
        return 1.0f;

    dist = sqrtf(ctx.best_dist_sq);
    inner_r = frad * 0.45f;
    if (dist <= inner_r) {
        t = 0.0f;
    } else {
        t = (dist - inner_r) / (frad - inner_r);
        if (t > 1.0f) t = 1.0f;
    }

    scale = fmin + t * (1.0f - fmin);

    /* Ease friction when deliberately pulling away to lead a moving target or disengage */
    if (dist > 2.0f) {
        float dot = (cnx * ctx.best_dx + cny * ctx.best_dy) / dist;
        if (dot < -0.25f && scale < 0.75f) {
            scale = 0.75f;
        }
    }
    return scale;
}

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

/* Conjurer CreatureCage context menu queries (src/GAME3.c guisumn.c):
 *  - byte_5D4594[1321032]: CreatureCage root window
 *  - byte_5D4594[1321036]: CreatureCage 2x2 creature portrait grid (sub_4C2B10)
 *  - byte_5D4594[1321040]: CreatureCage global order icon button  (sub_4C24B0)
 *  - byte_5D4594[1321044]: Active creature context menu popup     (sub_4C2560/sub_4C2470) */
static void *get_creature_ctx_menu(void)
{
    return (void *)(uintptr_t)nox_read_u32(&byte_5D4594[1321044]);
}

static bool is_creature_cage_hit(int x, int y)
{
    void *cage_root;
    void *cage_grid;
    void *cage_btn;

    if (get_creature_ctx_menu() != NULL)
        return false;
    if (nox_read_u32(&byte_5D4594[1064904]) != 0 || sub_477600() != 0)
        return false;

    cage_btn = (void *)(uintptr_t)nox_read_u32(&byte_5D4594[1321040]);
    if (cage_btn && !sub_46ACC0((int)(uintptr_t)cage_btn) && sub_46AAB0(cage_btn, x, y))
        return true;

    cage_root = (void *)(uintptr_t)nox_read_u32(&byte_5D4594[1321032]);
    cage_grid = (void *)(uintptr_t)nox_read_u32(&byte_5D4594[1321036]);
    if (cage_root && cage_grid && !sub_46ACC0((int)(uintptr_t)cage_root) && sub_46AAB0(cage_grid, x, y))
        return true;

    return false;
}

void nox_3ds_input_init(void)
{
    memset(&g_in, 0, sizeof(g_in));
    g_in.cstick_cfg.deadzone        = read_env_float("NOX_3DS_CSTICK_DEADZONE", 0.14f, 0.02f, 0.85f);
    g_in.cstick_cfg.curve_exp       = read_env_float("NOX_3DS_CSTICK_CURVE",    1.45f, 1.0f,  4.0f);
    g_in.cstick_cfg.sensitivity     = read_env_float("NOX_3DS_CSTICK_SENS",     1.0f,  0.2f,  4.0f);
    g_in.cstick_cfg.close_radius    = read_env_float("NOX_3DS_AIM_RADIUS",      NOX_3DS_AIM_CLOSE_RADIUS, 10.0f, 36.0f);
    g_in.cstick_cfg.free_speed      = read_env_float("NOX_3DS_FREE_AIM_SPEED",  NOX_3DS_FREE_AIM_SPEED, 2.0f, 28.0f);
    g_in.cstick_cfg.friction_min    = read_env_float("NOX_3DS_AIM_FRICTION",    NOX_3DS_AIM_FRICTION_MIN, 0.15f, 1.0f);
    g_in.cstick_cfg.friction_radius = read_env_float("NOX_3DS_AIM_FRICTION_RADIUS", NOX_3DS_AIM_FRICTION_RADIUS, 0.0f, 64.0f);

    g_in.world_aim_x    = NOX_3DS_LOGICAL_W / 2;
    g_in.world_aim_y    = (NOX_3DS_LOGICAL_H / 2) + (int)lrintf(g_in.cstick_cfg.close_radius);
    g_in.bottom_touch_x = NOX_3DS_LOGICAL_W / 2;
    g_in.bottom_touch_y = NOX_3DS_LOGICAL_H / 2;

    g_prev_touch        = false;
    g_prev_l_held       = false;
    g_lmb_owner         = LMB_OWNER_NONE;
    g_aim_dir_x         = 0.0f;
    g_aim_dir_y         = 1.0f;
    g_free_aim_x        = (float)g_in.world_aim_x;
    g_free_aim_y        = (float)g_in.world_aim_y;
    g_world_aim_orient  = orientation_from_vec(g_aim_dir_x, g_aim_dir_y);
    g_aim_lock_active       = false;
    g_aim_lock_until_ms     = 0;
    g_aim_lock_orient       = g_world_aim_orient;
    g_aim_lock_x            = g_in.world_aim_x;
    g_aim_lock_y            = g_in.world_aim_y;
    g_prev_cam_valid        = false;
    g_prev_cam_ox           = 0;
    g_prev_cam_oy           = 0;
    g_spell_pin_active      = false;
    g_spell_pin_saw_casting = false;
    g_spell_pin_start_ms    = 0;
    g_spell_pin_wx          = 0;
    g_spell_pin_wy          = 0;

    fprintf(stderr, "[input3ds] Hybrid input ready (closeR=%.0fpx, L-freeSpeed=%.1f, friction=%.2f@%.0fpx, dz=%.2f)\n",
            g_in.cstick_cfg.close_radius, g_in.cstick_cfg.free_speed,
            g_in.cstick_cfg.friction_min, g_in.cstick_cfg.friction_radius,
            g_in.cstick_cfg.deadzone);
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
    if (sub_446360() || sub_413A50() || sub_450560() || sub_46A4A0() || sub_459DA0())
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

bool nox_3ds_input_is_bottom_touch_active(void)
{
    return g_in.touch_active && g_in.bottom_touch_frame;
}

bool nox_3ds_input_is_ranged_aim_held(void)
{
    return g_in.ranged_aim_held;
}

bool nox_3ds_input_is_cstick_or_r_active(void)
{
    return g_cstick_or_r_active;
}

bool nox_3ds_input_get_aim_lock(int *out_orient, int *out_x, int *out_y)
{
    if (!g_aim_lock_active)
        return false;
    if (out_orient) *out_orient = g_aim_lock_orient;
    if (out_x)      *out_x      = g_aim_lock_x;
    if (out_y)      *out_y      = g_aim_lock_y;
    return true;
}

bool nox_3ds_input_get_spell_pin(int *out_wx, int *out_wy)
{
    if (!g_spell_pin_active)
        return false;
    if (out_wx) *out_wx = g_spell_pin_wx;
    if (out_wy) *out_wy = g_spell_pin_wy;
    return true;
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
    /* NEW: Explicitly suppress touch movement if L-trackpad aim is active */
    if (g_in.ranged_aim_held) {
        g_touch_mode = TOUCH_MODE_NONE;
        nox_ctrl_set_touch_move(0, 0, 0);
        return;
    }

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
            float nx = dx / dist;
            float ny = dy / dist;
            int orient = orientation_from_vec(nx, ny);
            int cmd = (dist >= 85.0f) ? 3 : 1; /* close = walk, outer = run */
            if (!g_cstick_or_r_active && !nox_3ds_input_is_cpad_move_active()) {
                g_aim_dir_x = nx;
                g_aim_dir_y = ny;
                g_world_aim_orient = orient;
            }
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

bool nox_3ds_consume_direct_ip_request(void)
{
    bool v = g_pending_direct_ip;
    g_pending_direct_ip = false;
    return v;
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

    /* Live A/B toggle for Native GPU Font: hold L + R, tap SELECT.
     * Swallow SELECT until released so minimap / movie skip do not fire. */
    {
        static bool s_swallow_select = false;
        if (((g_in.keys_held & (NOX_3DS_KEY_L | NOX_3DS_KEY_R)) == (NOX_3DS_KEY_L | NOX_3DS_KEY_R)) &&
            (g_in.keys_down & NOX_3DS_KEY_SELECT)) {
            nox_3ds_font_toggle();
            s_swallow_select = true;
        }
        if (s_swallow_select) {
            bool raw_sel = (g_in.keys_held & NOX_3DS_KEY_SELECT) != 0;
            g_in.keys_held &= ~NOX_3DS_KEY_SELECT;
            g_in.keys_down &= ~NOX_3DS_KEY_SELECT;
            g_in.keys_up   &= ~NOX_3DS_KEY_SELECT;
            if (!raw_sel) {
                s_swallow_select = false;
            }
        }
    }

    /* Live toggle for Bottom-Screen top-right FPS HUD: hold L + R, tap START.
     * Swallow START until released so the Pause/ESC menu or chat does not fire. */
    {
        static bool s_swallow_perf_start = false;
        if (((g_in.keys_held & (NOX_3DS_KEY_L | NOX_3DS_KEY_R)) == (NOX_3DS_KEY_L | NOX_3DS_KEY_R)) &&
            (g_in.keys_down & NOX_3DS_KEY_START)) {
            nox_3ds_perf_hud_toggle();
            s_swallow_perf_start = true;
        }
        if (s_swallow_perf_start) {
            bool raw_start = (g_in.keys_held & NOX_3DS_KEY_START) != 0;
            g_in.keys_held &= ~NOX_3DS_KEY_START;
            g_in.keys_down &= ~NOX_3DS_KEY_START;
            g_in.keys_up   &= ~NOX_3DS_KEY_START;
            if (!raw_start) {
                s_swallow_perf_start = false;
            }
        }
    }

    /* Direct IPv4 prompt in Multiplayer Server Browser: plain SELECT when
     * SelServ.wnd (+814980) is open outside gameplay. */
    {
        static bool s_swallow_direct_ip_sel = false;
        if (!(g_in.keys_held & (NOX_3DS_KEY_L | NOX_3DS_KEY_R)) &&
            (g_in.keys_down & NOX_3DS_KEY_SELECT) &&
            nox_read_u32(&byte_5D4594[814980]) != 0 &&
            !sub_43C700()) {
            g_pending_direct_ip = true;
            s_swallow_direct_ip_sel = true;
        }
        if (s_swallow_direct_ip_sel) {
            bool raw_sel = (g_in.keys_held & NOX_3DS_KEY_SELECT) != 0;
            g_in.keys_held &= ~NOX_3DS_KEY_SELECT;
            g_in.keys_down &= ~NOX_3DS_KEY_SELECT;
            g_in.keys_up   &= ~NOX_3DS_KEY_SELECT;
            if (!raw_sel) {
                s_swallow_direct_ip_sel = false;
            }
        }
    }

    /* Initiate multiplayer chat + virtual keyboard: hold L, tap START.
     * Swallow START until released so the Pause/ESC menu does not fire. */
    {
        static bool s_swallow_start = false;
        if (!(g_in.keys_held & NOX_3DS_KEY_R) &&
            (g_in.keys_held & NOX_3DS_KEY_L) &&
            (g_in.keys_down & NOX_3DS_KEY_START)) {
            g_pending_chat = true;
            s_swallow_start = true;
        }
        if (s_swallow_start) {
            bool raw_start = (g_in.keys_held & NOX_3DS_KEY_START) != 0;
            g_in.keys_held &= ~NOX_3DS_KEY_START;
            g_in.keys_down &= ~NOX_3DS_KEY_START;
            g_in.keys_up   &= ~NOX_3DS_KEY_START;
            if (!raw_start) {
                s_swallow_start = false;
            }
        }
    }

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
    if (g_wait_touch_release) {
        if (!cur_touch) {
            g_wait_touch_release = false;
        }
        cur_touch = false;
    }
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
    bool l_held = (g_in.keys_held & NOX_3DS_KEY_L) != 0;

    if (g_pending_chat) {
        g_pending_chat = false;
        uint32_t chat_win = nox_read_u32(&byte_5D4594[1064860]);
        if (chat_win != 0 && !sub_46A4A0() && !sub_446360() &&
            !sub_413A50() && !sub_450560() && !sub_459DA0()) {
            sub_46A430(0);
        }
    }

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

        int cam_ox = nox_read_s32(&byte_5D4594[811084]) - nox_read_s32(&byte_5D4594[811068]);
        int cam_oy = nox_read_s32(&byte_5D4594[811088]) - nox_read_s32(&byte_5D4594[811072]);
        int cam_dx = g_prev_cam_valid ? (cam_ox - g_prev_cam_ox) : 0;
        int cam_dy = g_prev_cam_valid ? (cam_oy - g_prev_cam_oy) : 0;
        g_prev_cam_ox    = cam_ox;
        g_prev_cam_oy    = cam_oy;
        g_prev_cam_valid = true;

        bool l_down = (l_held && !g_prev_l_held);
        bool l_up   = (!l_held && g_prev_l_held);
        g_prev_l_held = l_held;
        g_in.ranged_aim_held = l_held;

        float cnx = 0.0f, cny = 0.0f, cmag = 0.0f;
        bool cstick_active = compute_cstick_curved(&cnx, &cny, &cmag);
        g_cstick_or_r_active = (cstick_active || l_held);

        if (l_held) {
            if (g_in.touch_active) {
                /* TRACKPAD MODE: Match exact bottom touch scaled location directly */
                g_in.world_aim_x = g_in.bottom_touch_x;
                g_in.world_aim_y = g_in.bottom_touch_y;
                g_free_aim_x = (float)g_in.world_aim_x;
                g_free_aim_y = (float)g_in.world_aim_y;
                
                float dx = g_free_aim_x - (float)cx;
                float dy = g_free_aim_y - (float)cy;
                float dlen = sqrtf(dx * dx + dy * dy);
                if (dlen > 2.0f) {
                    g_aim_dir_x = dx / dlen;
                    g_aim_dir_y = dy / dlen;
                    g_world_aim_orient = orientation_from_vec(g_aim_dir_x, g_aim_dir_y);
                }
            } else {
                /* Entering L-ranged mode: start detached cursor from current close-aim point */
                if (l_down) {
                    g_free_aim_x = (float)g_in.world_aim_x;
                    g_free_aim_y = (float)g_in.world_aim_y;
                } else if (cam_dx > -120 && cam_dx < 120 && cam_dy > -120 && cam_dy < 120) {
                    /* World-anchor the L-reticle so dodging/strafing keeps it glued
                     * to its ground tile (like leaving a mouse cursor on a spot on PC). */
                    g_free_aim_x -= (float)cam_dx;
                    g_free_aim_y -= (float)cam_dy;
                }

                if (cstick_active) {
                    float fscale = compute_aim_friction_scale(cnx, cny);
                    float step   = cmag * g_in.cstick_cfg.free_speed * fscale;
                    g_free_aim_x += cnx * step;
                    g_free_aim_y += cny * step;
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

        /* Auto-flick aim-override lock: when explicit aim (L or C-Stick) is active
         * and the player presses/holds R (attack) or a Spell/Ability button
         * (A/B/X/Y/D-Down), temporarily lock orientation + exact 0xAC coordinates
         * to the reticle so attacks/spells fire toward the reticle rather than
         * the Circle Pad movement direction. */
        {
            const uint32_t spell_mask  = NOX_3DS_KEY_A | NOX_3DS_KEY_B |
                                         NOX_3DS_KEY_X | NOX_3DS_KEY_Y | NOX_3DS_KEY_DDOWN;
            const uint32_t combat_mask = NOX_3DS_KEY_R | spell_mask;
            bool r_down      = (g_in.keys_down & NOX_3DS_KEY_R) != 0;
            bool r_is_held   = (g_in.keys_held & NOX_3DS_KEY_R) != 0;
            bool spell_down  = (g_in.keys_down & spell_mask) != 0;
            bool combat_held = (g_in.keys_held & combat_mask) != 0;
            uint32_t now_ms  = SDL_GetTicks();

            if (g_cstick_or_r_active) {
                if (r_down || spell_down) {
                    uint32_t dur_ms = r_down ? 220u : 180u;
                    uint32_t target = now_ms + dur_ms;
                    if (!g_aim_lock_active || (int32_t)(target - g_aim_lock_until_ms) > 0)
                        g_aim_lock_until_ms = target;
                    g_aim_lock_orient = g_world_aim_orient;
                    g_aim_lock_x      = g_in.world_aim_x;
                    g_aim_lock_y      = g_in.world_aim_y;
                    g_aim_lock_active = true;
                } else if (combat_held) {
                    if (r_is_held) {
                        uint32_t floor_ms = now_ms + 100u;
                        if (!g_aim_lock_active || (int32_t)(floor_ms - g_aim_lock_until_ms) > 0)
                            g_aim_lock_until_ms = floor_ms;
                    }
                    g_aim_lock_orient = g_world_aim_orient;
                    g_aim_lock_x      = g_in.world_aim_x;
                    g_aim_lock_y      = g_in.world_aim_y;
                    g_aim_lock_active = true;
                } else if (g_aim_lock_active && (int32_t)(g_aim_lock_until_ms - now_ms) > 0) {
                    /* Track live reticle adjustments while finishing the post-tap window */
                    g_aim_lock_orient = g_world_aim_orient;
                    g_aim_lock_x      = g_in.world_aim_x;
                    g_aim_lock_y      = g_in.world_aim_y;
                } else {
                    g_aim_lock_active = false;
                }
            } else if (g_aim_lock_active && (int32_t)(g_aim_lock_until_ms - now_ms) > 0) {
                /* Player released L or C-Stick immediately after tapping attack/spell:
                 * preserve the latched reticle aim until the swing/shot finishes. */
            } else {
                g_aim_lock_active = false;
            }

            /* World-pinned spell cast target ("Cast Anchor"):
             * The server snapshots 0xAC only when a spell's phoneme chant
             * finishes (replicated anim == 21). Pinning 0xAC to the world spot
             * at spell-press lets you cast ground spells at your feet (or at
             * the L-reticle) and immediately dodge out of the way on the Circle
             * Pad without dragging the spell in front of your escape path. */
            if (spell_down) {
                if (g_cstick_or_r_active) {
                    nox_3ds_i2_t wpos;
                    wpos.x = g_in.world_aim_x;
                    wpos.y = g_in.world_aim_y;
                    sub_473970(&wpos, &wpos);
                    g_spell_pin_wx          = wpos.x;
                    g_spell_pin_wy          = wpos.y;
                    g_spell_pin_active      = true;
                    g_spell_pin_saw_casting = false;
                    g_spell_pin_start_ms    = now_ms;
                } else if (!nox_3ds_input_is_cpad_move_active() &&
                           !nox_3ds_input_is_touch_move_active()) {
                    nox_3ds_i2_t hero_pos;
                    sub_435670(&hero_pos);
                    /* Pin right on player's standing spot (+4px along facing to avoid
                     * degenerate 0-length vector on directional spells). */
                    g_spell_pin_wx          = hero_pos.x + (int)lrintf(g_aim_dir_x * 4.0f);
                    g_spell_pin_wy          = hero_pos.y + (int)lrintf(g_aim_dir_y * 4.0f);
                    g_spell_pin_active      = true;
                    g_spell_pin_saw_casting = false;
                    g_spell_pin_start_ms    = now_ms;
                } else {
                    g_spell_pin_active      = false;
                    g_spell_pin_saw_casting = false;
                }
            } else if (g_spell_pin_active) {
                uint32_t local_dr = nox_read_u32(&byte_5D4594[2614252]);
                bool is_casting   = (local_dr != 0 &&
                                     nox_read_u32((const uint8_t *)(uintptr_t)local_dr + 276) == 21u);
                uint32_t elapsed  = now_ms - g_spell_pin_start_ms;

                if (is_casting) {
                    g_spell_pin_saw_casting = true;
                }

                if ((g_spell_pin_saw_casting && !is_casting) ||
                    (!g_spell_pin_saw_casting && elapsed >= 220u) ||
                    elapsed >= 950u) {
                    g_spell_pin_active      = false;
                    g_spell_pin_saw_casting = false;
                } else if (l_held && (cstick_active || g_in.touch_active)) {
                    /* Deliberate mid-chant L-reticle adjustment updates the pin live */
                    nox_3ds_i2_t wpos;
                    wpos.x = g_in.world_aim_x;
                    wpos.y = g_in.world_aim_y;
                    sub_473970(&wpos, &wpos);
                    g_spell_pin_wx = wpos.x;
                    g_spell_pin_wy = wpos.y;
                }
            }
        }

        nox_ctrl_set_radial_aim(1, g_world_aim_orient, g_in.world_aim_x, g_in.world_aim_y);
    } else {
        g_prev_l_held = false;
        g_prev_cam_valid = false;
        g_cstick_or_r_active = false;
        g_aim_lock_active = false;
        g_aim_lock_until_ms = 0;
        g_spell_pin_active = false;
        g_spell_pin_saw_casting = false;
        g_touch_mode = TOUCH_MODE_NONE;
        g_in.ranged_aim_held = false;
        nox_ctrl_set_radial_aim(0, 0, 0, 0);
        nox_ctrl_set_touch_move(0, 0, 0);
    }

    /* =====================================================================
     * 2. Strict Screen Input Arbitration:
     *    - Touch Movement: Stays active independently of the mouse pointer.
     *    - Mouse Pointer & Clicks: R-button (attack) or L-button (trackpad)
     *      take priority and force the mouse to the Top Screen. Otherwise,
     *      Touch owns the mouse for UI interaction.
     * ===================================================================== */
     
    bool r_held = (g_in.keys_held & NOX_3DS_KEY_R) != 0;
    bool top_screen_priority = world_mode && (l_held || r_held || g_aim_lock_active);

    if (top_screen_priority) {
        /* TOP SCREEN OWNS THE MOUSE POINTER */
        g_in.bottom_touch_frame = false;

        /* Temporarily lift touch's hold on the mouse to allow Top Screen actions */
        release_lmb_if_owned(LMB_OWNER_TOUCH);
        if (g_touch_mode == TOUCH_MODE_CTX_MENU) {
            g_touch_mode = TOUCH_MODE_NONE;
        }

        /* We explicitly DO NOT clear g_touch_mode here. If you are running
           via the touch screen, we want that to continue seamlessly in the background. */

        nox_ctrl_inject_mouse_abs(
            g_aim_lock_active ? g_aim_lock_x : g_in.world_aim_x,
            g_aim_lock_active ? g_aim_lock_y : g_in.world_aim_y);

        if (r_held && g_lmb_owner != LMB_OWNER_WORLD_R) {
            nox_ctrl_inject_mouse_button(0, 1);
            g_lmb_owner = LMB_OWNER_WORLD_R;
        } else if (!r_held && g_lmb_owner == LMB_OWNER_WORLD_R) {
            nox_ctrl_inject_mouse_button(0, 0);
            g_lmb_owner = LMB_OWNER_NONE;
        }

    } else if (g_in.touch_active || g_in.touch_up || !world_mode) {
        /* BOTTOM SCREEN OWNS THE MOUSE POINTER */
        g_in.bottom_touch_frame = true;

        /* Release world R-click if it was active */
        release_lmb_if_owned(LMB_OWNER_WORLD_R);

        if (g_in.touch_active) {
            nox_ctrl_inject_mouse_abs(g_in.bottom_touch_x, g_in.bottom_touch_y);

            if (g_in.touch_down && world_mode &&
                is_creature_cage_hit(g_in.bottom_touch_x, g_in.bottom_touch_y)) {
                /* Drag-and-Release State 1 (Stylus Down):
                 * Fire Mouse Down + Mouse Up to open the Conjurer creature
                 * control context menu immediately, then keep LMB up for hover. */
                g_touch_mode = TOUCH_MODE_CTX_MENU;
                g_lmb_owner  = LMB_OWNER_NONE;
                nox_ctrl_inject_mouse_button(0, 1);
                nox_ctrl_inject_mouse_button(0, 0);
            } else if (g_touch_mode == TOUCH_MODE_CTX_MENU) {
                /* Drag-and-Release State 2 (Stylus Drag):
                 * Keep LMB released while updating cursor X/Y to hover options. */
                release_lmb_if_owned(LMB_OWNER_TOUCH);
            } else if (g_lmb_owner != LMB_OWNER_TOUCH) {
                nox_ctrl_inject_mouse_button(0, 1);
                g_lmb_owner = LMB_OWNER_TOUCH;
            }
        } else if (g_in.touch_up) {
            nox_ctrl_inject_mouse_abs(g_in.bottom_touch_x, g_in.bottom_touch_y);

            if (g_touch_mode == TOUCH_MODE_CTX_MENU) {
                /* Drag-and-Release State 3 (Stylus Up):
                 * Synthesize a final Mouse Down + Mouse Up at the release
                 * coordinates so the PC engine selects the hovered option. */
                void *ctx_menu = get_creature_ctx_menu();
                if (ctx_menu && sub_46AAB0(ctx_menu, g_in.bottom_touch_x, g_in.bottom_touch_y)) {
                    nox_ctrl_inject_mouse_button(0, 1);
                    nox_ctrl_inject_mouse_button(0, 0);
                }
                g_lmb_owner = LMB_OWNER_NONE;
            } else {
                release_lmb_if_owned(LMB_OWNER_TOUCH);
            }
            g_touch_mode = TOUCH_MODE_NONE;
            nox_ctrl_set_touch_move(0, 0, 0);
        } else {
            release_lmb_if_owned(LMB_OWNER_TOUCH);
            g_touch_mode = TOUCH_MODE_NONE;
            nox_ctrl_set_touch_move(0, 0, 0);
        }
    } else {
        /* IDLE / DEFAULT: TOP SCREEN OWNS THE POINTER (No touch, no L/R) */
        g_in.bottom_touch_frame = false;
        
        /* Clean up any stale touch state */
        release_lmb_if_owned(LMB_OWNER_TOUCH);
        g_touch_mode = TOUCH_MODE_NONE;
        nox_ctrl_set_touch_move(0, 0, 0);

        nox_ctrl_inject_mouse_abs(
            g_aim_lock_active ? g_aim_lock_x : g_in.world_aim_x,
            g_aim_lock_active ? g_aim_lock_y : g_in.world_aim_y);
        
        if (g_lmb_owner == LMB_OWNER_WORLD_R) {
            nox_ctrl_inject_mouse_button(0, 0);
            g_lmb_owner = LMB_OWNER_NONE;
        }
    }
}

void nox_3ds_input_process_twin_stick_and_touch(void)
{
    nox_3ds_input_process_hybrid();
}

void nox_3ds_input_reset_after_applet(void)
{
    /* Release any latched mouse buttons before resuming the Nox event loop */
    release_lmb_if_owned(LMB_OWNER_TOUCH);
    release_lmb_if_owned(LMB_OWNER_WORLD_R);
    g_touch_mode = TOUCH_MODE_NONE;
    g_prev_cam_valid = false;
    g_aim_lock_active = false;
    g_aim_lock_until_ms = 0;
    g_spell_pin_active = false;
    g_spell_pin_saw_casting = false;
    g_pending_chat = false;
    g_pending_direct_ip = false;
    nox_ctrl_set_touch_move(0, 0, 0);

    /* Drain stale HID state and ignore touch until stylus is lifted so tapping
     * OK/Cancel on swkbd never leaks a click into Nox UI underneath. */
    hidScanInput();
    g_wait_touch_release = (hidKeysHeld() & KEY_TOUCH) != 0;
    g_prev_touch         = false;
    g_in.touch_active    = false;
    g_in.touch_down      = false;
    g_in.touch_up        = false;
    g_in.keys_down       = 0;
    g_in.keys_up         = 0;
}
