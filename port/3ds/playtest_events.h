#ifndef NOX_PLAYTEST_EVENTS_H
#define NOX_PLAYTEST_EVENTS_H

/*
 * Playtest hook v6 event ring (#36). Pure C (no engine/3DS deps) so
 * scripts/test_playtest_gate.c can unit-test the message parser and the chant
 * decoder on the host.
 *
 * Every event comes from a server message the CLIENT receives and handles
 * (the client opcode switch in src/GAME2.c, sub_48D660-style dispatcher
 * around GAME2.c:50160). The hook sees each message once, before the engine
 * handler runs (nox_pt_note_msg), and copies the fields the client is about
 * to use. It never reads server objects. See docs/PLAYTEST_HOOK.md, "v6".
 *
 * Opcode = MSG_* index + 0x27 (name table at byte_587000[238648]):
 *   0x42 MSG_REPORT_HEALTH_DELTA   u16 net, i16 delta (<0)    -> floating damage digits (sub_49A650/sub_49A6A0)
 *   0xA9 MSG_INFORM sub 14         kill feed record           -> sub_495210 (GAME2.c:54316), "%s %s" line
 *   0xE8 MSG_PLAYER_DIED           u16 net
 *   0xE9 MSG_PLAYER_RESPAWN        u16 net, u32, u8, u8
 *   0xA6/0xA7 MSG_AUDIO_(PLAYER_)EVENT i8 pan, u16 sound|vol  -> phoneme sounds 186..201 = other players' chants
 *   0x9E MSG_FX_DURATION_SPELL     u8 sub, u8 level, u16 a, u16 b (rays: lightning, drain mana, heal, charm, plasma)
 *   0x7E MSG_FX_SUMMON             u16 x, u16 y, u16 id, u16 type, u8 dir, u16 frames
 *   0x7F MSG_FX_SUMMON_CANCEL      u16 id
 *   0xB3 MSG_OBELISK_CHARGE        u16 net, u8 charge (0..100)
 *   0x2B MSG_USE_MAP               map change (accepted when s32 +37 > [1200804])
 *   0x56..0x59 MSG_REPORT_*_WINNER game over banner (same acceptance test)
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    PT_EV_DMG = 1,           /* a target, f delta (negative: damage = -f) */
    PT_EV_KILL = 2,          /* a victim, c killer player, d assist player, e code, b kind */
    PT_EV_DIED = 3,          /* a player */
    PT_EV_RESPAWN = 4,       /* a player, b flags byte (+8), c byte (+7) */
    PT_EV_CHANT = 5,         /* b phoneme 0..8, c female, d volume, f pan */
    PT_EV_CAST = 6,          /* c spell id (decoded chant), b female, d volume, e phonemes, f pan; a = 0 (caster unknown) */
    PT_EV_DSPELL = 7,        /* a caster, d target, c spell id, b raw sub, e level, f 1 start / 0 stop */
    PT_EV_SUMMON = 8,        /* a summon id, c x, d y, e creature thing type, b dir, f frames */
    PT_EV_SUMMON_CANCEL = 9, /* a summon id */
    PT_EV_OBELISK = 10,      /* a obelisk, b charge 0..100 */
    PT_EV_MAP = 11,          /* map change accepted (name in the session block) */
    PT_EV_GAME_OVER = 12     /* b 1 flagball, 2 flag, 3 deathmatch, 4 team DM; a winner id (+1), c byte +3 */
};

/* Wire entry (20 B). Natural alignment, so packed is layout-only. */
typedef struct __attribute__((packed)) pt_event {
    uint32_t seq;      /* 1, 2, ...: gaps on the host = events lost (ring overflow or dropped packets) */
    uint32_t frame;    /* client game frame (byte_5D4594[2598000]) when the message arrived */
    uint8_t  type;     /* PT_EV_* */
    uint8_t  b;
    uint16_t a;
    uint16_t c;
    uint16_t d;
    uint16_t e;
    int16_t  f;
} pt_event_t;
_Static_assert(sizeof(pt_event_t) == 20, "pt_event_t wire size");

#define PT_EV_RING 256u   /* power of two */

typedef struct {
    pt_event_t ev[PT_EV_RING];
    uint32_t   next_seq;  /* seq of the next event; events so far = next_seq - 1 */
} pt_ev_ring_t;

static inline void pt_ev_reset(pt_ev_ring_t *r)
{
    memset(r, 0, sizeof(*r));
    r->next_seq = 1;
}

static inline pt_event_t *pt_ev_push(pt_ev_ring_t *r, uint8_t type, uint32_t frame)
{
    pt_event_t *e = &r->ev[r->next_seq & (PT_EV_RING - 1u)];
    memset(e, 0, sizeof(*e));
    e->seq = r->next_seq++;
    if (!r->next_seq)
        r->next_seq = 1;
    e->frame = frame;
    e->type = type;
    return e;
}

/* The newest min(max, events, ring) events, oldest first. Each packet carries
 * this sliding window, so an event is repeated until newer ones push it out
 * and the host dedups by seq. */
static inline uint32_t pt_ev_newest(const pt_ev_ring_t *r, pt_event_t *dst, uint32_t max)
{
    uint32_t total = r->next_seq - 1u;
    uint32_t n = total < max ? total : max;
    if (n > PT_EV_RING)
        n = PT_EV_RING;
    uint32_t first = r->next_seq - n;
    for (uint32_t i = 0; i < n; ++i)
        dst[i] = r->ev[(first + i) & (PT_EV_RING - 1u)];
    return n;
}

static inline uint16_t pt_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline int32_t  pt_rds32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

/* ---- chant decoding --------------------------------------------------
 * The server plays each caster's spell phonemes to every other player
 * (sub_4FC960 -> sub_501960(sound, caster, 2, listener), GAME4.c). The sound
 * id comes from sub_4FE1C0: male 186..193, female 194..201 (by the caster's
 * playerInfo+2252). The client gets only {sound, volume, pan} (0xA6/0xA7), not
 * the caster. The phoneme order spells a unique path in the client's own
 * spell trie (root *(byte_587000[66120]), node +0 spell id, +4+4*p child for
 * phoneme p 0..8: sub_424820/sub_424830, GAME1.c:32563), the same trie the
 * local cast UI walks (sub_49BBC0). */

/* sound id -> phoneme 0..8 (-1 = not a phoneme sound); see sub_4FE1C0. */
static inline int pt_phoneme_from_sound(int snd, int *female)
{
    static const int8_t k_ph[8] = { 1, 2, 5, 8, 7, 6, 3, 0 }; /* 186+i (male) / 194+i (female) */
    if (snd >= 186 && snd <= 193) { *female = 0; return k_ph[snd - 186]; }
    if (snd >= 194 && snd <= 201) { *female = 1; return k_ph[snd - 194]; }
    return -1;
}

typedef struct {
    uintptr_t root;                              /* 0 = no trie: chants are exported, casts are not decoded */
    uintptr_t (*child)(uintptr_t node, int p);   /* 0 = none */
    int       (*spell)(uintptr_t node);          /* spell id at node, 0 = none */
} pt_trie_t;

#define PT_CHANT_STREAMS  4
#define PT_CHANT_JOIN_F   30   /* frames: max gap between phonemes of one chant */
#define PT_CHANT_PAN_TOL  48   /* pan difference still treated as the same caster */
#define PT_CHANT_DONE_F   10   /* a stream at a spell node, silent this long, is a cast */

typedef struct {
    bool      on;
    uint8_t   female, vol, n;
    int8_t    pan;
    uint32_t  last;
    uintptr_t node;
} pt_chant_t;

static inline bool pt_trie_leaf(const pt_trie_t *t, uintptr_t node)
{
    for (int p = 0; p <= 8; ++p)
        if (t->child(node, p))
            return false;
    return true;
}

static inline void pt_chant_emit(pt_ev_ring_t *r, const pt_trie_t *t, pt_chant_t *s, uint32_t frame)
{
    int sp = t->spell(s->node);
    if (sp > 0) {
        pt_event_t *e = pt_ev_push(r, PT_EV_CAST, frame);
        e->c = (uint16_t)sp;
        e->b = s->female;
        e->d = s->vol;
        e->e = s->n;
        e->f = s->pan;
    }
    s->on = false;
}

/* Finalize chants that went quiet (call once per pump). */
static inline void pt_chant_tick(pt_chant_t *st, const pt_trie_t *t, pt_ev_ring_t *r, uint32_t frame)
{
    if (!t->root)
        return;
    for (int i = 0; i < PT_CHANT_STREAMS; ++i) {
        pt_chant_t *s = &st[i];
        if (!s->on)
            continue;
        uint32_t gap = frame - s->last;
        if (t->spell(s->node) > 0 ? gap > PT_CHANT_DONE_F : gap > PT_CHANT_JOIN_F)
            pt_chant_emit(r, t, s, frame);
    }
}

static inline void pt_chant_note(pt_chant_t *st, const pt_trie_t *t, pt_ev_ring_t *r,
                                 int ph, int female, int pan, int vol, uint32_t frame)
{
    pt_event_t *e = pt_ev_push(r, PT_EV_CHANT, frame);
    e->b = (uint8_t)ph;
    e->c = (uint16_t)female;
    e->d = (uint16_t)vol;
    e->f = (int16_t)pan;
    if (!t->root)
        return;
    pt_chant_tick(st, t, r, frame);
    /* Same gender, recent, similar pan, and the phoneme continues its path. */
    pt_chant_t *best = 0, *free_s = 0, *oldest = 0;
    for (int i = 0; i < PT_CHANT_STREAMS; ++i) {
        pt_chant_t *s = &st[i];
        if (!s->on) { if (!free_s) free_s = s; continue; }
        if (!oldest || (int32_t)(s->last - oldest->last) < 0) oldest = s;
        int dp = pan - s->pan;
        if (s->female != female || frame - s->last > PT_CHANT_JOIN_F ||
            dp > PT_CHANT_PAN_TOL || dp < -PT_CHANT_PAN_TOL || !t->child(s->node, ph))
            continue;
        if (!best || (int32_t)(s->last - best->last) > 0) best = s;
    }
    if (!best) {
        uintptr_t c0 = t->child(t->root, ph);
        if (!c0)
            return; /* not the start of any spell */
        best = free_s;
        if (!best) { /* evict the stalest chant (emit it if it already spells something) */
            best = oldest;
            pt_chant_emit(r, t, best, frame);
        }
        best->on = true;
        best->female = (uint8_t)female;
        best->n = 0;
        best->node = t->root;
    }
    best->node = t->child(best->node, ph);
    best->n++;
    best->last = frame;
    best->pan = (int8_t)pan;
    best->vol = (uint8_t)vol;
    if (t->spell(best->node) > 0 && pt_trie_leaf(t, best->node))
        pt_chant_emit(r, t, best, frame); /* no longer spell shares this prefix: done now */
}

/* 0x9E MSG_FX_DURATION_SPELL sub -> spell id (sub_4FEF90 start / sub_4FF130 stop,
 * GAME4.c:7200-7345). 14 / 7 = Harpoon (Warrior ability, no spell id). */
static inline int pt_dspell_spell(int sub, bool *start)
{
    static const uint8_t k_stop[8]  = { 0, 59, 9, 7, 24, 22, 35, 0 };   /* sub 1..7 */
    static const uint8_t k_start[7] = { 59, 9, 7, 24, 22, 35, 0 };      /* sub 8..14 */
    *start = sub >= 8;
    if (sub >= 1 && sub <= 7) return k_stop[sub];
    if (sub >= 8 && sub <= 14) return k_start[sub - 8];
    return -1;
}

typedef struct {
    uint32_t frame;      /* client game frame */
    int32_t  map_gate;   /* byte_5D4594[1200804]: frame of the last accepted 0x2B */
    bool     map_loaded; /* sub_43C700() */
    bool     rec_others; /* record events about other objects (MP gate open) */
} pt_msg_ctx_t;

/* Parse one server message at p (avail bytes left in the frame). Returns the
 * event pushed for it (the last one, for chants), or NULL. */
static inline pt_event_t *pt_ev_parse_msg(pt_ev_ring_t *r, pt_chant_t *ch, const pt_trie_t *t,
                                          const uint8_t *p, uint32_t avail, const pt_msg_ctx_t *cx)
{
    if (!avail)
        return 0;
    pt_event_t *e = 0;
    uint8_t op = p[0];
    bool ml = cx->map_loaded, oth = cx->rec_others;
    switch (op) {
    case 0x42: /* MSG_REPORT_HEALTH_DELTA (sent only for delta < 0, sub_4D8760) */
        if (avail >= 5 && ml && oth) {
            e = pt_ev_push(r, PT_EV_DMG, cx->frame);
            e->a = pt_rd16(p + 1) & 0x7FFF;
            e->f = (int16_t)pt_rd16(p + 3);
        }
        break;
    case 0xA9: /* MSG_INFORM, sub 14 = kill record (sub_54D2B0 -> sub_4DA180(14), GAME5.c:6209) */
        if (avail >= 11 && p[1] == 14 && oth) {
            e = pt_ev_push(r, PT_EV_KILL, cx->frame);
            e->c = pt_rd16(p + 2);
            e->d = pt_rd16(p + 4);
            e->a = pt_rd16(p + 6);
            e->e = pt_rd16(p + 8);
            e->b = p[10];
        }
        break;
    case 0xE8: /* MSG_PLAYER_DIED */
        if (avail >= 3 && ml && oth) {
            e = pt_ev_push(r, PT_EV_DIED, cx->frame);
            e->a = pt_rd16(p + 1) & 0x7FFF;
        }
        break;
    case 0xE9: /* MSG_PLAYER_RESPAWN */
        if (avail >= 9 && ml && oth) {
            e = pt_ev_push(r, PT_EV_RESPAWN, cx->frame);
            e->a = pt_rd16(p + 1) & 0x7FFF;
            e->c = p[7];
            e->b = p[8];
        }
        break;
    case 0xA6: /* MSG_AUDIO_EVENT / MSG_AUDIO_PLAYER_EVENT (GAME2.c:52108) */
    case 0xA7:
        if (avail >= 4 && ml && oth) {
            uint16_t w = pt_rd16(p + 2);
            int female, ph = pt_phoneme_from_sound(w & 0x3FF, &female);
            if (ph >= 0) {
                pt_chant_note(ch, t, r, ph, female, (int8_t)p[1], (w >> 9) & 0x7E, cx->frame);
                e = &r->ev[(r->next_seq - 1u) & (PT_EV_RING - 1u)];
            }
        }
        break;
    case 0x9E: /* MSG_FX_DURATION_SPELL */
        if (avail >= 7 && ml && oth) {
            bool start;
            int sp = pt_dspell_spell(p[1], &start);
            if (sp >= 0) {
                e = pt_ev_push(r, PT_EV_DSPELL, cx->frame);
                uint16_t x = pt_rd16(p + 3) & 0x7FFF, y = pt_rd16(p + 5) & 0x7FFF;
                /* Greater Heal (13 start / 6 stop) sends target first. */
                bool heal = p[1] == 13 || p[1] == 6;
                e->a = heal ? y : x;
                e->d = heal ? x : y;
                e->c = (uint16_t)sp;
                e->b = p[1];
                e->e = p[2];
                e->f = start ? 1 : 0;
            }
        }
        break;
    case 0x7E: /* MSG_FX_SUMMON (sub_5236F0, GAME4.c:38687) */
        if (avail >= 12 && ml && oth) {
            e = pt_ev_push(r, PT_EV_SUMMON, cx->frame);
            e->c = pt_rd16(p + 1);
            e->d = pt_rd16(p + 3);
            e->a = pt_rd16(p + 5);
            e->e = pt_rd16(p + 7);
            e->b = p[9];
            e->f = (int16_t)pt_rd16(p + 10);
        }
        break;
    case 0x7F: /* MSG_FX_SUMMON_CANCEL */
        if (avail >= 3 && ml && oth) {
            e = pt_ev_push(r, PT_EV_SUMMON_CANCEL, cx->frame);
            e->a = pt_rd16(p + 1);
        }
        break;
    case 0xB3: /* MSG_OBELISK_CHARGE (GAME2.c:52397): light 16*c/10, anim frame 8*c/50 */
        if (avail >= 4 && ml && oth) {
            e = pt_ev_push(r, PT_EV_OBELISK, cx->frame);
            e->a = pt_rd16(p + 1) & 0x7FFF;
            e->b = p[3];
        }
        break;
    case 0x2B: /* MSG_USE_MAP, same acceptance test as GAME2.c:50235 */
        if (avail >= 41 && pt_rds32(p + 37) > cx->map_gate)
            e = pt_ev_push(r, PT_EV_MAP, cx->frame);
        break;
    case 0x56: case 0x57: case 0x58: case 0x59: /* winner reports (GAME2.c:50871-51124) */
        if (avail >= 8 && ml && pt_rds32(p + 4) > cx->map_gate) {
            e = pt_ev_push(r, PT_EV_GAME_OVER, cx->frame);
            e->b = (uint8_t)(op - 0x55);
            e->a = pt_rd16(p + 1);
            e->c = p[3];
        }
        break;
    default:
        break;
    }
    return e;
}

/* Last obelisk charge per net code (MSG_OBELISK_CHARGE). Reset on map change. */
#define PT_OBL_SLOTS 16u
typedef struct { uint16_t net; uint8_t charge; uint8_t used; } pt_obl_t;

static inline void pt_obl_note(pt_obl_t *t, uint16_t net, uint8_t charge)
{
    pt_obl_t *free_s = 0;
    for (uint32_t i = 0; i < PT_OBL_SLOTS; ++i) {
        if (t[i].used && t[i].net == net) { t[i].charge = charge; return; }
        if (!t[i].used && !free_s) free_s = &t[i];
    }
    if (free_s) { free_s->used = 1; free_s->net = net; free_s->charge = charge; }
}

#endif /* NOX_PLAYTEST_EVENTS_H */
