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

/* Engine state queries (src/GAME1.c, src/GAME2.c, src/GAME_data.c) */
extern unsigned char byte_5D4594[];
extern unsigned char byte_587000[];
extern int sub_42CF50(const char *a1);   /* parse & prepend keybinding line */
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

/* 3DS physical button <-> Westwood JOYSTICK_BUTTON_1..13 (0x20000..0x2000C) table */
typedef struct {
    uint32_t       hid_bit;
    const wchar_t *label;
} nox_3ds_joy_map_t;

static const nox_3ds_joy_map_t k_3ds_joy_map[NOX_3DS_JOY_BTN_COUNT] = {
    { NOX_3DS_KEY_A,      L"[A]"       }, /* 0x20000 JOYSTICK_BUTTON_1  */
    { NOX_3DS_KEY_B,      L"[B]"       }, /* 0x20001 JOYSTICK_BUTTON_2  */
    { NOX_3DS_KEY_X,      L"[X]"       }, /* 0x20002 JOYSTICK_BUTTON_3  */
    { NOX_3DS_KEY_Y,      L"[Y]"       }, /* 0x20003 JOYSTICK_BUTTON_4  */
    { NOX_3DS_KEY_L,      L"[L]"       }, /* 0x20004 JOYSTICK_BUTTON_5  */
    { NOX_3DS_KEY_R,      L"[R]"       }, /* 0x20005 JOYSTICK_BUTTON_6  */
    { NOX_3DS_KEY_ZL,     L"[ZL]"      }, /* 0x20006 JOYSTICK_BUTTON_7  */
    { NOX_3DS_KEY_ZR,     L"[ZR]"      }, /* 0x20007 JOYSTICK_BUTTON_8  */
    { NOX_3DS_KEY_DUP,    L"[D-Up]"    }, /* 0x20008 JOYSTICK_BUTTON_9  */
    { NOX_3DS_KEY_DDOWN,  L"[D-Down]"  }, /* 0x20009 JOYSTICK_BUTTON_10 */
    { NOX_3DS_KEY_DLEFT,  L"[D-Left]"  }, /* 0x2000A JOYSTICK_BUTTON_11 */
    { NOX_3DS_KEY_DRIGHT, L"[D-Right]" }, /* 0x2000B JOYSTICK_BUTTON_12 */
    { NOX_3DS_KEY_SELECT, L"[Select]"  }, /* 0x2000C JOYSTICK_BUTTON_13 */
};

static float s_base_cstick_sens = 1.0f;
static float s_base_free_speed  = NOX_3DS_FREE_AIM_SPEED;

/* 1-frame <RangedAim> + <InvokeSlot1..5> self-cast state machine on 0x20000..0x2000C */
static bool     s_inv_shift_held       = false;
static bool     s_inv_shift_ready      = false;
static uint32_t s_inv_pending_slots    = 0;
static uint32_t s_inv_early_up_slots   = 0;
static uint32_t s_inv_active_slots     = 0;
static uint32_t s_prev_slot_held_mask  = 0;

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
    int l = 0, t = 48, r = 639, b = 431;
    nox_3ds_get_top_viewport(&l, &t, &r, &b, NULL, NULL, NULL, NULL);

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
    s_base_cstick_sens           = read_env_float("NOX_3DS_CSTICK_SENS",     1.0f,  0.2f,  4.0f);
    s_base_free_speed            = read_env_float("NOX_3DS_FREE_AIM_SPEED", NOX_3DS_FREE_AIM_SPEED, 2.0f, 20.0f);
    g_in.cstick_cfg.deadzone     = read_env_float("NOX_3DS_CSTICK_DEADZONE", 0.14f, 0.02f, 0.85f);
    g_in.cstick_cfg.curve_exp    = read_env_float("NOX_3DS_CSTICK_CURVE",    1.75f, 1.0f,  4.0f);
    g_in.cstick_cfg.sensitivity  = s_base_cstick_sens;
    g_in.cstick_cfg.close_radius = read_env_float("NOX_3DS_AIM_RADIUS", NOX_3DS_AIM_CLOSE_RADIUS, 10.0f, 36.0f);
    g_in.cstick_cfg.free_speed   = s_base_free_speed;

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

void nox_3ds_input_apply_sensitivity(float sens)
{
    if (sens < 0.1f) sens = 0.1f;
    if (sens > 10.0f) sens = 10.0f;
    /* Smooth sqrt curve maps Options slider [0..50..100] (0.1..1.0..10.0) -> ~0.32x..1.0x..3.16x */
    float scale = sqrtf(sens);
    float c_sens = s_base_cstick_sens * scale;
    float f_spd  = s_base_free_speed * scale;
    if (c_sens < 0.25f) c_sens = 0.25f;
    if (c_sens > 4.0f)  c_sens = 4.0f;
    if (f_spd  < 3.0f)  f_spd  = 3.0f;
    if (f_spd  > 36.0f) f_spd  = 36.0f;
    g_in.cstick_cfg.sensitivity = c_sens;
    g_in.cstick_cfg.free_speed  = f_spd;
}

const void *nox_3ds_get_joy_button_label(int joy_idx)
{
    if (joy_idx < 0 || joy_idx >= NOX_3DS_JOY_BTN_COUNT)
        return NULL;
    return (const void *)k_3ds_joy_map[joy_idx].label;
}

static uint32_t get_bound_joy_bits_for_action(int action_id, bool *out_has_lmb)
{
    uint32_t joy_bits = 0;
    bool has_lmb = false;
    uint32_t *node = (uint32_t *)(uintptr_t)nox_read_u32(&byte_5D4594[754056]);
    while (node) {
        int num_inputs  = (int)node[8];
        int num_actions = (int)node[17];
        bool match = false;
        for (int a = 0; a < num_actions && a < 8; ++a) {
            if ((int)node[9 + a] == action_id) {
                match = true;
                break;
            }
        }
        if (match && num_inputs == 1) {
            uint32_t code = node[0];
            if (code == 0x10000u) {
                has_lmb = true;
            } else if ((code & 0xFFFF0000u) == NOX_3DS_JOY_BTN_BASE) {
                uint32_t idx = code & 0xFFFFu;
                if (idx < NOX_3DS_JOY_BTN_COUNT)
                    joy_bits |= (1u << idx);
            }
        }
        node = (uint32_t *)(uintptr_t)node[19];
    }
    if (out_has_lmb)
        *out_has_lmb = has_lmb;
    return joy_bits;
}

uint32_t nox_3ds_get_bound_hid_mask(int action_id)
{
    uint32_t joy_bits = get_bound_joy_bits_for_action(action_id, NULL);
    uint32_t hid_mask = 0;
    for (int i = 0; i < NOX_3DS_JOY_BTN_COUNT; ++i) {
        if (joy_bits & (1u << i))
            hid_mask |= k_3ds_joy_map[i].hid_bit;
    }
    return hid_mask;
}

int nox_3ds_poll_ui_capture_token(void)
{
    uint32_t down = g_in.keys_down;
    if (!down)
        return 0;
    for (int i = 0; i < NOX_3DS_JOY_BTN_COUNT; ++i) {
        if (down & k_3ds_joy_map[i].hid_bit)
            return (int)(NOX_3DS_JOY_BTN_BASE + (uint32_t)i);
    }
    return 0;
}

static void remove_3ds_joy_bindings_for_action(int action_id)
{
    uint32_t *node = (uint32_t *)(uintptr_t)nox_read_u32(&byte_5D4594[754056]);
    while (node) {
        uint32_t *next = (uint32_t *)(uintptr_t)node[19];
        int num_inputs  = (int)node[8];
        int num_actions = (int)node[17];
        bool has_joy = false;
        bool has_act = false;
        for (int i = 0; i < num_inputs && i < 8; ++i) {
            if ((node[i] & 0xFFFF0000u) == NOX_3DS_JOY_BTN_BASE)
                has_joy = true;
        }
        for (int a = 0; a < num_actions && a < 8; ++a) {
            if ((int)node[9 + a] == action_id)
                has_act = true;
        }
        if (has_joy && (action_id < 0 || has_act)) {
            uint32_t *prev = (uint32_t *)(uintptr_t)node[18];
            if (prev)
                prev[19] = (uint32_t)(uintptr_t)next;
            else
                nox_write_u32(&byte_5D4594[754056], (uint32_t)(uintptr_t)next);
            if (next)
                next[18] = (uint32_t)(uintptr_t)prev;
            free(node);
        }
        node = next;
    }
}

void nox_3ds_ensure_default_button_bindings(bool force_defaults)
{
    if (!force_defaults) {
        uint32_t *node = (uint32_t *)(uintptr_t)nox_read_u32(&byte_5D4594[754056]);
        while (node) {
            int n = (int)node[8];
            for (int i = 0; i < n && i < 8; ++i) {
                if ((node[i] & 0xFFFF0000u) == NOX_3DS_JOY_BTN_BASE)
                    return; /* User already has saved 3DS button bindings in nox.cfg */
            }
            node = (uint32_t *)(uintptr_t)node[19];
        }
    } else {
        remove_3ds_joy_bindings_for_action(-1);
    }

    /* Seed ergonomic New 3DS defaults into Key 1 (preserving PC mouse/key fallbacks in Key 2) */
    static const char *const k_defaults[] = {
        "JOYSTICK_BUTTON_13 = ToggleMap",         /* [Select]  */
        "JOYSTICK_BUTTON_8 = NextSpellSet",       /* [ZR]      */
        "JOYSTICK_BUTTON_7 = PreviousSpellSet",   /* [ZL]      */
        "JOYSTICK_BUTTON_12 = QuickMana",         /* [D-Right] */
        "JOYSTICK_BUTTON_11 = QuickHealth",       /* [D-Left]  */
        "JOYSTICK_BUTTON_9 = Jump",               /* [D-Up]    */
        "JOYSTICK_BUTTON_10 = InvokeSlot5",       /* [D-Down]  */
        "JOYSTICK_BUTTON_4 = InvokeSlot4",        /* [Y]       */
        "JOYSTICK_BUTTON_3 = InvokeSlot3",        /* [X]       */
        "JOYSTICK_BUTTON_2 = InvokeSlot2",        /* [B]       */
        "JOYSTICK_BUTTON_1 = InvokeSlot1",        /* [A]       */
        "JOYSTICK_BUTTON_5 = InvertSpellTarget",  /* [L]       */
        "JOYSTICK_BUTTON_6 = Action",             /* [R]       */
    };
    for (size_t i = 0; i < sizeof(k_defaults) / sizeof(k_defaults[0]); ++i) {
        sub_42CF50(k_defaults[i]);
    }
}

uint32_t nox_3ds_update_joystick_buttons(void)
{
    uint32_t raw_joy = 0;
    for (int i = 0; i < NOX_3DS_JOY_BTN_COUNT; ++i) {
        if (g_in.keys_held & k_3ds_joy_map[i].hid_bit)
            raw_joy |= (1u << i);
    }

    const bool world_mode = nox_3ds_input_is_world_mode();
    if (!world_mode) {
        s_inv_shift_held      = false;
        s_inv_shift_ready     = false;
        s_inv_pending_slots   = 0;
        s_inv_early_up_slots  = 0;
        s_inv_active_slots    = 0;
        s_prev_slot_held_mask = 0;
        return raw_joy;
    }

    bool attack_has_lmb = false;
    uint32_t attack_joy_bits = get_bound_joy_bits_for_action(1, &attack_has_lmb);
    uint32_t aim_joy_bits    = get_bound_joy_bits_for_action(49, NULL);
    uint32_t slot_joy_bits[5];
    uint32_t cur_slot_held_mask = 0;
    for (int s = 0; s < 5; ++s) {
        slot_joy_bits[s] = get_bound_joy_bits_for_action(28 + s, NULL);
        if (raw_joy & slot_joy_bits[s])
            cur_slot_held_mask |= (1u << s);
    }

    uint32_t out_joy = raw_joy;

    /* Primary Attack ([R] by default) already injects LMB at world_aim_x/y in world_mode;
     * avoid duplicate Action firing when MOUSE_BUTTON_LEFT is also bound to Action. */
    if (attack_has_lmb && attack_joy_bits)
        out_joy &= ~attack_joy_bits;

    /* Overloaded <RangedAim> ([L]) + <InvokeSlot1..5> -> 1-frame InvertSpellTarget + Spell self-cast!
     * Holding <RangedAim> alone detaches C-Stick ranged free-aim without flipping ally heals. */
    if (aim_joy_bits) {
        bool aim_phys_held = (raw_joy & aim_joy_bits) != 0;
        out_joy &= ~aim_joy_bits;

        if (s_inv_shift_held && !s_inv_shift_ready)
            s_inv_shift_ready = true;

        /* Release finished inverted spell casts */
        for (int s = 0; s < 5; ++s) {
            uint32_t sbit = (1u << s);
            if ((s_inv_active_slots & sbit) &&
                ((s_inv_early_up_slots & sbit) || !(cur_slot_held_mask & sbit))) {
                s_inv_active_slots   &= ~sbit;
                s_inv_early_up_slots &= ~sbit;
            }
        }

        uint32_t fired_now = 0;
        if (s_inv_shift_ready && s_inv_pending_slots) {
            for (int s = 0; s < 5; ++s) {
                uint32_t sbit = (1u << s);
                if (s_inv_pending_slots & sbit) {
                    s_inv_pending_slots &= ~sbit;
                    s_inv_active_slots  |= sbit;
                    fired_now           |= sbit;
                }
            }
        }

        for (int s = 0; s < 5; ++s) {
            uint32_t sbit = (1u << s);
            bool down_edge = ((cur_slot_held_mask & sbit) != 0) && ((s_prev_slot_held_mask & sbit) == 0);
            bool up_edge   = ((cur_slot_held_mask & sbit) == 0) && ((s_prev_slot_held_mask & sbit) != 0);

            if (down_edge && aim_phys_held) {
                if (!s_inv_shift_held) {
                    s_inv_shift_held  = true;
                    s_inv_shift_ready = false;
                }
                if (s_inv_shift_ready) {
                    s_inv_active_slots   |= sbit;
                    s_inv_early_up_slots &= ~sbit;
                    fired_now            |= sbit;
                } else {
                    s_inv_pending_slots  |= sbit;
                    s_inv_early_up_slots &= ~sbit;
                }
            } else if (up_edge) {
                if ((s_inv_pending_slots & sbit) || (fired_now & sbit)) {
                    s_inv_early_up_slots |= sbit;
                } else if (s_inv_active_slots & sbit) {
                    s_inv_active_slots   &= ~sbit;
                    s_inv_early_up_slots &= ~sbit;
                }
            }

            if (s_inv_pending_slots & sbit) {
                /* Hold spell back for 1 frame while InvertSpellTarget flips the spell bar */
                out_joy &= ~slot_joy_bits[s];
            } else if (s_inv_active_slots & sbit) {
                out_joy |= slot_joy_bits[s];
            }
        }

        if (s_inv_shift_held && s_inv_pending_slots == 0 && s_inv_active_slots == 0) {
            s_inv_shift_held  = false;
            s_inv_shift_ready = false;
        }
        if (s_inv_shift_held) {
            out_joy |= aim_joy_bits;
        }
    }

    s_prev_slot_held_mask = cur_slot_held_mask;
    return out_joy;
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
    uint32_t aim_hid_mask = nox_3ds_get_bound_hid_mask(49); /* InvertSpellTarget / Ranged Aim */
    if (aim_hid_mask == 0)
        aim_hid_mask = NOX_3DS_KEY_L;
    uint32_t atk_hid_mask = nox_3ds_get_bound_hid_mask(1);  /* Action / Primary Attack */
    if (atk_hid_mask == 0)
        atk_hid_mask = NOX_3DS_KEY_R;
    bool l_held = (g_in.keys_held & aim_hid_mask) != 0;

    if (g_pending_chat) {
        g_pending_chat = false;
        uint32_t chat_win = nox_read_u32(&byte_5D4594[1064860]);
        if (chat_win != 0 && !sub_46A4A0() && !sub_446360() &&
            !sub_413A50() && !sub_450560() && !sub_459DA0()) {
            sub_46A430(0);
        }
    }

    /* =====================================================================
     * 1. Top-Screen World Aiming: C-Stick + <RangedAim> ([L] by default)
     *    - Default (L not held): Tight fixed radius (~15px physical) right next
     *      to the character so adjacent doors, items, and melee targets hover.
     *    - Hold L: Detaches into free-aim cursor across the full active Top
     *      Screen viewport (400x240, 640x384, or 800x480) for ranged spells.
     * ===================================================================== */
    if (world_mode) {
        int vp_l, vp_t, vp_r, vp_b, cx, cy;
        get_top_viewport_bounds(&vp_l, &vp_t, &vp_r, &vp_b, &cx, &cy);
        float vp_scale = (float)(vp_r - vp_l + 1) / (float)NOX_3DS_VP_W;

        bool l_down = (l_held && !g_prev_l_held);
        bool l_up   = (!l_held && g_prev_l_held);
        g_prev_l_held = l_held;
        g_in.ranged_aim_held = l_held;

        float cnx = 0.0f, cny = 0.0f, cmag = 0.0f;
        bool cstick_active = compute_cstick_curved(&cnx, &cny, &cmag);
        g_cstick_or_r_active = (cstick_active || l_held);

        if (l_held) {
            if (g_in.touch_active) {
                /* TRACKPAD MODE: Scale 320x240 bottom touch proportionally across active Top viewport */
                g_in.world_aim_x = vp_l + ((int)g_in.touch_x * (vp_r - vp_l)) / (NOX_3DS_BOT_W - 1);
                g_in.world_aim_y = vp_t + ((int)g_in.touch_y * (vp_b - vp_t)) / (NOX_3DS_BOT_H - 1);
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
                }

                if (cstick_active) {
                    float eff_speed = g_in.cstick_cfg.free_speed * vp_scale;
                    g_free_aim_x += cnx * cmag * eff_speed;
                    g_free_aim_y += cny * cmag * eff_speed;
                }

                /* Clamp strictly to the active Top Screen viewport bounds */
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
                /* Direct C-Stick aim on tight fixed radius (~15px physical) */
                g_aim_dir_x = cnx;
                g_aim_dir_y = cny;
                g_world_aim_orient = orientation_from_vec(g_aim_dir_x, g_aim_dir_y);
            } else {
                /* When walking with Circle Pad and not touching C-Stick, smoothly
                 * keep the close-aim probe directly ahead of the hero so
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

            float r = g_in.cstick_cfg.close_radius * vp_scale;
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
     *    - Touch Movement: Stays active independently of the mouse pointer.
     *    - Mouse Pointer & Clicks: Primary Attack ([R] or remapped button) or
     *      Ranged Aim ([L] or remapped button) take priority and force the
     *      mouse to the Top Screen. Otherwise, Touch owns the mouse for UI.
     * ===================================================================== */
     
    bool r_held = (g_in.keys_held & atk_hid_mask) != 0;
    bool top_screen_priority = world_mode && (l_held || r_held);

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

        nox_ctrl_inject_mouse_abs(g_in.world_aim_x, g_in.world_aim_y);

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

        nox_ctrl_inject_mouse_abs(g_in.world_aim_x, g_in.world_aim_y);
        
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
