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

/* ---- MP allow decision (pure) ------------------------------------------ */
#define PT_GATE_BOTS_OK_TAG  "[bots-ok]"
enum {
    PT_ALLOW_TAG      = 1u, /* name contains "[bots-ok]" */
    PT_ALLOW_LIST     = 2u, /* name equals a cfg mp_server_allow entry */
    PT_ALLOW_WILDCARD = 4u  /* cfg mp_server_allow=* (dev-only, any server) */
};

static inline char pt_gate_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static inline size_t pt_gate_len(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        ++n;
    return n;
}

static inline bool pt_gate_ci_equal(const char *a, const char *b, size_t max)
{
    size_t la = pt_gate_len(a, max), lb = pt_gate_len(b, max);
    if (la != lb)
        return false;
    for (size_t i = 0; i < la; ++i)
        if (pt_gate_lc(a[i]) != pt_gate_lc(b[i]))
            return false;
    return true;
}

static inline bool pt_gate_ci_contains(const char *s, size_t max, const char *needle)
{
    size_t sl = pt_gate_len(s, max), nl = strlen(needle);
    if (nl == 0 || sl < nl)
        return false;
    for (size_t i = 0; i + nl <= sl; ++i) {
        size_t j = 0;
        while (j < nl && pt_gate_lc(s[i + j]) == pt_gate_lc(needle[j]))
            ++j;
        if (j == nl)
            return true;
    }
    return false;
}

/* Returns PT_ALLOW_* bits; 0 = MP commands/export not allowed. name may be
 * NULL (unknown, fail closed) -- only the wildcard allows then. */
static inline unsigned pt_gate_mp_allow(const char *name, const char (*allow)[PT_GATE_NAME_MAX + 1],
                                        unsigned n_allow, bool wildcard)
{
    unsigned bits = wildcard ? PT_ALLOW_WILDCARD : 0u;
    if (name && name[0]) {
        if (pt_gate_ci_contains(name, PT_GATE_NAME_MAX, PT_GATE_BOTS_OK_TAG))
            bits |= PT_ALLOW_TAG;
        for (unsigned i = 0; i < n_allow; ++i)
            if (pt_gate_ci_equal(name, allow[i], PT_GATE_NAME_MAX)) {
                bits |= PT_ALLOW_LIST;
                break;
            }
    }
    return bits;
}

#endif /* NOX_PLAYTEST_GATE_H */
