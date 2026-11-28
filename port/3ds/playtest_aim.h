#ifndef NOX_PLAYTEST_AIM_H
#define NOX_PLAYTEST_AIM_H

/*
 * Playtest hook world-aim override (#36). Pure C (no engine/3DS deps) so
 * scripts/test_playtest_gate.c can unit-test it on the host.
 *
 * The 3DS client hovers/picks objects from input_3ds's world-aim point
 * (nox_3ds_input_get_world_aim, used by sub_476FA0 hover, the world cursor
 * and the radial aim), not from the mouse. A hook AIM_WORLD/AIM_DIR command
 * therefore installs an override that nox_3ds_input_get_world_aim returns
 * while it is on. The hook clears it on command timeout, STOP, gate close
 * and shutdown. Only compiled with NOX_PLAYTEST_HOOK.
 */

#include <stdbool.h>

typedef struct {
    bool on;
    int  x, y; /* Top viewport coords inside 640x480 (same space as world_aim_x/y) */
} pt_aim_ovr_t;

static inline void pt_aim_ovr_set(pt_aim_ovr_t *o, bool on, int x, int y)
{
    o->on = on;
    o->x = on ? x : 0;
    o->y = on ? y : 0;
}

/* Effective world-aim point: the override when on, else the human aim. */
static inline void pt_aim_resolve(const pt_aim_ovr_t *o, int human_x, int human_y,
                                  int *out_x, int *out_y)
{
    int x = o->on ? o->x : human_x;
    int y = o->on ? o->y : human_y;
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
}

static inline int pt_aim_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* World -> screen (screen = world - cam + vp), clamped to the Top viewport. */
static inline void pt_aim_world_to_screen(int wx, int wy, int cam_x, int cam_y,
                                          int vp_l, int vp_t, int vp_r, int vp_b,
                                          int *sx, int *sy)
{
    *sx = pt_aim_clampi(wx - cam_x + vp_l, vp_l, vp_r);
    *sy = pt_aim_clampi(wy - cam_y + vp_t, vp_t, vp_b);
}

#endif /* NOX_PLAYTEST_AIM_H */
