#ifndef NOX_PLAYTEST_GATE_H
#define NOX_PLAYTEST_GATE_H

/*
 * Server-name snapshot state machine for the playtest hook MP gate (#36).
 * Pure C (no engine/3DS deps) so scripts/test_playtest_gate.c can unit-test
 * it on the host.
 *
 * Problem: opcode 0xB0 (server name) arrives while browsing the server list,
 * before the session counts as MP, and is not resent after joining. Several
 * servers may be browsed before joining one.
 *
 * Rules (fail closed):
 *   - While no map is loaded, capture the name of the server-list row that is
 *     currently selected as the join target (row_name, NULL if none, e.g.
 *     Direct-IP). That is the server sub_438A90 joins.
 *   - On the first map-loaded frame in MP, promote the captured row name to
 *     the session name. If the row name is 14 chars (the list clamps names to
 *     14) and the last 0xB0 name extends it, use the full 0xB0 name instead.
 *     No captured row -> no name -> gate stays closed.
 *   - A 0xB0 received during an MP session (map loaded) is authoritative.
 *   - 0xB0 names seen while browsing never open the gate by themselves.
 *   - Clear the session name when returning to the menu shell after a map,
 *     or when leaving MP.
 */

#include <stdbool.h>
#include <string.h>

#define PT_GATE_NAME_MAX   15
#define PT_GATE_ROW_MAX    14

typedef struct {
    char name[PT_GATE_NAME_MAX + 1];    /* session server name */
    bool valid;
    char cand[PT_GATE_NAME_MAX + 1];    /* selected join-target row name */
    bool cand_valid;
    char pending[PT_GATE_NAME_MAX + 1]; /* last 0xB0 name seen outside a session */
    bool pending_valid;
    bool was_map_loaded;
} pt_gate_t;

static inline void pt_gate_copy(char *dst, const char *src, size_t max)
{
    size_t n = 0;
    while (n < max && src[n])
        ++n;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static inline void pt_gate_reset(pt_gate_t *g)
{
    memset(g, 0, sizeof(*g));
}

/* Opcode 0xB0 on a client. in_session = is_mp && map_loaded. */
static inline void pt_gate_note_b0(pt_gate_t *g, const char *name, bool in_session)
{
    if (!name)
        return;
    if (in_session) {
        pt_gate_copy(g->name, name, PT_GATE_NAME_MAX);
        g->valid = true;
    } else {
        pt_gate_copy(g->pending, name, PT_GATE_NAME_MAX);
        g->pending_valid = true;
    }
}

/* Once per frame. row_name: selected server-list row name, or NULL. */
static inline void pt_gate_frame(pt_gate_t *g, bool is_mp, bool menu_shell, bool map_loaded,
                                 const char *row_name)
{
    if ((menu_shell && g->was_map_loaded) || !is_mp)
        g->valid = false;

    if (!map_loaded) {
        if (row_name && row_name[0]) {
            pt_gate_copy(g->cand, row_name, PT_GATE_ROW_MAX);
            g->cand_valid = true;
        } else {
            g->cand_valid = false;
        }
    } else if (!g->was_map_loaded && is_mp && !g->valid) {
        if (g->cand_valid) {
            size_t cl = strlen(g->cand);
            if (cl == PT_GATE_ROW_MAX && g->pending_valid &&
                strncmp(g->pending, g->cand, cl) == 0)
                pt_gate_copy(g->name, g->pending, PT_GATE_NAME_MAX);
            else
                pt_gate_copy(g->name, g->cand, PT_GATE_NAME_MAX);
            g->valid = true;
        }
        g->pending_valid = false;
    }
    g->was_map_loaded = map_loaded;
}

static inline const char *pt_gate_name(const pt_gate_t *g)
{
    return g->valid ? g->name : NULL;
}

#endif /* NOX_PLAYTEST_GATE_H */
