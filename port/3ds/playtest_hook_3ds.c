/*
 * Gated game-state hook for automated playtesting (GitHub issue #36).
 *
 * Built only with -DNOX_PLAYTEST_HOOK=ON. Inert at runtime unless
 * sdmc:/3ds/nox/playtest.cfg exists. See docs/PLAYTEST_HOOK.md.
 *
 * Rules this file must keep (reviewers: please grep):
 *   - Reads ONLY client-side state: the client drawable list at
 *     byte_5D4594[1046572] and client globals. It never calls server-side
 *     accessors (sub_4DA7xx, sub_4ECCB0, sub_518EE0, the sub_418C80 server
 *     branch), even on the host, where the server list is in-process.
 *   - Never writes game state. Commands go through the same injectors the
 *     3DS pad path uses (src/input.c nox_ctrl_set_* / nox_ctrl_inject_*).
 *   - No per-frame allocation, no blocking socket calls, all engine memory
 *     reads via memcpy-based nox_read_* (ARM11 alignment safety).
 *
 * Does NOT include src/proto.h / windows.h so libctru BSD socket symbols are
 * used directly (same convention as net_sockets_3ds.c).
 */
#include "platform_3ds.h"
#include "playtest_hook_3ds.h"
#include "playtest_gate.h"
#include "playtest_aim.h"
#include "playtest_missile.h"
#include "playtest_events.h"

#include <3ds.h>
#include <SDL2/SDL.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Engine (client-side) symbols ------------------------------------- */
extern unsigned char byte_5D4594[];
extern unsigned char byte_587000[];   /* static data; [133480] = spell-bar struct* (GAME1.c:4592) */
extern int   sub_43C700(void);          /* map loaded */
extern int   sub_40A5C0(int a1);        /* game flags test; 1 = hosting server */
extern char *sub_40A4C0(void);          /* host's server name (byte_5D4594[1324]) */
extern char *sub_437250(void);          /* gameplay viewport rect [l,t,r,b] */
extern int   sub_4984B0(int a1);        /* client draw-visibility test (view polygon/LOS), read-only */
extern int   sub_46ACC0(int a1);        /* GUI window hidden check: 1 = hidden */
/* Client drawable spatial-grid walker (GAME2.c:58737): calls cb(drawable, arg)
 * for every drawable in 128 px buckets overlapping rect {l,t,r,b}. Read-only. */
extern void  sub_49AB00(void *rect4, void (*cb)(int, int), int arg);
extern int   sub_40A220(void);          /* game timer on (byte_587000[4660]) */
extern int   sub_40A230(void);          /* timer remaining ms: deadline [3468] - sub_416BB0() */

extern void nox_ctrl_set_thumbstick_move(int active, int orientation, int move_cmd, double mag);
extern void nox_ctrl_set_radial_aim(int active, int orientation, int screen_x, int screen_y);
extern void nox_ctrl_inject_mouse_abs(int x, int y);
extern void nox_ctrl_inject_mouse_button(int button, int down);
extern void nox_ctrl_inject_key_scancode(int sdl_scancode, int down);

/* ---- byte_5D4594 offsets (see docs/PLAYTEST_HOOK.md for evidence) ------ */
#define OFF_MENU_SHELL        815132u   /* main menu shell active (input_3ds.c:246) */
#define OFF_LOCAL_NETCODE     2616328u  /* GAME2.c:50255 opcode 0x2C */
#define OFF_LOCAL_PLAYERINFO  2618908u  /* GAME2.c:50261 */
#define OFF_CLIENT_FRAME      2598000u  /* GAME2.c:50174 */
#define OFF_DRAWABLE_LIST     1046572u  /* sub_45A060 / sub_45A6F0, GAME2.c:9801 */
#define OFF_DRAW_GRID         1301784u  /* int** grid[x][y] of bucket heads, chain +400 */
#define OFF_DRAW_GRID_N       1301788u  /* grid dimension (buckets per axis) */
#define OFF_SEL_SERVER_ROW     814624u   /* selected server-list row* (sub_43B300) */
#define OFF_SEL_SERVER_VALID  815056u   /* row join target in use (sub_43B300/sub_43B320) */
#define ROW_SERVER_IP         12u       /* char[] dotted IP (inet_addr in sub_43B300) */
#define ROW_SERVER_NAME       120u      /* char[14], strncpy(...,0xE) in sub_4375F0 */
#define OFF_PLAYERINFO_TABLE  371780u   /* 32 x 4828 B client player table (sub_417040) */
#define PLAYERINFO_STRIDE     4828u
#define PLAYERINFO_SLOTS      32u
#define PI_NETCODE            2060u     /* sub_417040 key */
#define PI_ACTIVE             2092u     /* sub_417040 requires nonzero */
#define PI_NAME               4704u     /* wchar_t[] (-fshort-wchar UTF-16), GAME2.c:19547 */
#define PI_NAME_MAX_WCHARS    31u
#define OFF_PLAYER_DR_LIST    1046600u  /* client player-drawable list, next +416 (sub_459ED0) */
#define OFF_NPC_DIALOG_WND    1123524u  /* Dialog.wnd root (sub_479... GAME2.c:34289) */
#define OFF_HP_CUR            1093040u  /* sub_470C80, GAME2.c:27447 */
#define OFF_HP_MAX            1093044u
#define OFF_MP_CUR            1093060u  /* sub_470CE0, GAME2.c:27483 */
#define OFF_MP_MAX            1093064u
#define OFF_CAM_X             811084u   /* input.c translator world = cam + screen - vp */
#define OFF_CAM_Y             811088u
#define OFF_ABILITY_BASE      1047764u  /* record a (1..5) at base + 24*a, sub_461090 */
#define OFF_ABILITY_DELAY     599160u   /* sub_4252D0(id) = [52*id + 599160] */
#define OFF587_SPELLBAR       133480u   /* byte_587000: spell-bar struct* (= &byte_5D4594[1049220]) */
#define SPELLBAR_CUR_SET      200u      /* u8 active spell set 0..4: sub_45E180 (GAME2.c), set by sub_45E110 */
/* v6 */
#define OFF_MAP_NAME          2649740u  /* sub_409B40(): map name without path/extension (sub_409D70) */
#define OFF_GAME_FLAGS        3600u     /* sub_40A5B0() game flags; sub_40A5C0(f) tests it */
#define OFF_MAP_GATE          1200804u  /* frame of the last accepted MSG_USE_MAP (GAME2.c:50235) */
#define OFF_SETTINGS0         371380u   /* sub_416590(0): +54 u16 kill limit, +56 u8 time limit (0xAF) */
#define OFF_FRIEND_LIST       1203864u  /* client friend list head {u32 netcode, next*} (sub_4959F0/sub_495A80) */
#define OFF587_SPELL_TRIE     66120u    /* byte_587000: spell phoneme trie root* (sub_424820) */
#define PI_SCORE              2136u     /* i32 lessons/kills (0x4E) */
#define PI_DEATHS             2140u     /* i32 deaths (0x4E) */
#define PT_MAX_FRIEND_WALK    128
#define GAMEFLAG_GAME_OVER    8

/* playerInfo fields */
#define PI_CLASS              2251u     /* u8, GAME2.c:2214 */
#define PI_FLAGS              3680u     /* u8, bit0 observer (GAME2.c:29138) */
#define PI_HP_PCT             2282u     /* u8 0..100, client opcode 0xC4 sub 12 (GAME2.c:52755) */
#define PI_POISON_BIT         (1u << 10) /* u32 +3680 bit10: alt bar colour (GAME3.c:16243) */
#define PI_ARMOR_FLAGS        0u        /* u32 armor mask: opcodes 0x4F/0x52 set, 0x53 clear (sub_417AA0/sub_417B80) */
#define PI_WEAPON_FLAGS       4u        /* u32 weapon mask: opcodes 0x50/0x51 set, 0x54 clear */
#define GAMEFLAG_QUEST        4096       /* sub_40A5C0(0x1000): Quest; gates 0xC4/12 + tag bar */

/* Client owned-unit health table (sub_495020): 32 x 16 B at [1200916]:
 * +0 net code, +4 u8 poison colour, +6 u16 hp, +8 u16 hp_max, +12 active.
 * Filled by opcodes 0xDB/0xDC (add/remove), 0xDD (hp, max), 0x41 (hp/2). */
#define OFF_UNIT_HP_TABLE     1200916u
#define UNIT_HP_SLOTS         32u
#define UNIT_HP_STRIDE        16u

/* drawable fields */
#define DR_X                  12u
#define DR_Y                  16u
#define DR_TYPE               108u
#define DR_CLASS              112u
#define DR_SUBCLASS           116u      /* thing.bin SUBCLASS, copied by sub_44D0C0 (GAME2.c:310) */
#define DR_ENCHANT            124u      /* enchant bitmask, sub_4356C0(d, bit) */
#define DR_NETCODE            128u
#define DR_ANIM               276u
#define DR_FACING             297u
#define DR_NEXT               368u
#define DR_PLAYER_NEXT        416u
#define DR_CLASS_NONNET       0x20400000u /* skipped by sub_45A6F0 netcode lookup */
#define ENCH_INVISIBLE_BIT    (1u << 0)   /* GAME2.c:30355 renderer skips invisible non-allies */
#define ENCH_INFRAVISION_BIT  (1u << 21)  /* GAME2.c:32368 local enchant 21 reveals invisible */

#define PT_CFG_PATH           NOX_3DS_DATA_ROOT "/playtest.cfg"
#define PT_DEFAULT_PORT       47800u
#define PT_DEFAULT_LISTEN     47801u
#define PT_MAX_CMDS_PER_FRAME 4
#define PT_MAX_TAPS           8
#define PT_MAX_VISITS         1024      /* per-publish drawable visit budget (spatial query) */
#define PT_MAX_PLAYER_WALK    256       /* player-drawable list walk cap */
#define PT_TTL_DEFAULT_MS     250u
#define PT_TTL_MAX_MS         2000u
#define PT_AIM_DIR_RADIUS     64.0f
#define PT_RETRY_MS           10000u
#define PT_MAX_ALLOW          8

enum { PT_UNINIT = 0, PT_DISABLED, PT_RETRY, PT_ACTIVE };

typedef struct {
    uint32_t host_ip_be;
    uint16_t port;
    uint16_t listen_port;
    uint32_t every;          /* publish every N calls (publish_every_n_frames) */
    uint32_t max_objects;
    bool     commands;
    bool     clip_viewport;
    bool     accept_any_src;
    uint32_t n_allow;
    bool     allow_any;      /* mp_server_allow=* (dev-only wildcard) */
    char     allow[PT_MAX_ALLOW][PT_GATE_NAME_MAX + 1]; /* mp_server_allow entries (<=15 chars) */
} pt_cfg_t;

typedef enum { TAP_KEY = 0, TAP_MOUSE = 1 } tap_kind_t;

typedef struct {
    bool       used;
    bool       pressed;
    tap_kind_t kind;
    int        code;         /* SDL scancode or mouse button */
    uint8_t    delay;        /* frames before press */
    uint8_t    hold;         /* frames to hold after press */
} pt_tap_t;

static int        g_pt_state = PT_UNINIT;
static pt_cfg_t   g_cfg;
static int        g_sock = -1;
static uint32_t   g_tick;
static uint32_t   g_seq;
static bool       g_send_err_logged;

static bool       g_move_on;
static uint8_t    g_move_orient;
static uint8_t    g_move_cmd;
static uint32_t   g_move_until_ms;

static bool       g_aim_on;
static bool       g_aim_is_dir;
static int32_t    g_aim_a, g_aim_b;
static uint32_t   g_aim_until_ms;

static pt_tap_t   g_taps[PT_MAX_TAPS];
static bool       g_was_cmds_active;
static uint32_t   g_retry_at_ms;
static bool       g_open_err_logged;

/* Client server-name snapshot, filled ONLY by a fresh opcode 0xB0 in this
 * session (nox_pt_note_server_name) and cleared when the player returns to the
 * menu shell from a map or leaves WIFI mode. Fail closed while empty. */
static pt_gate_t  g_gate;             /* see playtest_gate.h */
static bool       g_in_session;       /* is_mp && map_loaded, last frame */

/* Static send buffer: header + max objects. No per-frame allocation. */
static uint8_t    g_txbuf[sizeof(nox_pt_state_hdr_t) + NOX_PT_MAX_OBJECTS * sizeof(nox_pt_obj_t)
                          + 2u + NOX_PT_MAX_NAMES * sizeof(nox_pt_name_t)
                          + 2u + (NOX_PT_MAX_OBJECTS + 1u) * sizeof(nox_pt_ext_t)
                          + 4u + NOX_PT_MAX_MIS * sizeof(nox_pt_mis_t)
                          + sizeof(nox_pt_session_t) + NOX_PT_MAX_EV_PKT * sizeof(pt_event_t)
                          + NOX_PT_MAX_SCORES * sizeof(nox_pt_score_t) + NOX_PT_MAX_OBL * sizeof(nox_pt_obl_t)];

/* v6: client message events (playtest_events.h), filled by nox_pt_note_msg. */
static pt_ev_ring_t g_ev;
static pt_chant_t   g_chant[PT_CHANT_STREAMS];
static pt_obl_t     g_obl[PT_OBL_SLOTS];
static uint32_t     g_map_seq;
static bool         g_ev_others_ok;   /* export gate of the last pump (MP gate open or not MP) */
static bool         g_ev_init;

/* v5 missile tracker: visible missiles sampled on every pump (playtest_missile.h). */
static pt_mtrk_t  g_mtrk[PT_MTRK_SLOTS];

/* v4 velocity: positions from the previous publish (local + objects). */
typedef struct { uint32_t netcode; int32_t x, y; } pt_prev_t;
static pt_prev_t  g_prev[NOX_PT_MAX_OBJECTS + 1u];
static uint32_t   g_n_prev;
static uint32_t   g_prev_frame;

typedef struct { uint32_t key; uint32_t ptr; } pt_cand_t;
static pt_cand_t  g_cand[NOX_PT_MAX_OBJECTS];

/* ---- small helpers ----------------------------------------------------- */
static inline uint32_t rd_u32(uint32_t off) { return nox_read_u32(&byte_5D4594[off]); }
static inline int32_t  rd_s32(uint32_t off) { return nox_read_s32(&byte_5D4594[off]); }
static inline const uint8_t *ptr_of(uint32_t v) { return (const uint8_t *)(uintptr_t)v; }
static inline uint32_t dr_u32(uint32_t d, uint32_t off) { return nox_read_u32(ptr_of(d) + off); }
static inline int32_t  dr_s32(uint32_t d, uint32_t off) { return nox_read_s32(ptr_of(d) + off); }
static inline uint8_t  dr_u8(uint32_t d, uint32_t off)  { return ptr_of(d)[off]; }

static uint16_t clamp_u16(int32_t v)
{
    if (v < 0) return 0;
    if (v > 0xFFFF) return 0xFFFF;
    return (uint16_t)v;
}

/* Same orientation convention as gamepad.c update_left_analog_absolute()
 * and the src/input.c translator: dx = -cos(ang), dy = -sin(ang),
 * ang = (o/128 - 1) * pi. */
static uint8_t orient_from_vec(float dx, float dy)
{
    float theta = atan2f(-dy, -dx) / (float)M_PI;
    return (uint8_t)(((int)((theta + 1.0f) * 128.0f + 0.5f)) & 255);
}

static void vec_from_orient(uint8_t o, float *dx, float *dy)
{
    float ang = (((float)o / 128.0f) - 1.0f) * (float)M_PI;
    *dx = -cosf(ang);
    *dy = -sinf(ang);
}

/* ---- config ------------------------------------------------------------ */
static bool cfg_bool(const char *v) { return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T'; }

static bool load_cfg(void)
{
    FILE *f = fopen(PT_CFG_PATH, "r");
    if (!f)
        return false;

    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.port = PT_DEFAULT_PORT;
    g_cfg.listen_port = PT_DEFAULT_LISTEN;
    g_cfg.every = 1;
    g_cfg.max_objects = NOX_PT_MAX_OBJECTS;
    g_cfg.commands = true;
    g_cfg.clip_viewport = true;
    bool have_host = false;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == ';' || *p == '\0' || *p == '\n' || *p == '\r')
            continue;
        char *eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *k = p, *v = eq + 1;
        for (char *e = eq - 1; e >= k && (*e == ' ' || *e == '\t'); --e) *e = '\0';
        while (*v == ' ' || *v == '\t') ++v;
        if (!strcmp(k, "mp_server_allow")) {
            /* Comma-separated; names may contain spaces; each entry trimmed. */
            v[strcspn(v, "\r\n#")] = '\0';
            g_cfg.n_allow = 0;
            for (char *tok = v; tok && g_cfg.n_allow < PT_MAX_ALLOW;) {
                char *comma = strchr(tok, ',');
                if (comma) *comma = '\0';
                while (*tok == ' ' || *tok == '\t') ++tok;
                size_t tl = strlen(tok);
                while (tl && (tok[tl - 1] == ' ' || tok[tl - 1] == '\t')) --tl;
                if (tl > 15) tl = 15; /* Nox server names are <=15 chars */
                if (tl == 1 && tok[0] == '*') {
                    g_cfg.allow_any = true;
                } else if (tl) {
                    memcpy(g_cfg.allow[g_cfg.n_allow], tok, tl);
                    g_cfg.allow[g_cfg.n_allow][tl] = '\0';
                    ++g_cfg.n_allow;
                }
                tok = comma ? comma + 1 : NULL;
            }
            continue;
        }
        v[strcspn(v, " \t\r\n#")] = '\0';

        if (!strcmp(k, "host")) {
            struct in_addr a;
            if (inet_aton(v, &a)) {
                g_cfg.host_ip_be = a.s_addr;
                have_host = true;
            }
        } else if (!strcmp(k, "port")) {
            long n = strtol(v, NULL, 10);
            if (n > 0 && n < 65536) g_cfg.port = (uint16_t)n;
        } else if (!strcmp(k, "listen_port")) {
            long n = strtol(v, NULL, 10);
            if (n > 0 && n < 65536) g_cfg.listen_port = (uint16_t)n;
        } else if (!strcmp(k, "every") || !strcmp(k, "publish_every_n_frames")) {
            long n = strtol(v, NULL, 10);
            if (n >= 1 && n <= 60) g_cfg.every = (uint32_t)n;
        } else if (!strcmp(k, "max_objects")) {
            long n = strtol(v, NULL, 10);
            if (n >= 0 && n <= (long)NOX_PT_MAX_OBJECTS) g_cfg.max_objects = (uint32_t)n;
        } else if (!strcmp(k, "commands")) {
            g_cfg.commands = cfg_bool(v);
        } else if (!strcmp(k, "clip_viewport")) {
            g_cfg.clip_viewport = cfg_bool(v);
        } else if (!strcmp(k, "accept_any_src")) {
            g_cfg.accept_any_src = cfg_bool(v);
        }
    }
    fclose(f);

    if (!have_host) {
        nox_3ds_net_log("[pt] %s present but has no valid host=a.b.c.d; hook disabled", PT_CFG_PATH);
        return false;
    }
    return true;
}

/* ---- socket ------------------------------------------------------------ */
static void close_sock(void)
{
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
}

/* Log open failures once; retries stay silent. */
#define PT_OPEN_LOG(...) do { if (!g_open_err_logged) { g_open_err_logged = true; nox_3ds_net_log(__VA_ARGS__); } } while (0)

static bool open_sock(void)
{
    if (!nox_3ds_soc_ensure_ready_any_mode()) {
        PT_OPEN_LOG("[pt] soc:u init failed; retrying every %us", (unsigned)(PT_RETRY_MS / 1000u));
        return false;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        PT_OPEN_LOG("[pt] socket() failed errno=%d; retrying", errno);
        return false;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) {
        /* Never risk a blocking recv on the main loop. */
        PT_OPEN_LOG("[pt] fcntl(O_NONBLOCK) failed errno=%d; retrying", errno);
        close(fd);
        return false;
    }
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(g_cfg.listen_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        PT_OPEN_LOG("[pt] bind(:%u) failed errno=%d; retrying", (unsigned)g_cfg.listen_port, errno);
        close(fd);
        return false;
    }
    g_sock = fd;
    return true;
}

static void pt_log_active(void);

static void pt_try_open(void)
{
    if (!open_sock()) {
        g_pt_state = PT_RETRY;
        g_retry_at_ms = nox_3ds_get_ticks_ms() + PT_RETRY_MS;
        return;
    }
    pt_log_active();
}

static void pt_init(void)
{
    g_pt_state = PT_DISABLED;
    if (!load_cfg())
        return; /* no cfg: fully inert, no log */
    pt_try_open();
}

static void pt_log_active(void)
{
    const uint8_t *ip = (const uint8_t *)&g_cfg.host_ip_be;
    nox_3ds_net_log("[pt] playtest hook ACTIVE -> %u.%u.%u.%u:%u, cmd port %u, every %u, commands %s",
                    ip[0], ip[1], ip[2], ip[3], (unsigned)g_cfg.port, (unsigned)g_cfg.listen_port,
                    (unsigned)g_cfg.every, g_cfg.commands ? "on" : "off");
    g_pt_state = PT_ACTIVE;
}

/* ---- session / gating -------------------------------------------------- */
static bool session_is_mp(void)
{
    /* WIFI net mode and not Solo campaign (0x800, GAME3.c:36001). */
    return nox_3ds_net_get_mode() == NOX_3DS_NET_MODE_WIFI && !sub_40A5C0(2048);
}

void nox_pt_note_server_name(const char *name)
{
    if (g_pt_state != PT_ACTIVE || !name)
        return;
    pt_gate_note_b0(&g_gate, name, g_in_session);
}

/* Name of the server-list row selected as the join target, or NULL. */
static const char *selected_row_name(char *buf)
{
    if (!rd_u32(OFF_SEL_SERVER_VALID))
        return NULL;
    uint32_t row = rd_u32(OFF_SEL_SERVER_ROW);
    if (!row)
        return NULL;
    memcpy(buf, ptr_of(row) + ROW_SERVER_NAME, PT_GATE_ROW_MAX);
    buf[PT_GATE_ROW_MAX] = '\0';
    return buf;
}

/* Current server name, or NULL while unknown (fail closed).
 * Host: its own configured server name (always current).
 * Client: only the snapshot from a 0xB0 received this session; the engine
 * copy at byte_5D4594[371389] survives across sessions, so it is not used. */
static const char *current_server_name(void)
{
    if (sub_40A5C0(1))
        return sub_40A4C0();
    return pt_gate_name(&g_gate);
}

/* ---- local-player + visible-object snapshot ---------------------------- */
typedef struct {
    bool     valid;
    int32_t  x, y;
    int32_t  cam_x, cam_y;
    int      vp_l, vp_t, vp_r, vp_b;
} pt_view_t;

static void read_view(pt_view_t *v)
{
    v->cam_x = rd_s32(OFF_CAM_X);
    v->cam_y = rd_s32(OFF_CAM_Y);
    v->vp_l = 0; v->vp_t = 0; v->vp_r = 639; v->vp_b = 479;
    const char *vp = sub_437250();
    if (vp) {
        int l = nox_read_s32(vp + 0), t = nox_read_s32(vp + 4);
        int r = nox_read_s32(vp + 8), b = nox_read_s32(vp + 12);
        if (r > l && b > t) {
            v->vp_l = l; v->vp_t = t; v->vp_r = r; v->vp_b = b;
        }
    }
}

/* Find the local player's drawable via the short client player-drawable list
 * (byte_5D4594[1046600], next +416; every class&4 drawable is linked at
 * creation by sub_459ED0, GAME2.c:9654, and unlinked in sub_45A4E0). The full
 * drawable list can exceed any sane cap in Quest maps. The cached pointer
 * byte_5D4594[2614252] is not used: it is refreshed only in the draw pass and
 * can dangle after a purge between draws. */
static uint32_t find_local_drawable(uint32_t netcode)
{
    if (!netcode || !rd_u32(OFF_DRAWABLE_LIST))
        return 0; /* no drawables (pool torn down / not in a map) */
    uint32_t d = rd_u32(OFF_PLAYER_DR_LIST);
    for (int n = 0; d && n < PT_MAX_PLAYER_WALK; ++n) {
        uint32_t cls = dr_u32(d, DR_CLASS);
        if ((cls & 4u) && !(cls & DR_CLASS_NONNET) && dr_u32(d, DR_NETCODE) == netcode)
            return d;
        d = dr_u32(d, DR_PLAYER_NEXT);
    }
    return 0;
}

static bool npc_dialog_open(void)
{
    uint32_t w = rd_u32(OFF_NPC_DIALOG_WND);
    return w && sub_46ACC0((int)w) != 1;
}

/* Spatial query state for the sub_49AB00 callback (no allocation). */
typedef struct {
    uint32_t self_d, max, count, worst, visits, frame;
    int32_t  px, py, wx0, wy0, wx1, wy1;
    bool     valid, clip, infravision, truncated, track_only;
} pt_collect_t;
static pt_collect_t g_col;

/* Same filters as the export: camera rect, invisibility, draw visibility. */
static bool obj_passes_filters(const pt_collect_t *c, uint32_t d, int32_t x, int32_t y)
{
    if (c->clip && (x < c->wx0 || x > c->wx1 || y < c->wy0 || y > c->wy1))
        return false;
    /* Invisible objects: the renderer hides them (GAME2.c:30355) unless
     * allied; we drop them unless the local player has infravision. Allies
     * are dropped too (conservative; ally test needs sub_418C80). */
    if (!c->infravision && (dr_u32(d, DR_ENCHANT) & ENCH_INVISIBLE_BIT))
        return false;
    /* Same draw-visibility test the renderer applies (GAME2.c:30306, 30849):
     * view polygon / line of sight from the last frame. */
    if (c->clip && !sub_4984B0((int)d))
        return false;
    return true;
}

static void collect_cb(int di, int unused)
{
    (void)unused;
    pt_collect_t *c = &g_col;
    uint32_t d = (uint32_t)di;
    if (d == c->self_d)
        return;
    uint32_t cls = dr_u32(d, DR_CLASS);
    /* Visit budget: past PT_MAX_VISITS only missiles are still considered
     * (class test is cheap), up to twice the budget, so a crowded bucket
     * cannot hide an incoming missile. */
    if (++c->visits > PT_MAX_VISITS) {
        if (!c->track_only)
            c->truncated = true;
        if (c->visits > 2u * PT_MAX_VISITS || !(cls & 1u))
            return;
    }
    if (cls & DR_CLASS_NONNET)
        return;
    if (c->track_only && !(cls & 1u))
        return;
    int32_t x = dr_s32(d, DR_X), y = dr_s32(d, DR_Y);
    if (!obj_passes_filters(c, d, x, y))
        return;
    if (cls & 1u)
        pt_mtrk_note(g_mtrk, PT_MTRK_SLOTS, dr_u32(d, DR_NETCODE), x, y, c->frame, g_tick);
    if (c->track_only)
        return;

    /* near missiles, players, monsters, other missiles, others; nearest
     * first. No local player: no distances, so no missile counts as near. */
    int32_t dx = x - c->px, dy = y - c->py;
    int64_t dd = c->valid ? (int64_t)dx * dx + (int64_t)dy * dy : (int64_t)0xFFFFFF;
    uint32_t key = pt_obj_sort_key(cls, dd);

    if (c->count < c->max) {
        g_cand[c->count].key = key;
        g_cand[c->count].ptr = d;
        if (c->count == 0 || key > g_cand[c->worst].key) c->worst = c->count;
        ++c->count;
    } else {
        c->truncated = true;
        if (key < g_cand[c->worst].key) {
            g_cand[c->worst].key = key;
            g_cand[c->worst].ptr = d;
            c->worst = 0;
            for (uint32_t i = 1; i < c->count; ++i)
                if (g_cand[i].key > g_cand[c->worst].key) c->worst = i;
        }
    }
}

/* Objects come from the client's own spatial grid (128 px buckets at
 * byte_5D4594[1301784], size [1301788], chain next +400), queried with the
 * same read-only walker the renderer uses to build its per-frame draw lists
 * (sub_475560 via sub_49AB00, GAME2.c:30826). Only buckets overlapping the
 * query rect are visited, so cost scales with nearby objects, not map size. */
static uint32_t collect_objects(uint32_t self_d, const pt_view_t *v, uint32_t max, bool clip,
                                bool track_only, bool *truncated)
{
    *truncated = false;
    if ((max == 0 && !track_only) || !rd_u32(OFF_DRAW_GRID) || rd_s32(OFF_DRAW_GRID_N) <= 0)
        return 0;

    pt_collect_t *c = &g_col;
    memset(c, 0, sizeof(*c));
    c->self_d = self_d;
    c->max = track_only ? 0 : max;
    c->track_only = track_only;
    c->frame = rd_u32(OFF_CLIENT_FRAME);
    c->valid = v->valid;
    c->px = v->x;
    c->py = v->y;
    c->clip = clip;
    c->infravision = self_d && (dr_u32(self_d, DR_ENCHANT) & ENCH_INFRAVISION_BIT);
    int32_t vw = v->vp_r - v->vp_l, vh = v->vp_b - v->vp_t;
    c->wx0 = v->cam_x;
    c->wy0 = v->cam_y;
    c->wx1 = v->cam_x + vw;
    c->wy1 = v->cam_y + vh;

    /* Query rect {l, t, r, b} like sub_4754F0: camera rect, +128 px at the
     * bottom for tall sprites. Unclipped (Solo only): one extra view each side. */
    int32_t q[4] = { c->wx0, c->wy0, c->wx1, c->wy1 + 128 };
    if (!clip) {
        q[0] -= vw; q[1] -= vh; q[2] += vw; q[3] += vh;
    }
    sub_49AB00(q, collect_cb, 0);
    if (track_only)
        return 0;

    uint32_t count = c->count;
    *truncated = c->truncated;
    /* insertion sort by key (pt_obj_sort_key) */
    for (uint32_t i = 1; i < count; ++i) {
        pt_cand_t cc = g_cand[i];
        uint32_t j = i;
        while (j > 0 && g_cand[j - 1].key > cc.key) { g_cand[j] = g_cand[j - 1]; --j; }
        g_cand[j] = cc;
    }
    return count;
}

/* Client player table lookup by net code (read-only reimplementation of
 * sub_417040, which also writes a slot-index byte, so it is not called). */
static uint32_t playerinfo_off_for(uint32_t netcode)
{
    for (uint32_t i = 0; i < PLAYERINFO_SLOTS; ++i) {
        uint32_t off = OFF_PLAYERINFO_TABLE + PLAYERINFO_STRIDE * i;
        if (rd_u32(off + PI_ACTIVE) && rd_u32(off + PI_NETCODE) == netcode)
            return off;
    }
    return 0;
}

/* UTF-16 (BMP; surrogates -> '?') to UTF-8, truncated on a char boundary. */
static uint8_t pt_utf16_to_utf8(const uint8_t *src, uint32_t max_wchars, char *dst, uint32_t cap)
{
    uint32_t o = 0;
    for (uint32_t i = 0; i < max_wchars; ++i) {
        uint32_t c = nox_read_u16(src + 2u * i);
        if (!c)
            break;
        if (c >= 0xD800 && c <= 0xDFFF)
            c = '?';
        uint32_t need = c < 0x80 ? 1u : c < 0x800 ? 2u : 3u;
        if (o + need > cap)
            break;
        if (need == 1) {
            dst[o++] = (char)c;
        } else if (need == 2) {
            dst[o++] = (char)(0xC0 | (c >> 6));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            dst[o++] = (char)(0xE0 | (c >> 12));
            dst[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    return (uint8_t)o;
}

static bool append_name(uint8_t *dst, uint32_t netcode)
{
    uint32_t off = playerinfo_off_for(netcode);
    if (!off)
        return false;
    nox_pt_name_t e;
    memset(&e, 0, sizeof(e));
    e.net_id = (uint16_t)netcode;
    e.len = pt_utf16_to_utf8(&byte_5D4594[off + PI_NAME], PI_NAME_MAX_WCHARS, e.name, NOX_PT_NAME_BYTES);
    memcpy(dst, &e, sizeof(e));
    return true;
}

/* Health bar and class for one exported object, from client-received data
 * only (the same values the renderer draws):
 *  - Players, Quest: playerInfo+2282, the percent sub_4B8EB0 draws beside
 *    the name tag (GAME3.c:16243). The server sends it as 100*hp/max in
 *    opcode 0xC4 sub 12 (sub_4D8670, GAME3.c:41455) only when flag 0x1000
 *    (Quest) is set. Outside Quest the client never receives other players'
 *    HP, so hp_pct stays 0xFF.
 *  - Any object in the owned-unit table [1200916] (summons/pets the server
 *    tracks for this client, sub_4D9250): cur/max as drawn by sub_4BC080
 *    (GAME3.c:16855, 19103).
 *  - class_id: playerInfo+2251 for player objects; the client gets it in the
 *    opcode 0x2D player record (GAME2.c:50294, qmemcpy into +2185). */
static void obj_health_class(uint32_t netcode, uint32_t class_flags, nox_pt_obj_t *ob)
{
    ob->hp_pct = 0xFF;
    ob->class_id = 0xFF;
    ob->hp_flags = 0;
    if (class_flags & 4u) {
        uint32_t pi = playerinfo_off_for(netcode);
        if (pi) {
            ob->class_id = byte_5D4594[pi + PI_CLASS];
            if (sub_40A5C0(GAMEFLAG_QUEST)) {
                uint8_t pct = byte_5D4594[pi + PI_HP_PCT];
                ob->hp_pct = pct > 100 ? 100 : pct;
                ob->hp_flags |= NOX_PT_HPF_QUEST_TAG;
                if (rd_u32(pi + PI_FLAGS) & PI_POISON_BIT)
                    ob->hp_flags |= NOX_PT_HPF_POISONED;
                return;
            }
        }
    }
    for (uint32_t i = 0; i < UNIT_HP_SLOTS; ++i) {
        uint32_t e = OFF_UNIT_HP_TABLE + UNIT_HP_STRIDE * i;
        if (!rd_u32(e + 12) || rd_u32(e) != netcode)
            continue;
        uint32_t cur = nox_read_u16(&byte_5D4594[e + 6]);
        uint32_t max = nox_read_u16(&byte_5D4594[e + 8]);
        if (max) { /* sub_4BC080 draws nothing when max == 0 */
            uint32_t pct = cur * 100u / max;
            ob->hp_pct = (uint8_t)(pct > 100 ? 100 : pct);
            ob->hp_flags |= NOX_PT_HPF_UNIT_BAR;
            if (byte_5D4594[e + 4])
                ob->hp_flags |= NOX_PT_HPF_POISONED;
        }
        return;
    }
}

/* Owned-unit table membership (own summons/pets), same table as above. */
static bool obj_owned_by_me(uint32_t netcode)
{
    for (uint32_t i = 0; i < UNIT_HP_SLOTS; ++i) {
        uint32_t e = OFF_UNIT_HP_TABLE + UNIT_HP_STRIDE * i;
        if (rd_u32(e + 12) && rd_u32(e) == netcode)
            return true;
    }
    return false;
}

/* Client friend list (MSG_OBJECT_FRIEND_ADD 0x34 / REMOVE 0x35 / RESET 0x36):
 * objects the server marks as not hostile to this player (teammates, own and
 * team creatures; server test sub_4E7F90 = !sub_5330C0). Read-only walk
 * like sub_495A80. */
static bool obj_is_friend(uint32_t netcode)
{
    uint32_t n = rd_u32(OFF_FRIEND_LIST);
    for (int i = 0; n && i < PT_MAX_FRIEND_WALK; ++i) {
        if (nox_read_u32(ptr_of(n)) == netcode)
            return true;
        n = nox_read_u32(ptr_of(n) + 4);
    }
    return false;
}

static int16_t clamp_s16(int32_t v)
{
    if (v < -32768) return -32768;
    if (v > 32767) return 32767;
    return (int16_t)v;
}

/* v4 ext entry for drawable d, from client-received display state only:
 *  - enchant: drawable +124, written only by opcode 0x5A (GAME2.c:51124),
 *    the mask the renderer tests with sub_4356C0 to draw auras/effects.
 *  - weapon/armor flags: client player table +4/+0, set by opcodes 0x50/0x51
 *    and 0x4F/0x52 (sub_417AA0, GAME1.c:21632), cleared by 0x54/0x53
 *    (sub_417B80); the renderer uses them to draw equipment on the sprite.
 *  - velocity: hook-side delta of the client position since the last publish. */
static void fill_ext(uint32_t d, nox_pt_ext_t *e, uint32_t now_frame, pt_prev_t *cur, uint32_t *n_cur)
{
    memset(e, 0, sizeof(*e));
    uint32_t nc = dr_u32(d, DR_NETCODE);
    uint32_t cls = dr_u32(d, DR_CLASS);
    int32_t x = dr_s32(d, DR_X), y = dr_s32(d, DR_Y);
    e->net_id = (uint16_t)nc;
    e->anim = (uint8_t)dr_u32(d, DR_ANIM);
    e->enchant = dr_u32(d, DR_ENCHANT);
    if (cls & 1u)
        e->ext_flags |= NOX_PT_XF_MISSILE;
    if (cls & 4u) {
        uint32_t pi = playerinfo_off_for(nc);
        if (pi) {
            e->weapon_flags = rd_u32(pi + PI_WEAPON_FLAGS);
            e->armor_flags = rd_u32(pi + PI_ARMOR_FLAGS);
            e->ext_flags |= NOX_PT_XF_EQUIP;
        }
    } else if (obj_owned_by_me(nc)) {
        e->ext_flags |= NOX_PT_XF_OWNED;
    }
    if (obj_is_friend(nc))
        e->ext_flags |= NOX_PT_XF_FRIEND;
    uint32_t df = now_frame - g_prev_frame;
    const pt_mtrk_t *tr = (cls & 1u) ? pt_mtrk_find(g_mtrk, PT_MTRK_SLOTS, nc) : NULL;
    if (tr && tr->vel) {
        /* v5: per-pump missile tracker (valid from the first export) */
        e->vx = clamp_s16(tr->vx16);
        e->vy = clamp_s16(tr->vy16);
        e->ext_flags |= NOX_PT_XF_VEL;
    } else if (g_n_prev && df > 0 && df < 64) {
        for (uint32_t i = 0; i < g_n_prev; ++i) {
            if (g_prev[i].netcode == nc) {
                e->vx = clamp_s16((x - g_prev[i].x) * 16 / (int32_t)df);
                e->vy = clamp_s16((y - g_prev[i].y) * 16 / (int32_t)df);
                e->ext_flags |= NOX_PT_XF_VEL;
                break;
            }
        }
    }
    if (*n_cur < NOX_PT_MAX_OBJECTS + 1u) {
        cur[*n_cur].netcode = nc;
        cur[*n_cur].x = x;
        cur[*n_cur].y = y;
        ++*n_cur;
    }
}

/* Active spell set 0..4 (sub_45E180: spell-bar struct +200, client GUI
 * state), or 0xFF when unknown. */
static uint8_t read_spell_set(void)
{
    uint32_t bar = nox_read_u32(&byte_587000[OFF587_SPELLBAR]);
    if (!bar)
        return 0xFF;
    uint8_t s = dr_u8(bar, SPELLBAR_CUR_SET);
    return s < PT_SPELL_SETS ? s : 0xFF;
}

/* ---- v6 events --------------------------------------------------------- */
/* Client spell phoneme trie (sub_424820 root, sub_424830 child): node +0
 * spell id, +4+4*p child for phoneme p (0..8). Read-only. */
static uintptr_t trie_child(uintptr_t node, int p)
{
    if (!node || p < 0 || p > 8)
        return 0;
    return nox_read_u32((const uint8_t *)node + 4u + 4u * (uint32_t)p);
}
static int trie_spell(uintptr_t node)
{
    if (!node)
        return 0;
    int32_t v = nox_read_s32((const uint8_t *)node);
    return (v > 0 && v < 137) ? v : 0;
}
static void trie_get(pt_trie_t *t)
{
    t->root = nox_read_u32(&byte_587000[OFF587_SPELL_TRIE]);
    t->child = trie_child;
    t->spell = trie_spell;
}

static void ev_init_once(void)
{
    if (g_ev_init)
        return;
    g_ev_init = true;
    pt_ev_reset(&g_ev);
    memset(g_chant, 0, sizeof(g_chant));
    memset(g_obl, 0, sizeof(g_obl));
}

void nox_pt_note_msg(const unsigned char *p, int avail)
{
    if (g_pt_state != PT_ACTIVE || !p || avail <= 0)
        return;
    ev_init_once();
    pt_msg_ctx_t cx;
    cx.frame = rd_u32(OFF_CLIENT_FRAME);
    cx.map_gate = rd_s32(OFF_MAP_GATE);
    cx.map_loaded = sub_43C700() != 0;
    cx.rec_others = g_ev_others_ok;
    pt_trie_t t;
    trie_get(&t);
    pt_event_t *e = pt_ev_parse_msg(&g_ev, g_chant, &t, p, (uint32_t)avail, &cx);
    if (!e)
        return;
    if (e->type == PT_EV_OBELISK) {
        pt_obl_note(g_obl, e->a, e->b);
    } else if (e->type == PT_EV_MAP) {
        ++g_map_seq;
        memset(g_obl, 0, sizeof(g_obl));
        memset(g_chant, 0, sizeof(g_chant));
    }
}

/* v6 block: session, newest events, scoreboard, obelisks. Returns the end. */
static uint8_t *append_v6(uint8_t *ne, uint32_t netcode, bool export_objects)
{
    ev_init_once();
    nox_pt_session_t ss;
    memset(&ss, 0, sizeof(ss));
    const char *mp = (const char *)&byte_5D4594[OFF_MAP_NAME];
    size_t ml = strnlen(mp, NOX_PT_MAP_BYTES - 1u);
    memcpy(ss.map, mp, ml);
    ss.map_seq = g_map_seq;
    ss.game_flags = rd_u32(OFF_GAME_FLAGS);
    ss.timer_ms = 0xFFFFFFFFu;
    if (sub_40A220()) {
        int32_t ms = sub_40A230();
        ss.timer_ms = ms < 0 ? 0u : (uint32_t)ms;
        ss.s_flags |= NOX_PT_SF_TIMER_ON;
    }
    ss.frag_limit = nox_read_u16(&byte_5D4594[OFF_SETTINGS0 + 54u]);
    ss.time_limit_min = byte_5D4594[OFF_SETTINGS0 + 56u];
    if (sub_40A5C0(GAMEFLAG_GAME_OVER))
        ss.s_flags |= NOX_PT_SF_GAME_OVER;
    if (!g_ev_others_ok)
        ss.s_flags |= NOX_PT_SF_EV_GATED;
    ss.ev_next_seq = g_ev.next_seq;
    uint8_t *o = ne + sizeof(ss);

    static pt_event_t evs[NOX_PT_MAX_EV_PKT];
    uint32_t n_ev = pt_ev_newest(&g_ev, evs, NOX_PT_MAX_EV_PKT);
    memcpy(o, evs, n_ev * sizeof(pt_event_t));
    o += n_ev * sizeof(pt_event_t);

    /* Scoreboard: the client player table the rank screen (guirank.c,
     * GAME2.c:25430-25850) reads. With a closed MP gate, the local entry only. */
    uint32_t n_sc = 0;
    for (uint32_t i = 0; i < PLAYERINFO_SLOTS && n_sc < NOX_PT_MAX_SCORES; ++i) {
        uint32_t off = OFF_PLAYERINFO_TABLE + PLAYERINFO_STRIDE * i;
        if (!rd_u32(off + PI_ACTIVE))
            continue;
        uint32_t nc = rd_u32(off + PI_NETCODE);
        if (!export_objects && nc != netcode)
            continue;
        nox_pt_score_t sc;
        sc.net_id = (uint16_t)nc;
        sc.class_id = byte_5D4594[off + PI_CLASS];
        sc.pflags = byte_5D4594[off + PI_FLAGS];
        sc.score = rd_s32(off + PI_SCORE);
        sc.deaths = rd_s32(off + PI_DEATHS);
        memcpy(o, &sc, sizeof(sc));
        o += sizeof(sc);
        ++n_sc;
    }
    uint32_t n_ob = 0;
    if (export_objects) {
        for (uint32_t i = 0; i < PT_OBL_SLOTS && n_ob < NOX_PT_MAX_OBL; ++i) {
            if (!g_obl[i].used)
                continue;
            nox_pt_obl_t ob = { g_obl[i].net, g_obl[i].charge, 0 };
            memcpy(o, &ob, sizeof(ob));
            o += sizeof(ob);
            ++n_ob;
        }
    }
    ss.n_ev = (uint8_t)n_ev;
    ss.ev_size = (uint8_t)sizeof(pt_event_t);
    ss.n_score = (uint8_t)n_sc;
    ss.score_size = (uint8_t)sizeof(nox_pt_score_t);
    ss.n_obl = (uint8_t)n_ob;
    ss.obl_size = (uint8_t)sizeof(nox_pt_obl_t);
    memcpy(ne, &ss, sizeof(ss));
    return o;
}

static void publish_state(uint16_t flags_in, uint32_t netcode, uint32_t self_d, const pt_view_t *v,
                          bool export_objects, bool clip)
{
    nox_pt_state_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = NOX_PT_MAGIC_STATE;
    h.version = NOX_PT_VERSION;
    h.header_size = (uint16_t)sizeof(h);
    h.obj_size = (uint16_t)sizeof(nox_pt_obj_t);
    h.seq = ++g_seq;
    h.game_frame = rd_u32(OFF_CLIENT_FRAME);
    h.hook_tick = g_tick;
    h.net_id = netcode;
    h.player_class = 0xFF;
    h.cam_x = v->cam_x;
    h.cam_y = v->cam_y;

    uint16_t flags = flags_in;
    uint32_t pinfo = rd_u32(OFF_LOCAL_PLAYERINFO);
    if (pinfo) {
        h.player_class = dr_u8(pinfo, PI_CLASS);
        if (dr_u8(pinfo, PI_FLAGS) & 1u)
            flags |= NOX_PT_F_OBSERVER;
    }

    uint32_t n_obj = 0;
    if (flags & NOX_PT_F_MAP_LOADED) {
        if (self_d) {
            h.hp = clamp_u16(rd_s32(OFF_HP_CUR));
            h.hp_max = clamp_u16(rd_s32(OFF_HP_MAX));
            h.mp = clamp_u16(rd_s32(OFF_MP_CUR));
            h.mp_max = clamp_u16(rd_s32(OFF_MP_MAX));
            h.x = v->x;
            h.y = v->y;
            h.facing = dr_u8(self_d, DR_FACING);
            h.anim = (uint8_t)dr_u32(self_d, DR_ANIM);
            if (h.hp == 0)
                flags |= NOX_PT_F_HP_ZERO;
        } else {
            /* player_valid=0: mark local-player fields invalid, not stale. */
            h.hp = h.hp_max = h.mp = h.mp_max = 0xFFFF;
            h.facing = h.anim = 0xFF;
        }

        uint32_t now_frame = h.game_frame;
        for (uint32_t i = 0; i < NOX_PT_NUM_ABILITIES; ++i) {
            uint32_t rec = OFF_ABILITY_BASE + 24u * (i + 1u);
            uint32_t id = rd_u32(rec + 0);
            uint32_t level = rd_u32(rec + 16); /* set by opcode 0xCD sub_4611E0; 0 = not learned */
            uint32_t ready = level ? rd_u32(rec + 8) : 0;
            nox_pt_ability_t *ab = &h.abilities[i];
            ab->id = (uint8_t)id;
            ab->level = (uint8_t)(level > 0xFE ? 0xFE : level);
            ab->ready = ready ? 1 : 0;
            ab->active = rd_u32(rec + 12) ? 1 : 0;
            if (id >= 1 && id <= 5) {
                int32_t total = rd_s32(OFF_ABILITY_DELAY + 52u * id);
                ab->cd_total = clamp_u16(total);
                if (level && !ready) {
                    int32_t elapsed = (int32_t)(now_frame - rd_u32(rec + 20));
                    ab->cd_remaining = clamp_u16(total - elapsed);
                }
            }
        }

        bool trunc = false;
        n_obj = export_objects ? collect_objects(self_d, v, g_cfg.max_objects, clip, false, &trunc) : 0;
        if (trunc)
            flags |= NOX_PT_F_OBJ_TRUNCATED;
        uint8_t *o = g_txbuf + sizeof(h);
        for (uint32_t i = 0; i < n_obj; ++i, o += sizeof(nox_pt_obj_t)) {
            uint32_t d = g_cand[i].ptr;
            nox_pt_obj_t ob;
            ob.net_id = (uint16_t)dr_u32(d, DR_NETCODE);
            ob.type_id = (uint16_t)dr_u32(d, DR_TYPE);
            ob.class_flags = dr_u32(d, DR_CLASS);
            ob.x = dr_s32(d, DR_X);
            ob.y = dr_s32(d, DR_Y);
            ob.facing = dr_u8(d, DR_FACING);
            obj_health_class(dr_u32(d, DR_NETCODE), ob.class_flags, &ob);
            memcpy(o, &ob, sizeof(ob));
        }
    }
    h.flags = flags;
    h.n_obj = (uint8_t)n_obj;
    memcpy(g_txbuf, &h, sizeof(h));

    /* v2 names block: local player (if known), then exported player objects. */
    uint8_t *nb = g_txbuf + sizeof(h) + n_obj * sizeof(nox_pt_obj_t);
    uint8_t n_names = 0;
    uint8_t *ne = nb + 2;
    if (netcode && append_name(ne, netcode)) {
        ne += sizeof(nox_pt_name_t);
        ++n_names;
    }
    for (uint32_t i = 0; i < n_obj && n_names < NOX_PT_MAX_NAMES; ++i) {
        uint32_t d = g_cand[i].ptr;
        if (!(dr_u32(d, DR_CLASS) & 4u))
            continue;
        uint32_t nc = dr_u32(d, DR_NETCODE);
        if (nc == netcode)
            continue;
        if (append_name(ne, nc)) {
            ne += sizeof(nox_pt_name_t);
            ++n_names;
        }
    }
    nb[0] = n_names;
    nb[1] = (uint8_t)sizeof(nox_pt_name_t);

    /* v4 ext block: local player (if valid), then each exported object. */
    uint8_t *xb = ne;
    uint8_t n_ext = 0;
    ne = xb + 2;
    if (flags & NOX_PT_F_MAP_LOADED) {
        static pt_prev_t cur[NOX_PT_MAX_OBJECTS + 1u];
        uint32_t n_cur = 0;
        nox_pt_ext_t ex;
        if (self_d) {
            fill_ext(self_d, &ex, h.game_frame, cur, &n_cur);
            memcpy(ne, &ex, sizeof(ex));
            ne += sizeof(ex);
            ++n_ext;
        }
        for (uint32_t i = 0; i < n_obj; ++i) {
            fill_ext(g_cand[i].ptr, &ex, h.game_frame, cur, &n_cur);
            memcpy(ne, &ex, sizeof(ex));
            ne += sizeof(ex);
            ++n_ext;
        }
        memcpy(g_prev, cur, n_cur * sizeof(cur[0]));
        g_n_prev = n_cur;
        g_prev_frame = h.game_frame;
    } else {
        g_n_prev = 0;
    }
    xb[0] = n_ext;
    xb[1] = (uint8_t)sizeof(nox_pt_ext_t);

    /* v5 block: active spell set, then one entry per exported missile. */
    uint8_t *vb = ne;
    uint8_t n_mis = 0;
    ne = vb + 4;
    for (uint32_t i = 0; i < n_obj && n_mis < NOX_PT_MAX_MIS; ++i) {
        uint32_t d = g_cand[i].ptr;
        if (!(dr_u32(d, DR_CLASS) & 1u))
            continue;
        nox_pt_mis_t m;
        memset(&m, 0, sizeof(m));
        uint32_t nc = dr_u32(d, DR_NETCODE);
        m.net_id = (uint16_t)nc;
        m.subclass = dr_u32(d, DR_SUBCLASS);
        const pt_mtrk_t *tr = pt_mtrk_find(g_mtrk, PT_MTRK_SLOTS, nc);
        bool vel = tr && tr->vel;
        uint16_t miss = 0xFFFF, tca = 0xFFFF; /* locals: no pointers into the packed struct (ARM11) */
        if (self_d)
            m.mis_flags = pt_mis_approach(dr_s32(d, DR_X) - v->x, dr_s32(d, DR_Y) - v->y,
                                          vel ? tr->vx16 : 0, vel ? tr->vy16 : 0, vel, &miss, &tca);
        m.miss_px = miss;
        m.tca_f = tca;
        if (vel)
            m.mis_flags |= PT_MISF_TRACKED;
        memcpy(ne, &m, sizeof(m));
        ne += sizeof(m);
        ++n_mis;
    }
    vb[0] = read_spell_set();
    vb[1] = 0;
    vb[2] = n_mis;
    vb[3] = (uint8_t)sizeof(nox_pt_mis_t);

    ne = append_v6(ne, netcode, export_objects);

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(g_cfg.port);
    to.sin_addr.s_addr = g_cfg.host_ip_be;
    size_t len = (size_t)(ne - g_txbuf);
    if (sendto(g_sock, g_txbuf, len, 0, (struct sockaddr *)&to, sizeof(to)) < 0 && !g_send_err_logged) {
        g_send_err_logged = true;
        nox_3ds_net_log("[pt] sendto failed errno=%d (logged once)", errno);
    }
}

/* ---- command input ----------------------------------------------------- */
static void tap_add(tap_kind_t kind, int code, uint8_t delay, uint8_t hold)
{
    for (int i = 0; i < PT_MAX_TAPS; ++i) {
        if (!g_taps[i].used) {
            g_taps[i].used = true;
            g_taps[i].pressed = false;
            g_taps[i].kind = kind;
            g_taps[i].code = code;
            g_taps[i].delay = delay;
            g_taps[i].hold = hold;
            return;
        }
    }
}

static void tap_emit(const pt_tap_t *t, int down)
{
    if (t->kind == TAP_KEY)
        nox_ctrl_inject_key_scancode(t->code, down);
    else
        nox_ctrl_inject_mouse_button(t->code, down);
}

static void taps_release_all(void)
{
    for (int i = 0; i < PT_MAX_TAPS; ++i) {
        if (g_taps[i].used && g_taps[i].pressed)
            tap_emit(&g_taps[i], 0);
        g_taps[i].used = false;
    }
}

static void taps_step(void)
{
    for (int i = 0; i < PT_MAX_TAPS; ++i) {
        pt_tap_t *t = &g_taps[i];
        if (!t->used)
            continue;
        if (t->delay) {
            --t->delay;
        } else if (!t->pressed) {
            tap_emit(t, 1);
            t->pressed = true;
        } else if (t->hold) {
            --t->hold;
        } else {
            tap_emit(t, 0);
            t->used = false;
        }
    }
}

static bool spellset_tap_pending(void)
{
    for (int i = 0; i < PT_MAX_TAPS; ++i)
        if (g_taps[i].used && g_taps[i].kind == TAP_KEY &&
            (g_taps[i].code == SDL_SCANCODE_W || g_taps[i].code == SDL_SCANCODE_E))
            return true;
    return false;
}

static void drop_overrides(void)
{
    g_move_on = false;
    g_aim_on = false;
    nox_3ds_input_set_hook_world_aim(false, 0, 0);
    taps_release_all();
}

static const int k_slot_scancodes[5] = {
    /* Same keys as gamepad.c k_3ds_spell_scancodes / default bindings. */
    SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F, SDL_SCANCODE_G
};

static void apply_cmd(const nox_pt_cmd_t *c, uint32_t now_ms)
{
    uint32_t ttl = c->ttl_ms ? c->ttl_ms : PT_TTL_DEFAULT_MS;
    if (ttl > PT_TTL_MAX_MS) ttl = PT_TTL_MAX_MS;

    switch (c->op) {
    case NOX_PT_OP_MOVE:
        g_move_on = true;
        g_move_orient = (uint8_t)(c->a & 255);
        g_move_cmd = (c->b == 1 || c->b == 3) ? (uint8_t)c->b : 0;
        g_move_until_ms = now_ms + ttl;
        break;
    case NOX_PT_OP_AIM_WORLD:
    case NOX_PT_OP_AIM_DIR:
        g_aim_on = true;
        g_aim_is_dir = (c->op == NOX_PT_OP_AIM_DIR);
        g_aim_a = c->a;
        g_aim_b = c->b;
        g_aim_until_ms = now_ms + ttl;
        break;
    case NOX_PT_OP_ATTACK: {
        int hold = (c->a > 0 && c->a < 60) ? c->a : 2;
        tap_add(TAP_MOUSE, 0, 0, (uint8_t)hold);
        break;
    }
    case NOX_PT_OP_SLOT:
        if (c->a >= 1 && c->a <= 5) {
            if (c->b) {
                /* Mirror gamepad.c: LSHIFT down one frame before the slot key. */
                tap_add(TAP_KEY, SDL_SCANCODE_LSHIFT, 0, 3);
                tap_add(TAP_KEY, k_slot_scancodes[c->a - 1], 1, 1);
            } else {
                tap_add(TAP_KEY, k_slot_scancodes[c->a - 1], 0, 1);
            }
        }
        break;
    case NOX_PT_OP_HEAL:
        tap_add(TAP_KEY, SDL_SCANCODE_X, 0, 1);
        break;
    case NOX_PT_OP_MANA:
        tap_add(TAP_KEY, SDL_SCANCODE_C, 0, 1);
        break;
    case NOX_PT_OP_JUMP:
        tap_add(TAP_MOUSE, 2, 0, 1);
        break;
    case NOX_PT_OP_STOP:
        drop_overrides();
        break;
    case NOX_PT_OP_SPELLSET: {
        /* W / E taps, exactly what ZL / ZR send (gamepad.c IN_L2 / IN_R2;
         * nox.cfg W = PreviousSpellSet, E = NextSpellSet). Ignored while an
         * earlier set change is still being tapped, since the current set is
         * read when the command arrives. */
        if (spellset_tap_pending())
            break;
        bool next;
        int n = pt_spellset_steps(read_spell_set(), c->a, &next);
        for (int k = 0; k < n; ++k)
            tap_add(TAP_KEY, next ? SDL_SCANCODE_E : SDL_SCANCODE_W, (uint8_t)(3 * k), 1);
        break;
    }
    default:
        break;
    }
}

static void drain_cmds(bool accept, uint32_t now_ms)
{
    for (int i = 0; i < PT_MAX_CMDS_PER_FRAME; ++i) {
        uint8_t buf[64];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(g_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n <= 0)
            return; /* EAGAIN / nothing pending */
        if (!accept || n != (ssize_t)sizeof(nox_pt_cmd_t))
            continue;
        if (!g_cfg.accept_any_src && from.sin_addr.s_addr != g_cfg.host_ip_be)
            continue;
        nox_pt_cmd_t c;
        memcpy(&c, buf, sizeof(c));
        if (c.magic != NOX_PT_MAGIC_CMD || c.version != NOX_PT_CMD_VERSION)
            continue;
        apply_cmd(&c, now_ms);
    }
}

static void apply_overrides(const pt_view_t *v, uint32_t now_ms)
{
    if (g_move_on && (int32_t)(now_ms - g_move_until_ms) >= 0)
        g_move_on = false;
    if (g_aim_on && (int32_t)(now_ms - g_aim_until_ms) >= 0)
        g_aim_on = false;

    /* Player screen position (world = cam + screen - vp  =>  screen = world - cam + vp). */
    int psx = v->vp_l + (v->vp_r - v->vp_l) / 2, psy = v->vp_t + (v->vp_b - v->vp_t) / 2;
    if (v->valid) {
        psx = v->x - v->cam_x + v->vp_l;
        psy = v->y - v->cam_y + v->vp_t;
    }

    if (g_move_on) {
        if (g_move_cmd)
            nox_ctrl_set_thumbstick_move(1, g_move_orient, g_move_cmd, 1.0);
        else
            nox_ctrl_set_thumbstick_move(0, 0, 0, 0.0);
    }

    if (g_aim_on) {
        int sx, sy;
        if (g_aim_is_dir) {
            float dx, dy;
            vec_from_orient((uint8_t)(g_aim_a & 255), &dx, &dy);
            sx = psx + (int)lrintf(dx * PT_AIM_DIR_RADIUS);
            sy = psy + (int)lrintf(dy * PT_AIM_DIR_RADIUS);
            sx = pt_aim_clampi(sx, v->vp_l, v->vp_r);
            sy = pt_aim_clampi(sy, v->vp_t, v->vp_b);
        } else {
            pt_aim_world_to_screen(g_aim_a, g_aim_b, v->cam_x, v->cam_y,
                                   v->vp_l, v->vp_t, v->vp_r, v->vp_b, &sx, &sy);
        }
        uint8_t o = orient_from_vec((float)(sx - psx), (float)(sy - psy));
        nox_ctrl_set_radial_aim(1, o, sx, sy);
        /* Queued after input_3ds's own mouse_abs this pump, so it wins. */
        nox_ctrl_inject_mouse_abs(sx, sy);
        /* The client hovers/picks from input_3ds's world-aim point (sub_476FA0,
         * world cursor), not the mouse: drive it too. Held until timeout. */
        nox_3ds_input_set_hook_world_aim(true, sx, sy);
    } else {
        nox_3ds_input_set_hook_world_aim(false, 0, 0);
    }

    taps_step();
}

/* ---- entry points ------------------------------------------------------ */
void nox_pt_frame(void)
{
    if (g_pt_state != PT_ACTIVE) {
        if (g_pt_state == PT_UNINIT)
            pt_init();
        else if (g_pt_state == PT_RETRY &&
                 (int32_t)(nox_3ds_get_ticks_ms() - g_retry_at_ms) >= 0)
            pt_try_open();
        if (g_pt_state != PT_ACTIVE)
            return;
    }
    ++g_tick;
    uint32_t now_ms = nox_3ds_get_ticks_ms();

    uint16_t flags = 0;
    bool menu_shell = rd_u32(OFF_MENU_SHELL) != 0;
    bool map_loaded = !menu_shell && sub_43C700();
    /* Hook-only: also treat an open NPC Dialog.wnd as not in-world. The
     * human pad path is left unchanged (players need A/B etc. in dialogs). */
    bool in_world = map_loaded && nox_3ds_input_is_world_mode() && !npc_dialog_open();
    bool is_mp = session_is_mp();
    /* Session boundary: back to the menu shell after a map, or out of MP. */
    {
        char rowbuf[PT_GATE_ROW_MAX + 1];
        pt_gate_frame(&g_gate, is_mp, menu_shell, map_loaded,
                      sub_40A5C0(1) ? NULL : selected_row_name(rowbuf));
        g_in_session = is_mp && map_loaded;
    }
    const char *srv = is_mp ? current_server_name() : NULL;
    unsigned allow = is_mp ? pt_gate_mp_allow(srv, (const char (*)[PT_GATE_NAME_MAX + 1])g_cfg.allow,
                                              g_cfg.n_allow, g_cfg.allow_any) : 0u;
    bool tag_ok = (allow & PT_ALLOW_TAG) != 0;
    bool allow_ok = (allow & (PT_ALLOW_LIST | PT_ALLOW_WILDCARD)) != 0;
    bool bots_ok = allow != 0;
    if (menu_shell) flags |= NOX_PT_F_MENU_SHELL;
    if (map_loaded) flags |= NOX_PT_F_MAP_LOADED;
    if (in_world)   flags |= NOX_PT_F_IN_WORLD;
    if (is_mp)      flags |= NOX_PT_F_IS_MP;
    if (sub_40A5C0(1)) flags |= NOX_PT_F_IS_HOST;
    if (tag_ok)     flags |= NOX_PT_F_BOTS_OK;
    if (allow_ok)   flags |= NOX_PT_F_ALLOWLISTED;
    if (allow & PT_ALLOW_WILDCARD) flags |= NOX_PT_F_WILDCARD;

    pt_view_t v;
    memset(&v, 0, sizeof(v));
    uint32_t netcode = rd_u32(OFF_LOCAL_NETCODE);
    uint32_t self_d = 0;
    if (map_loaded) {
        read_view(&v);
        self_d = find_local_drawable(netcode);
        if (self_d) {
            v.valid = true;
            v.x = dr_s32(self_d, DR_X);
            v.y = dr_s32(self_d, DR_Y);
            flags |= NOX_PT_F_PLAYER_VALID;
        }
    }

    /* MP gating: commands only if the server opted in via "[bots-ok]". */
    bool cmds_active = g_cfg.commands && in_world && self_d && (!is_mp || bots_ok);
    drain_cmds(cmds_active, now_ms);
    if (cmds_active) {
        flags |= NOX_PT_F_CMDS_ACTIVE;
        apply_overrides(&v, now_ms);
    } else if (g_was_cmds_active) {
        drop_overrides();
    }
    g_was_cmds_active = cmds_active;

    bool export_objects = !is_mp || bots_ok;
    bool clip = is_mp || g_cfg.clip_viewport;
    /* v6: events about other objects follow the same gate as the object export. */
    g_ev_others_ok = export_objects;
    {
        ev_init_once();
        pt_trie_t t;
        trie_get(&t);
        pt_chant_tick(g_chant, &t, &g_ev, rd_u32(OFF_CLIENT_FRAME));
    }
    if (!map_loaded)
        pt_mtrk_reset(g_mtrk, PT_MTRK_SLOTS);
    if ((g_tick % g_cfg.every) == 0) {
        /* MP without opt-in: header + local player only. In MP the viewport
         * and visibility clip is forced on regardless of cfg. */
        publish_state(flags, netcode, self_d, &v, export_objects, clip);
    } else if (map_loaded && export_objects && g_cfg.max_objects) {
        /* v5: sample visible missiles between publishes for their velocity. */
        bool unused;
        collect_objects(self_d, &v, 0, clip, true, &unused);
    }
}

void nox_pt_shutdown(void)
{
    if (g_pt_state == PT_ACTIVE)
        taps_release_all();
    nox_3ds_input_set_hook_world_aim(false, 0, 0);
    close_sock();
    g_pt_state = PT_DISABLED;
    pt_gate_reset(&g_gate);
}
