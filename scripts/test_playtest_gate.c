/* Host unit test for port/3ds/playtest_gate.h (issue #36).
 *   cc -std=c99 -Wall -Wextra -Iport/3ds scripts/test_playtest_gate.c -o /tmp/t && /tmp/t
 * Also run by: python3 -I scripts/playtest_hook_client.py --selftest */
#include <stdio.h>
#include <stdlib.h>
#include "playtest_gate.h"

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

    if (fails) {
        printf("playtest_gate: %d failure(s)\n", fails);
        return 1;
    }
    printf("playtest_gate OK\n");
    return 0;
}
