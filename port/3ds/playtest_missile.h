#ifndef NOX_PLAYTEST_MISSILE_H
#define NOX_PLAYTEST_MISSILE_H

/*
 * Playtest hook missile tracking, export priority and spell-set stepping
 * (#36, state v5). Pure C (no engine/3DS deps) so scripts/test_playtest_gate.c
 * can unit-test it on the host.
 *
 * Why a tracker: the hook publishes every `every` event pumps (bots use 2,
 * about 4 game frames apart), and v4 computed velocity only from the previous
 * *publish*. A crossbow bolt (about 44 px/frame) crosses half the 640x384 view
 * in 7 frames, so it was usually exported once and never had a velocity. The
 * tracker samples visible missiles on every pump, so a missile has a velocity
 * from its first export if any earlier pump saw it.
 *
 * Positions here are client drawable positions (+12/+16), already filtered by
 * the same rules as the object export (camera rect, sub_4984B0 visibility,
 * invisibility). The drawable's own previous-position fields (+32/+36) are not
 * used: the client's smoothing pass (GAME2.c:9860) rewrites them and resets
 * them on moves over 10 px, so fast missiles read 0 there.
 */

#include <stdbool.h>
#include <stdint.h>

#define PT_MTRK_SLOTS          32u
#define PT_MTRK_STALE_TICKS    30u   /* pumps without a sighting before a slot is reused */
#define PT_MTRK_MAX_STEP       80    /* px/frame; a bigger jump = reused net code, restart */
#define PT_MIS_NEAR_PX         320   /* "near" missile: exported before everything else */
#define PT_MIS_INCOMING_MISS   64    /* closest approach within this many px ... */
#define PT_MIS_INCOMING_TCA_F  90    /* ... and within this many game frames = incoming */

/* v5 missile flags (nox_pt_mis_t.mis_flags). */
#define PT_MISF_INCOMING       0x01u /* approaching and the closest approach is <= 64 px within 90 frames */
#define PT_MISF_NEAR           0x02u /* within PT_MIS_NEAR_PX of the local player */
#define PT_MISF_TRACKED        0x04u /* velocity from the per-pump tracker (valid from the first export) */
#define PT_MISF_APPROACHING    0x08u /* distance to the local player is shrinking */

typedef struct {
    uint32_t netcode;      /* 0 = free */
    int32_t  x, y;
    uint32_t frame;        /* game frame of (x, y) */
    uint32_t tick;         /* hook pump of the last sighting */
    int32_t  vx16, vy16;   /* px per 16 frames (Q4 px/frame) */
    bool     vel;
} pt_mtrk_t;

static inline void pt_mtrk_reset(pt_mtrk_t *t, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) {
        t[i].netcode = 0;
        t[i].vel = false;
    }
}

static inline int32_t pt_mtrk_abs(int32_t v) { return v < 0 ? -v : v; }

/* floor(sqrt(v)), no libm (bitwise integer square root). */
static inline uint32_t pt_isqrt(uint64_t v)
{
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* Record a sighting. Several sightings in the same game frame keep the first. */
static inline void pt_mtrk_note(pt_mtrk_t *t, uint32_t n, uint32_t netcode, int32_t x, int32_t y,
                                uint32_t frame, uint32_t tick)
{
    if (!netcode)
        return;
    pt_mtrk_t *slot = 0, *free_s = 0, *oldest = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (t[i].netcode == netcode) { slot = &t[i]; break; }
        if (!t[i].netcode || tick - t[i].tick > PT_MTRK_STALE_TICKS) {
            if (!free_s) free_s = &t[i];
        } else if (!oldest || (int32_t)(t[i].tick - oldest->tick) < 0) {
            oldest = &t[i];
        }
    }
    if (slot) {
        uint32_t df = frame - slot->frame;
        if (df == 0)
            return;
        int32_t dx = x - slot->x, dy = y - slot->y;
        if (df < 64u && tick - slot->tick <= PT_MTRK_STALE_TICKS &&
            pt_mtrk_abs(dx) <= PT_MTRK_MAX_STEP * (int32_t)df &&
            pt_mtrk_abs(dy) <= PT_MTRK_MAX_STEP * (int32_t)df) {
            slot->vx16 = dx * 16 / (int32_t)df;
            slot->vy16 = dy * 16 / (int32_t)df;
            slot->vel = true;
        } else {
            slot->vel = false; /* gap too long or a reused net code */
        }
    } else {
        slot = free_s ? free_s : oldest;
        if (!slot)
            return;
        slot->netcode = netcode;
        slot->vel = false;
    }
    slot->x = x;
    slot->y = y;
    slot->frame = frame;
    slot->tick = tick;
}

static inline const pt_mtrk_t *pt_mtrk_find(const pt_mtrk_t *t, uint32_t n, uint32_t netcode)
{
    for (uint32_t i = 0; i < n; ++i)
        if (t[i].netcode == netcode && netcode)
            return &t[i];
    return 0;
}

/* Closest approach of a missile at (rx, ry) relative to the local player,
 * moving at (vx16, vy16) px per 16 frames (the player assumed still).
 * Returns PT_MISF_* bits; *miss_px / *tca_f are 0xFFFF when unknown or
 * moving away. */
static inline uint8_t pt_mis_approach(int32_t rx, int32_t ry, int32_t vx16, int32_t vy16, bool vel,
                                      uint16_t *miss_px, uint16_t *tca_f)
{
    uint8_t f = 0;
    double d2 = (double)rx * rx + (double)ry * ry;
    if (d2 <= (double)PT_MIS_NEAR_PX * PT_MIS_NEAR_PX)
        f |= PT_MISF_NEAR;
    *miss_px = 0xFFFF;
    *tca_f = 0xFFFF;
    if (!vel || (vx16 == 0 && vy16 == 0))
        return f;
    double vx = vx16 / 16.0, vy = vy16 / 16.0;
    double dot = rx * vx + ry * vy;       /* < 0: closing */
    if (dot >= 0)
        return f;
    f |= PT_MISF_APPROACHING;
    double v2 = vx * vx + vy * vy;
    double t = -dot / v2;                 /* frames to the closest approach */
    double cx = rx + vx * t, cy = ry + vy * t;
    double m2 = cx * cx + cy * cy;
    double miss = m2 >= 4.0e9 ? 65534.0 : (double)pt_isqrt((uint64_t)(m2 + 0.5));
    *miss_px = miss >= 65534.0 ? 0xFFFE : (uint16_t)miss;
    *tca_f = t >= 65534.0 ? 0xFFFE : (uint16_t)(t + 0.5);
    if (m2 <= (double)PT_MIS_INCOMING_MISS * PT_MIS_INCOMING_MISS && t <= PT_MIS_INCOMING_TCA_F)
        f |= PT_MISF_INCOMING;
    return f;
}

/* Export priority (lower first): near missiles, players, monsters, other
 * missiles, everything else; nearest first inside a tier. d2 = squared px. */
static inline uint32_t pt_obj_sort_key(uint32_t class_flags, int64_t d2)
{
    uint32_t tier;
    if ((class_flags & 1u) && d2 <= (int64_t)PT_MIS_NEAR_PX * PT_MIS_NEAR_PX)
        tier = 0;
    else if (class_flags & 4u)
        tier = 1;
    else if (class_flags & 2u)
        tier = 2;
    else if (class_flags & 1u)
        tier = 3;
    else
        tier = 4;
    uint32_t d = d2 > 0xFFFFFF ? 0xFFFFFFu : (d2 < 0 ? 0u : (uint32_t)d2);
    return (tier << 24) | d;
}

/* Spell-set stepping the way ZL / ZR do it (W = previous, E = next set, 5 sets
 * that wrap). Returns the number of key taps (0..2); *next is true for E. */
#define PT_SPELL_SETS 5
static inline int pt_spellset_steps(int cur, int want, bool *next)
{
    *next = true;
    if (cur < 0 || cur >= PT_SPELL_SETS || want < 0 || want >= PT_SPELL_SETS || cur == want)
        return 0;
    int fwd = (want - cur + PT_SPELL_SETS) % PT_SPELL_SETS;
    int back = (cur - want + PT_SPELL_SETS) % PT_SPELL_SETS;
    if (fwd <= back)
        return fwd;
    *next = false;
    return back;
}

#endif /* NOX_PLAYTEST_MISSILE_H */
