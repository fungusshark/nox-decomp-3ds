/* Host unit test for port/3ds/playtest_gate.h + playtest_aim.h + playtest_missile.h (issue #36).
 *   cc -std=c99 -Wall -Wextra -Iport/3ds scripts/test_playtest_gate.c -o /tmp/t && /tmp/t
 * Also run by: python3 -I scripts/playtest_hook_client.py --selftest */
#include <stdio.h>
#include <stdlib.h>
#include "playtest_gate.h"
#include "playtest_aim.h"
#include "playtest_missile.h"

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

    if (fails) {
        printf("playtest_gate: %d failure(s)\n", fails);
        return 1;
    }
    printf("playtest_gate OK\n");
    return 0;
}
