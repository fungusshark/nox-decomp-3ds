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
extern int   sub_43C700(void);          /* map loaded */
extern int   sub_40A5C0(int a1);        /* game flags test; 1 = hosting server */
extern char *sub_40A4C0(void);          /* host's server name (byte_5D4594[1324]) */
extern char *sub_437250(void);          /* gameplay viewport rect [l,t,r,b] */
extern int   sub_4984B0(int a1);        /* client draw-visibility test (view polygon/LOS), read-only */
extern int   sub_46ACC0(int a1);        /* GUI window hidden check: 1 = hidden */

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

/* playerInfo fields */
#define PI_CLASS              2251u     /* u8, GAME2.c:2214 */
#define PI_FLAGS              3680u     /* u8, bit0 observer (GAME2.c:29138) */

/* drawable fields */
#define DR_X                  12u
#define DR_Y                  16u
#define DR_TYPE               108u
#define DR_CLASS              112u
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
#define PT_MAX_WALK           2048      /* exported object list walk cap */
#define PT_MAX_PLAYER_WALK    256       /* player-drawable list walk cap */
#define PT_TTL_DEFAULT_MS     250u
#define PT_TTL_MAX_MS         2000u
#define PT_AIM_DIR_RADIUS     64.0f
#define PT_BOTS_OK_TAG        "[bots-ok]"
#define PT_RETRY_MS           10000u

enum { PT_UNINIT = 0, PT_DISABLED, PT_RETRY, PT_ACTIVE };

typedef struct {
    uint32_t host_ip_be;
    uint16_t port;
    uint16_t listen_port;
    uint32_t every;          /* publish every N calls */
    uint32_t max_objects;
    bool     commands;
    bool     clip_viewport;
    bool     accept_any_src;
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
static char       g_srv_name[16];
static bool       g_srv_name_valid;
static bool       g_was_map_loaded;

/* Static send buffer: header + max objects. No per-frame allocation. */
static uint8_t    g_txbuf[sizeof(nox_pt_state_hdr_t) + NOX_PT_MAX_OBJECTS * sizeof(nox_pt_obj_t)];

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

static bool ci_contains(const char *s, size_t max_len, const char *needle)
{
    size_t nl = strlen(needle);
    size_t sl = strnlen(s, max_len);
    if (nl == 0 || sl < nl)
        return false;
    for (size_t i = 0; i + nl <= sl; ++i) {
        size_t j = 0;
        while (j < nl) {
            char a = s[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            ++j;
        }
        if (j == nl)
            return true;
    }
    return false;
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
    g_cfg.every = 2;
    g_cfg.max_objects = NOX_PT_MAX_OBJECTS;
    g_cfg.commands = true;
    g_cfg.clip_viewport = true;
    bool have_host = false;

    char line[128];
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
        } else if (!strcmp(k, "every")) {
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
    size_t n = strnlen(name, 15);
    memcpy(g_srv_name, name, n);
    g_srv_name[n] = '\0';
    g_srv_name_valid = true;
}

static bool server_bots_ok(void)
{
    /* Host: its own configured server name (always current).
     * Client: only the snapshot from a 0xB0 received this session; the engine
     * copy at byte_5D4594[371389] survives across sessions, so it is not used. */
    if (sub_40A5C0(1)) {
        const char *name = sub_40A4C0();
        return name && ci_contains(name, 15, PT_BOTS_OK_TAG);
    }
    return g_srv_name_valid && ci_contains(g_srv_name, 15, PT_BOTS_OK_TAG);
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

static uint32_t collect_objects(uint32_t self_d, const pt_view_t *v, uint32_t max, bool clip,
                                bool *truncated)
{
    uint32_t count = 0, worst = 0;
    *truncated = false;
    if (max == 0)
        return 0;

    int32_t wx0 = v->cam_x, wy0 = v->cam_y;
    int32_t wx1 = wx0 + (v->vp_r - v->vp_l), wy1 = wy0 + (v->vp_b - v->vp_t);

    bool infravision = self_d && (dr_u32(self_d, DR_ENCHANT) & ENCH_INFRAVISION_BIT);

    uint32_t d = rd_u32(OFF_DRAWABLE_LIST);
    int n = 0;
    for (; d && n < PT_MAX_WALK; ++n, d = dr_u32(d, DR_NEXT)) {
        if (d == self_d)
            continue;
        uint32_t cls = dr_u32(d, DR_CLASS);
        if (cls & DR_CLASS_NONNET)
            continue;
        int32_t x = dr_s32(d, DR_X), y = dr_s32(d, DR_Y);
        if (clip && (x < wx0 || x > wx1 || y < wy0 || y > wy1))
            continue;
        /* Invisible objects: the renderer hides them (GAME2.c:30355) unless
         * allied; we drop them unless the local player has infravision. Allies
         * are dropped too (conservative; ally test needs sub_418C80). */
        if (!infravision && (dr_u32(d, DR_ENCHANT) & ENCH_INVISIBLE_BIT))
            continue;
        /* Same draw-visibility test the renderer uses before drawing
         * (GAME2.c:30306): view polygon / line of sight from the last frame. */
        if (clip && !sub_4984B0((int)d))
            continue;

        uint32_t tier = (cls & 4u) ? 0u : (cls & 2u) ? 1u : (cls & 1u) ? 2u : 3u;
        int32_t dx = v->valid ? x - v->x : 0, dy = v->valid ? y - v->y : 0;
        int64_t dd = (int64_t)dx * dx + (int64_t)dy * dy;
        uint32_t d2 = dd > 0xFFFFFF ? 0xFFFFFFu : (uint32_t)dd;
        uint32_t key = (tier << 24) | d2;

        if (count < max) {
            g_cand[count].key = key;
            g_cand[count].ptr = d;
            if (count == 0 || key > g_cand[worst].key) worst = count;
            ++count;
        } else {
            *truncated = true;
            if (key < g_cand[worst].key) {
                g_cand[worst].key = key;
                g_cand[worst].ptr = d;
                worst = 0;
                for (uint32_t i = 1; i < count; ++i)
                    if (g_cand[i].key > g_cand[worst].key) worst = i;
            }
        }
    }
    if (d && n >= PT_MAX_WALK)
        *truncated = true;
    /* insertion sort: nearest players first */
    for (uint32_t i = 1; i < count; ++i) {
        pt_cand_t c = g_cand[i];
        uint32_t j = i;
        while (j > 0 && g_cand[j - 1].key > c.key) { g_cand[j] = g_cand[j - 1]; --j; }
        g_cand[j] = c;
    }
    return count;
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
        n_obj = export_objects ? collect_objects(self_d, v, g_cfg.max_objects, clip, &trunc) : 0;
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
            ob.hp_pct = 0xFF; /* TODO(#36): ally HP% (0x41/0x42 -> sub_495150) */
            ob.reserved = 0;
            memcpy(o, &ob, sizeof(ob));
        }
    }
    h.flags = flags;
    h.n_obj = (uint8_t)n_obj;
    memcpy(g_txbuf, &h, sizeof(h));

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(g_cfg.port);
    to.sin_addr.s_addr = g_cfg.host_ip_be;
    size_t len = sizeof(h) + n_obj * sizeof(nox_pt_obj_t);
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

static void drop_overrides(void)
{
    g_move_on = false;
    g_aim_on = false;
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
        if (c.magic != NOX_PT_MAGIC_CMD || c.version != NOX_PT_VERSION)
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
        } else {
            sx = g_aim_a - v->cam_x + v->vp_l;
            sy = g_aim_b - v->cam_y + v->vp_t;
        }
        if (sx < v->vp_l) sx = v->vp_l;
        if (sx > v->vp_r) sx = v->vp_r;
        if (sy < v->vp_t) sy = v->vp_t;
        if (sy > v->vp_b) sy = v->vp_b;
        uint8_t o = orient_from_vec((float)(sx - psx), (float)(sy - psy));
        nox_ctrl_set_radial_aim(1, o, sx, sy);
        /* Queued after input_3ds's own mouse_abs this pump, so it wins. */
        nox_ctrl_inject_mouse_abs(sx, sy);
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
    if ((menu_shell && g_was_map_loaded) || !is_mp)
        g_srv_name_valid = false;
    g_was_map_loaded = map_loaded;
    bool bots_ok = is_mp && server_bots_ok();
    if (menu_shell) flags |= NOX_PT_F_MENU_SHELL;
    if (map_loaded) flags |= NOX_PT_F_MAP_LOADED;
    if (in_world)   flags |= NOX_PT_F_IN_WORLD;
    if (is_mp)      flags |= NOX_PT_F_IS_MP;
    if (sub_40A5C0(1)) flags |= NOX_PT_F_IS_HOST;
    if (bots_ok)    flags |= NOX_PT_F_BOTS_OK;

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

    if ((g_tick % g_cfg.every) == 0)
        /* MP without opt-in: header + local player only. In MP the viewport
         * and visibility clip is forced on regardless of cfg. */
        publish_state(flags, netcode, self_d, &v, !is_mp || bots_ok, is_mp || g_cfg.clip_viewport);
}

void nox_pt_shutdown(void)
{
    if (g_pt_state == PT_ACTIVE)
        taps_release_all();
    close_sock();
    g_pt_state = PT_DISABLED;
    g_srv_name_valid = false;
}
