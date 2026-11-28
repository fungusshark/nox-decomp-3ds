/* Host unit test for port/3ds/playtest_gate.h + playtest_aim.h + playtest_missile.h + playtest_events.h (issue #36).
 *   cc -std=c99 -Wall -Wextra -Iport/3ds scripts/test_playtest_gate.c -o /tmp/t && /tmp/t
 * Also run by: python3 -I scripts/playtest_hook_client.py --selftest */
#include <stdio.h>
#include <stdlib.h>
#include "playtest_gate.h"
#include "playtest_aim.h"
#include "playtest_missile.h"
#include "playtest_events.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
static int name_is(const pt_gate_t *g, const char *n)
{
    const char *cur = pt_gate_name(g);
    return n ? (cur && strcmp(cur, n) == 0) : cur == NULL;
}

/* menu frames: (is_mp, menu_shell, map_loaded, row) */
static void browse(pt_gate_t *g, bool is_mp, const char *row) { pt_gate_frame(g, is_mp, true, false, row); }
static void loading(pt_gate_t *g, const char *row) { pt_gate_frame(g, true, false, false, row); }
static void in_map(pt_gate_t *g) { pt_gate_frame(g, true, false, true, NULL); }

/* Fake spell trie for the chant decoder: node = index into k_tn. */
typedef struct { int spell; int child[9]; } fake_node_t;
static fake_node_t k_tn[16];
static uintptr_t fk_child(uintptr_t n, int p) { return (n && p >= 0 && p <= 8) ? (uintptr_t)k_tn[n].child[p] : 0; }
static int fk_spell(uintptr_t n) { return n ? k_tn[n].spell : 0; }
static void fk_build(void)
{
    memset(k_tn, 0, sizeof(k_tn));
    /* root 1. "1 2" -> spell 27 (leaf). "3" -> 50 with child "3 0" -> 52 (leaf). "5 6 7" -> 128. */
    k_tn[1].child[1] = 2; k_tn[2].child[2] = 3; k_tn[3].spell = 27;
    k_tn[1].child[3] = 4; k_tn[4].spell = 50; k_tn[4].child[0] = 5; k_tn[5].spell = 52;
    k_tn[1].child[5] = 6; k_tn[6].child[6] = 7; k_tn[7].child[7] = 8; k_tn[8].spell = 128;
}
static int snd_of(int ph, int female)
{
    for (int s = 186; s <= 201; ++s) { int f, q = pt_phoneme_from_sound(s, &f); if (q == ph && f == female) return s; }
    return -1;
}
static pt_event_t *audio(pt_ev_ring_t *r, pt_chant_t *ch, const pt_trie_t *t, int ph, int female, int pan, uint32_t fr)
{
    int snd = snd_of(ph, female);
    uint8_t m[4] = { 0xA7, (uint8_t)(int8_t)pan, (uint8_t)(snd & 0xFF), (uint8_t)(((snd >> 8) & 3) | (40 << 2)) };
    pt_msg_ctx_t cx = { fr, 0, true, true };
    return pt_ev_parse_msg(r, ch, t, m, 4, &cx);
}
static int count_type(const pt_ev_ring_t *r, int type, int *last_c)
{
    int n = 0;
    for (uint32_t s = 1; s < r->next_seq; ++s) {
        const pt_event_t *e = &r->ev[s & (PT_EV_RING - 1u)];
        if (e->type == type) { ++n; if (last_c) *last_c = e->c; }
    }
    return n;
}

int main(void)
{
    pt_gate_t g;

    /* 1. The live bug: 0xB0 while browsing (not yet MP), join "giraffepus". */
    pt_gate_reset(&g);
    pt_gate_note_b0(&g, "giraffepus", false);
    browse(&g, false, "giraffepus");
    browse(&g, false, "giraffepus");
    loading(&g, "giraffepus");
    in_map(&g);
    CHECK(name_is(&g, "giraffepus"));
    in_map(&g);
    CHECK(name_is(&g, "giraffepus"));

    /* 2. Browse several servers (0xB0 for each), join a non-allowlisted one. */
    pt_gate_reset(&g);
    pt_gate_note_b0(&g, "giraffepus", false);
    browse(&g, true, "giraffepus");
    pt_gate_note_b0(&g, "other", false);
    browse(&g, true, "other");
    pt_gate_note_b0(&g, "giraffepus", false); /* stale browse reply arrives late */
    loading(&g, "other");
    in_map(&g);
    CHECK(name_is(&g, "other"));

    /* 3. Direct-IP style join (no selected row): fail closed even with a browsed 0xB0. */
    pt_gate_reset(&g);
    pt_gate_note_b0(&g, "giraffepus", false);
    browse(&g, true, NULL);
    loading(&g, NULL);
    in_map(&g);
    CHECK(name_is(&g, NULL));
    /* ...until an in-session 0xB0 arrives. */
    pt_gate_note_b0(&g, "giraffepus", true);
    in_map(&g);
    CHECK(name_is(&g, "giraffepus"));

    /* 4. Leaving to the menu shell after a map clears; rejoining elsewhere re-captures. */
    pt_gate_frame(&g, true, true, false, "other");
    CHECK(name_is(&g, NULL));
    loading(&g, "other");
    in_map(&g);
    CHECK(name_is(&g, "other"));

    /* 5. Leaving MP clears. */
    pt_gate_frame(&g, false, false, true, NULL);
    CHECK(name_is(&g, NULL));

    /* 6. 14-char row name extended by the matching full 15-char 0xB0 name. */
    pt_gate_reset(&g);
    pt_gate_note_b0(&g, "[bots-ok]giraf!", false);
    browse(&g, true, "[bots-ok]giraf");
    loading(&g, "[bots-ok]giraf");
    in_map(&g);
    CHECK(name_is(&g, "[bots-ok]giraf!"));

    /* 7. Map rotation inside a session keeps the name even if the row is gone. */
    loading(&g, NULL);
    in_map(&g);
    CHECK(name_is(&g, "[bots-ok]giraf!"));

    /* 8. MP allow decision: tag, exact allowlist, wildcard, fail closed. */
    {
        static const char allow[2][PT_GATE_NAME_MAX + 1] = { "giraffepus", "Other Srv" };
        CHECK(pt_gate_mp_allow("giraffepus", allow, 2, false) == PT_ALLOW_LIST);
        CHECK(pt_gate_mp_allow("GIRAFFEPUS", allow, 2, false) == PT_ALLOW_LIST);
        CHECK(pt_gate_mp_allow("other srv", allow, 2, false) == PT_ALLOW_LIST);
        CHECK(pt_gate_mp_allow("giraffepus2", allow, 2, false) == 0);   /* exact, not prefix */
        CHECK(pt_gate_mp_allow("giraffe", allow, 2, false) == 0);
        CHECK(pt_gate_mp_allow("x [Bots-OK]", allow, 2, false) == PT_ALLOW_TAG);
        CHECK(pt_gate_mp_allow(NULL, allow, 2, false) == 0);            /* unknown name */
        CHECK(pt_gate_mp_allow("", allow, 2, false) == 0);
        CHECK(pt_gate_mp_allow("stranger", allow, 0, false) == 0);
        /* Wildcard: any server, even with the name unknown. */
        CHECK(pt_gate_mp_allow("stranger", allow, 0, true) == PT_ALLOW_WILDCARD);
        CHECK(pt_gate_mp_allow(NULL, allow, 0, true) == PT_ALLOW_WILDCARD);
        CHECK(pt_gate_mp_allow("giraffepus", allow, 2, true) == (PT_ALLOW_WILDCARD | PT_ALLOW_LIST));
    }

    /* World-aim override (hook AIM_* drives nox_3ds_input_get_world_aim). */
    {
        pt_aim_ovr_t o = {0};
        int x, y;
        pt_aim_resolve(&o, 320, 264, &x, &y);           /* off: human aim */
        CHECK(x == 320 && y == 264);
        pt_aim_ovr_set(&o, true, 400, 200);
        pt_aim_resolve(&o, 320, 264, &x, &y);           /* on: hook aim */
        CHECK(x == 400 && y == 200);
        pt_aim_resolve(&o, 320, 264, NULL, &y);         /* NULL-safe */
        CHECK(y == 200);
        pt_aim_ovr_set(&o, false, 400, 200);            /* timeout/STOP release */
        pt_aim_resolve(&o, 111, 222, &x, &y);
        CHECK(x == 111 && y == 222 && !o.on);
        /* world -> screen: cam (1000,2000), vp 120..519 x 120..359 */
        pt_aim_world_to_screen(1200, 2120, 1000, 2000, 120, 120, 519, 359, &x, &y);
        CHECK(x == 320 && y == 240);
        pt_aim_world_to_screen(0, 99999, 1000, 2000, 120, 120, 519, 359, &x, &y);
        CHECK(x == 120 && y == 359);                    /* clamped to viewport */
    }

    /* v5 missile tracker: velocity from the second sighting, independent of publishes. */
    {
        pt_mtrk_t t[4];
        pt_mtrk_reset(t, 4);
        pt_mtrk_note(t, 4, 77, 1000, 500, 100, 1);
        const pt_mtrk_t *m = pt_mtrk_find(t, 4, 77);
        CHECK(m && !m->vel);
        pt_mtrk_note(t, 4, 77, 1000, 500, 100, 2);      /* same frame: ignored */
        CHECK(!m->vel && m->tick == 1);
        pt_mtrk_note(t, 4, 77, 912, 500, 102, 3);       /* -44 px/frame (bolt) */
        CHECK(m->vel && m->vx16 == -44 * 16 && m->vy16 == 0);
        pt_mtrk_note(t, 4, 77, 100, 100, 103, 4);       /* jump > 80 px/frame: reused net code */
        CHECK(!m->vel && m->x == 100);
        pt_mtrk_note(t, 4, 77, 104, 102, 105, 5);
        CHECK(m->vel && m->vx16 == 32 && m->vy16 == 16);
        /* full table: reuse stale, else the oldest slot */
        pt_mtrk_note(t, 4, 1, 0, 0, 105, 5);
        pt_mtrk_note(t, 4, 2, 0, 0, 105, 6);
        pt_mtrk_note(t, 4, 3, 0, 0, 105, 7);
        pt_mtrk_note(t, 4, 4, 0, 0, 105, 8);            /* evicts 77 (tick 5) */
        CHECK(!pt_mtrk_find(t, 4, 77) && pt_mtrk_find(t, 4, 4));
        pt_mtrk_note(t, 4, 5, 0, 0, 200, 60);           /* all stale: first slot reused */
        CHECK(pt_mtrk_find(t, 4, 5) && pt_mtrk_find(t, 4, 5)->netcode == 5);
        CHECK(!pt_mtrk_find(t, 4, 0));
    }

    /* closest approach / incoming */
    {
        uint16_t miss, tca;
        /* 200 px east, flying west at 10 px/frame straight at us */
        uint8_t f = pt_mis_approach(200, 0, -160, 0, true, &miss, &tca);
        CHECK(f == (PT_MISF_NEAR | PT_MISF_APPROACHING | PT_MISF_INCOMING) && miss == 0 && tca == 20);
        /* passes 100 px north of us: approaching, not incoming */
        f = pt_mis_approach(200, -100, -160, 0, true, &miss, &tca);
        CHECK(f == (PT_MISF_NEAR | PT_MISF_APPROACHING) && miss == 100 && tca == 20);
        /* flying away */
        f = pt_mis_approach(200, 0, 160, 0, true, &miss, &tca);
        CHECK(f == PT_MISF_NEAR && miss == 0xFFFF && tca == 0xFFFF);
        /* too slow: closest approach after 90 frames */
        f = pt_mis_approach(300, 0, -16, 0, true, &miss, &tca);
        CHECK(f == (PT_MISF_NEAR | PT_MISF_APPROACHING) && tca == 300);
        /* no velocity, far */
        f = pt_mis_approach(400, 0, 0, 0, false, &miss, &tca);
        CHECK(f == 0 && miss == 0xFFFF);
    }

    /* export priority: near missile < player < monster < far missile < prop */
    {
        uint32_t near_m = pt_obj_sort_key(0x180001u, 250 * 250);
        uint32_t pl = pt_obj_sort_key(0x280004u, 10);
        uint32_t mon = pt_obj_sort_key(0x2u, 10);
        uint32_t far_m = pt_obj_sort_key(0x180001u, 330 * 330);
        uint32_t prop = pt_obj_sort_key(0x180000u, 0);
        CHECK(near_m < pl && pl < mon && mon < far_m && far_m < prop);
        CHECK(pt_obj_sort_key(4u, 5) < pt_obj_sort_key(4u, 6));
        CHECK(pt_obj_sort_key(0x180001u, 0x7FFFFFFFLL) >> 24 == 3u);
    }

    /* spell-set stepping (ZL/ZR, 5 sets that wrap) */
    {
        bool next;
        CHECK(pt_spellset_steps(0, 0, &next) == 0);
        CHECK(pt_spellset_steps(0, 1, &next) == 1 && next);
        CHECK(pt_spellset_steps(0, 4, &next) == 1 && !next);
        CHECK(pt_spellset_steps(4, 0, &next) == 1 && next);
        CHECK(pt_spellset_steps(1, 3, &next) == 2 && next);
        CHECK(pt_spellset_steps(3, 1, &next) == 2 && !next);
        CHECK(pt_spellset_steps(0xFF, 1, &next) == 0);
        CHECK(pt_spellset_steps(1, 5, &next) == 0);
    }

    /* v6 event ring + message parser */
    {
        static pt_ev_ring_t r;
        pt_chant_t ch[PT_CHANT_STREAMS];
        memset(ch, 0, sizeof(ch));
        pt_trie_t none = { 0, fk_child, fk_spell };
        pt_ev_reset(&r);
        pt_msg_ctx_t cx = { 500, 100, true, true };
        uint8_t dmg[5] = { 0x42, 0x11, 0x80, 0xF6, 0xFF };          /* net 0x8011 -> 0x11, delta -10 */
        pt_event_t *e = pt_ev_parse_msg(&r, ch, &none, dmg, 5, &cx);
        CHECK(e && e->type == PT_EV_DMG && e->a == 0x11 && e->f == -10 && e->seq == 1 && e->frame == 500);
        CHECK(!pt_ev_parse_msg(&r, ch, &none, dmg, 4, &cx));          /* truncated */
        uint8_t kill[11] = { 0xA9, 14, 7, 0, 9, 0, 5, 0, 2, 0, 2 };  /* killer 7, assist 9, victim 5 */
        e = pt_ev_parse_msg(&r, ch, &none, kill, 11, &cx);
        CHECK(e && e->type == PT_EV_KILL && e->a == 5 && e->c == 7 && e->d == 9 && e->e == 2 && e->b == 2);
        kill[1] = 13;
        CHECK(!pt_ev_parse_msg(&r, ch, &none, kill, 11, &cx));        /* other inform subtypes ignored */
        uint8_t ds[7] = { 0x9E, 11, 3, 7, 0, 9, 0 };                  /* Lightning start, 7 -> 9 */
        e = pt_ev_parse_msg(&r, ch, &none, ds, 7, &cx);
        CHECK(e && e->type == PT_EV_DSPELL && e->c == 24 && e->a == 7 && e->d == 9 && e->f == 1 && e->e == 3);
        ds[1] = 13;                                                   /* Greater Heal: target first */
        e = pt_ev_parse_msg(&r, ch, &none, ds, 7, &cx);
        CHECK(e && e->c == 35 && e->a == 9 && e->d == 7);
        ds[1] = 4;
        e = pt_ev_parse_msg(&r, ch, &none, ds, 7, &cx);
        CHECK(e && e->c == 24 && e->f == 0);
        uint8_t ob[4] = { 0xB3, 0x30, 0x00, 55 };
        e = pt_ev_parse_msg(&r, ch, &none, ob, 4, &cx);
        CHECK(e && e->type == PT_EV_OBELISK && e->a == 0x30 && e->b == 55);
        uint8_t map[41] = { 0x2B };
        map[37] = 101;                                                /* > map_gate 100 */
        e = pt_ev_parse_msg(&r, ch, &none, map, 41, &cx);
        CHECK(e && e->type == PT_EV_MAP);
        map[37] = 100;
        CHECK(!pt_ev_parse_msg(&r, ch, &none, map, 41, &cx));         /* stale 0x2B */
        uint8_t win[8] = { 0x58, 5, 0, 1, 200, 0, 0, 0 };
        e = pt_ev_parse_msg(&r, ch, &none, win, 8, &cx);
        CHECK(e && e->type == PT_EV_GAME_OVER && e->b == 3 && e->a == 5 && e->c == 1);
        /* gate closed: nothing about other objects, map/game over still recorded */
        pt_msg_ctx_t gx = { 501, 100, true, false };
        CHECK(!pt_ev_parse_msg(&r, ch, &none, dmg, 5, &gx));
        map[37] = 102;
        CHECK(pt_ev_parse_msg(&r, ch, &none, map, 41, &gx) != NULL);
        /* sliding window: newest N, oldest first */
        pt_event_t w[4];
        uint32_t n = pt_ev_newest(&r, w, 4);
        CHECK(n == 4 && w[3].seq == r.next_seq - 1 && w[0].seq == r.next_seq - 4);
        for (int i = 0; i < 300; ++i)
            pt_ev_parse_msg(&r, ch, &none, dmg, 5, &cx);
        static pt_event_t big[PT_EV_RING + 8];
        n = pt_ev_newest(&r, big, PT_EV_RING + 8);
        CHECK(n == PT_EV_RING && big[n - 1].seq == r.next_seq - 1 && big[0].seq == r.next_seq - PT_EV_RING);
        int fem;
        CHECK(pt_phoneme_from_sound(193, &fem) == 0 && fem == 0);
        CHECK(pt_phoneme_from_sound(194, &fem) == 1 && fem == 1);
        CHECK(pt_phoneme_from_sound(185, &fem) == -1 && pt_phoneme_from_sound(202, &fem) == -1);
    }

    /* v6 chant decoding (fake trie) */
    {
        static pt_ev_ring_t r;
        pt_chant_t ch[PT_CHANT_STREAMS];
        fk_build();
        pt_trie_t t = { 1, fk_child, fk_spell };
        int c = 0;
        pt_ev_reset(&r); memset(ch, 0, sizeof(ch));
        audio(&r, ch, &t, 1, 0, 10, 100);
        CHECK(count_type(&r, PT_EV_CAST, NULL) == 0 && count_type(&r, PT_EV_CHANT, NULL) == 1);
        pt_event_t *e = audio(&r, ch, &t, 2, 0, 12, 103);             /* leaf: cast now */
        CHECK(e && e->type == PT_EV_CAST && e->c == 27 && e->e == 2 && e->f == 12 && e->d == 80);
        /* prefix spell: "3" waits, then times out as 50 */
        audio(&r, ch, &t, 3, 1, -20, 200);
        pt_chant_tick(ch, &t, &r, 205);
        CHECK(count_type(&r, PT_EV_CAST, &c) == 1);
        pt_chant_tick(ch, &t, &r, 200 + PT_CHANT_DONE_F + 1);
        CHECK(count_type(&r, PT_EV_CAST, &c) == 2 && c == 50);
        /* "3 0" -> 52 (phoneme 4 has no sound) */
        audio(&r, ch, &t, 3, 1, -20, 300);
        audio(&r, ch, &t, 0, 1, -20, 303);
        CHECK(count_type(&r, PT_EV_CAST, &c) == 3 && c == 52);
        /* two interleaved casters (different gender): both decode */
        audio(&r, ch, &t, 5, 0, 0, 400);
        audio(&r, ch, &t, 1, 1, 0, 401);
        audio(&r, ch, &t, 6, 0, 0, 403);
        audio(&r, ch, &t, 2, 1, 0, 404);
        audio(&r, ch, &t, 7, 0, 0, 406);
        CHECK(count_type(&r, PT_EV_CAST, NULL) == 5);
        /* same gender, opposite sides: separate streams by pan */
        audio(&r, ch, &t, 1, 0, -100, 500);
        audio(&r, ch, &t, 5, 0, 100, 501);
        audio(&r, ch, &t, 2, 0, -100, 503);
        audio(&r, ch, &t, 6, 0, 100, 504);
        audio(&r, ch, &t, 7, 0, 100, 507);
        CHECK(count_type(&r, PT_EV_CAST, NULL) == 7);
        /* junk phoneme not starting any spell: chant event only */
        audio(&r, ch, &t, 8, 0, 0, 600);
        pt_chant_tick(ch, &t, &r, 700);
        CHECK(count_type(&r, PT_EV_CAST, NULL) == 7);
        /* obelisk table */
        pt_obl_t ob[PT_OBL_SLOTS];
        memset(ob, 0, sizeof(ob));
        pt_obl_note(ob, 9, 10); pt_obl_note(ob, 9, 80); pt_obl_note(ob, 11, 0);
        CHECK(ob[0].used && ob[0].net == 9 && ob[0].charge == 80 && ob[1].net == 11);
    }

    if (fails) {
        printf("playtest_gate: %d failure(s)\n", fails);
        return 1;
    }
    printf("playtest_gate OK\n");
    return 0;
}
