/*
 * Phase 2: OpenNox HTTP Lobby Integration for Nintendo 3DS.
 *
 * Fetches active internet Nox servers asynchronously via Horizon OS's native
 * http:C service on a dedicated low-priority worker thread (32 KB stack),
 * parses the JSON payload with pure-C cJSON, and injects synthetic 1999 MSVC
 * 0x0D server-info packets into GAME5.c:sub_554D70() via the engine's existing
 * nox_netextras_* hooks.
 *
 * Solo Campaign and Solo Quest remain strictly offline (zero httpcInit(), zero
 * worker thread spawned).
 */
#include "platform_3ds.h"
#include "third_party/cJSON.h"
#include "../../src/netextras.h"
#include "../../src/netextras_types.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

/* Externs from engine / port */
extern unsigned int sub_554040(int a1, int a2, char *a3);
extern int nox_env_truthy(const char *s);

#define NOX_3DS_LOBBY_MAX_SERVERS       32
#define NOX_3DS_LOBBY_FAKE_QUEUE_CAP    32
#define NOX_3DS_LOBBY_JSON_CAP          (64u * 1024u)
#define NOX_3DS_LOBBY_WORKER_STACK      (32u * 1024u)
#define NOX_3DS_LOBBY_CACHE_TTL_MS      30000u
#define NOX_3DS_LOBBY_RETRY_BACKOFF_MS  10000u
#define NOX_3DS_LOBBY_HTTP_TIMEOUT_NS   3000000000ULL /* 3 seconds */
#define NOX_3DS_LOBBY_MAX_REDIRECTS     4
#define NOX_3DS_LOBBY_DEFAULT_PORT      18590u
#define NOX_3DS_LOBBY_REG_PERIOD_MS     20000u
#define NOX_3DS_LOBBY_MAX_ENDPOINTS     8

/* Default endpoints matching desktop src/lobby.c */
static const char *const k_default_lobby_endpoints[] = {
    "nox.nwca.xyz:8088",
    "noxdecomp.qzz.io",
    "noxdecomp2.qzz.io",
};

typedef struct {
    char base_url[256]; /* e.g. "http://nox.nwca.xyz:8088" or "https://..." */
} lobby_endpoint_t;

typedef struct {
    bool used;
    int sockfd;
    size_t len;
    uint8_t data[256];
    struct sockaddr_in from;
} fake_srv_pkt_t;

typedef struct {
    bool pending;
    char name[64];
    char map[32];
    unsigned cur;
    unsigned max;
    unsigned port;
    uint16_t flags;
} host_reg_job_t;

/* Synchronization & worker lifecycle */
static bool g_lobby_inited = false;
static RecursiveLock g_lobby_lock;
static LightEvent g_lobby_event;
static Thread g_worker_thread = NULL;
static bool g_worker_started = false;
static volatile bool g_worker_stop = false;
static bool g_httpc_inited = false;

typedef struct {
    nox_server_row row;
    struct in_addr resolved_ip;
} lobby_cached_row_t;

/* Discovery fetch state & server cache */
static bool g_fetch_requested = false;
static bool g_fetch_inflight = false;
static bool g_cache_valid = false;
static uint32_t g_cache_last_ok_ms = 0;
static uint32_t g_cache_next_try_ms = 0;
static lobby_cached_row_t g_srv_cache[NOX_3DS_LOBBY_MAX_SERVERS];
static size_t g_srv_cache_count = 0;
static char g_last_good_base_url[256];

/* Active discovery ping state (used to stamp matching RTT cookie into 0x0D) */
static bool g_active_ping_valid = false;
static int g_active_ping_sockfd = -1;
static uint8_t g_active_ping_cookie[4];

/* Fake 0x0D packet queue drained by GAME5.c:sub_554D70() */
static fake_srv_pkt_t g_fake_packets[NOX_3DS_LOBBY_FAKE_QUEUE_CAP];

/* Cached serverinfo flags & optional host registration state */
static uint16_t g_last_serverinfo_flags = 0;
static bool g_host_active = false;
static unsigned g_host_bound_port = NOX_3DS_LOBBY_DEFAULT_PORT;
static uint32_t g_host_last_snapshot_ms = 0;
static uint32_t g_host_last_reg_ms = 0;
static host_reg_job_t g_host_reg_job;

/* =========================================================================
 * Mode Flag Helpers (matching opennox / src/lobby.c)
 * ========================================================================= */
static const char *lobby_mode_normalize(const char *mode)
{
    if (!mode || !*mode) return NULL;
    while (*mode && (unsigned char)*mode <= ' ') mode++;
    if (!*mode) return NULL;

    if (!strcasecmp(mode, "capflag"))     return "ctf";
    if (!strcasecmp(mode, "kotr"))        return "kotr";
    if (!strcasecmp(mode, "ctf"))         return "ctf";
    if (!strcasecmp(mode, "flagball"))    return "flagball";
    if (!strcasecmp(mode, "chat"))        return "chat";
    if (!strcasecmp(mode, "arena"))       return "arena";
    if (!strcasecmp(mode, "elim"))        return "elimination";
    if (!strcasecmp(mode, "elimination")) return "elimination";
    if (!strcasecmp(mode, "quest"))       return "quest";
    if (!strcasecmp(mode, "coop"))        return "coop";
    if (!strcasecmp(mode, "coopteam"))    return "coopteam";
    if (!strcasecmp(mode, "custom"))      return "custom";
    return mode;
}

static uint16_t lobby_mode_to_flagbit(const char *mode)
{
    mode = lobby_mode_normalize(mode);
    if (!mode) return 0;

    if (!strcasecmp(mode, "kotr"))        return NOX_GF_MODE_KOTR;
    if (!strcasecmp(mode, "ctf"))         return NOX_GF_MODE_CTF;
    if (!strcasecmp(mode, "flagball"))    return NOX_GF_MODE_FLAGBALL;
    if (!strcasecmp(mode, "chat"))        return NOX_GF_MODE_CHAT;
    if (!strcasecmp(mode, "arena"))       return NOX_GF_MODE_ARENA;
    if (!strcasecmp(mode, "elimination")) return NOX_GF_MODE_ELIMINATION;
    if (!strcasecmp(mode, "quest"))       return NOX_GF_MODE_QUEST;
    if (!strcasecmp(mode, "coop"))        return NOX_GF_MODE_COOP;
    if (!strcasecmp(mode, "coopteam"))    return NOX_GF_MODE_COOPTEAM;
    return 0;
}

static const char *lobby_mode_from_flags(uint16_t flags)
{
    if (flags & NOX_GF_MODE_KOTR)        return "kotr";
    if (flags & NOX_GF_MODE_CTF)         return "ctf";
    if (flags & NOX_GF_MODE_FLAGBALL)    return "flagball";
    if (flags & NOX_GF_MODE_CHAT)        return "chat";
    if (flags & NOX_GF_MODE_ARENA)       return "arena";
    if (flags & NOX_GF_MODE_ELIMINATION) return "elimination";
    if (flags & NOX_GF_MODE_QUEST)       return "quest";
    if (flags & NOX_GF_MODE_COOP)        return "coop";
    if (flags & NOX_GF_MODE_COOPTEAM)    return "coopteam";
    return "chat";
}

/* =========================================================================
 * Configuration Helpers (SDMC optional overrides + defaults)
 * ========================================================================= */
static bool lobby_endpoint_normalize(const char *raw, char *out_base, size_t out_sz)
{
    if (!raw || !out_base || out_sz < 16)
        return false;

    while (*raw && (unsigned char)*raw <= ' ') raw++;
    size_t len = strlen(raw);
    while (len > 0 && (unsigned char)raw[len - 1] <= ' ') len--;
    if (len == 0)
        return false;

    char tmp[256];
    if (len >= sizeof(tmp))
        len = sizeof(tmp) - 1u;
    memcpy(tmp, raw, len);
    tmp[len] = '\0';

    /* Strip trailing slash */
    while (len > 0 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }
    if (len == 0)
        return false;

    /* If user wrote full URL with /api/v0/games/list, strip the path suffix
     * so we hold a clean base URL for both list and register endpoints. */
    const char *api_pos = strstr(tmp, "/api/v0/games/list");
    if (api_pos) {
        size_t base_len = (size_t)(api_pos - tmp);
        tmp[base_len] = '\0';
        len = base_len;
    }

    if (strncasecmp(tmp, "http://", 7) == 0 || strncasecmp(tmp, "https://", 8) == 0) {
        snprintf(out_base, out_sz, "%s", tmp);
    } else {
        snprintf(out_base, out_sz, "http://%s", tmp);
    }
    return true;
}

static size_t lobby_load_endpoints(lobby_endpoint_t *eps, size_t max_eps, bool *out_disabled)
{
    if (out_disabled)
        *out_disabled = false;

    const char *no_inet = getenv("NOX_NO_INTERNET_SERVERS");
    if (no_inet && nox_env_truthy(no_inet)) {
        if (out_disabled) *out_disabled = true;
        return 0;
    }
    const char *inj = getenv("NOX_INJECT_INTERNET_SERVERS");
    if (inj && strcmp(inj, "0") == 0) {
        if (out_disabled) *out_disabled = true;
        return 0;
    }

    size_t count = 0;

    /* Optional SD card config: sdmc:/3ds/nox/lobby.txt
     * One endpoint per line: "nox.nwca.xyz:8088", "https://...", or "disabled". */
    FILE *f = fopen(NOX_3DS_DATA_ROOT "/lobby.txt", "r");
    if (f) {
        char line[256];
        while (count < max_eps && fgets(line, sizeof(line), f)) {
            char *p = line;
            while (*p && (unsigned char)*p <= ' ') p++;
            if (*p == '\0' || *p == '#')
                continue;
            char *end = p + strlen(p);
            while (end > p && (unsigned char)end[-1] <= ' ')
                *--end = '\0';
            if (strcasecmp(p, "disabled") == 0 || strcasecmp(p, "off") == 0 || strcmp(p, "0") == 0) {
                if (out_disabled) *out_disabled = true;
                fclose(f);
                return 0;
            }
            if (lobby_endpoint_normalize(p, eps[count].base_url, sizeof(eps[count].base_url))) {
                count++;
            }
        }
        fclose(f);
        if (count > 0)
            return count;
    }

    for (size_t i = 0; i < sizeof(k_default_lobby_endpoints) / sizeof(k_default_lobby_endpoints[0]) && count < max_eps; ++i) {
        if (lobby_endpoint_normalize(k_default_lobby_endpoints[i], eps[count].base_url, sizeof(eps[count].base_url))) {
            count++;
        }
    }
    return count;
}

static bool lobby_should_register_host(void)
{
    static int s_cached = -1;
    if (s_cached >= 0)
        return s_cached != 0;

    if (nox_env_truthy(getenv("NOX_LOBBY_REGISTER_ENABLE"))) {
        s_cached = 1;
        return true;
    }

    /* Opt-in via presence of sdmc:/3ds/nox/lobby_host.txt (OFF by default). */
    FILE *f = fopen(NOX_3DS_DATA_ROOT "/lobby_host.txt", "r");
    if (f) {
        char buf[32] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        s_cached = (n == 0 || nox_env_truthy(buf) != 0) ? 1 : 0;
        return s_cached != 0;
    }
    s_cached = 0;
    return false;
}

/* =========================================================================
 * Pure-C cJSON Parser -> nox_server_row[]
 * ========================================================================= */
static void copy_json_str(const cJSON *obj, const char *key, char *dst, size_t dst_sz)
{
    if (!dst || dst_sz == 0) return;
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it)) {
        snprintf(dst, dst_sz, "%s", it->valuestring);
    }
}

static size_t lobby_parse_games_json(const char *json_text, nox_server_row *out, size_t out_cap)
{
    if (!json_text || !out || out_cap == 0)
        return 0;

    cJSON *root = cJSON_Parse(json_text);
    if (!root)
        return 0;

    /* Support both OpenNox {"data":[...]} and bare top-level [...] array. */
    const cJSON *arr = NULL;
    if (cJSON_IsArray(root)) {
        arr = root;
    } else if (cJSON_IsObject(root)) {
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (cJSON_IsArray(data))
            arr = data;
    }

    size_t count = 0;
    if (arr) {
        const cJSON *elem = NULL;
        cJSON_ArrayForEach(elem, arr) {
            if (count >= out_cap)
                break;
            if (!cJSON_IsObject(elem))
                continue;

            nox_server_row row;
            memset(&row, 0, sizeof(row));
            row.port = (uint16_t)NOX_3DS_LOBBY_DEFAULT_PORT;
            row.players_max = 31;

            copy_json_str(elem, "name", row.name, sizeof(row.name));
            copy_json_str(elem, "addr", row.addr, sizeof(row.addr));
            if (row.addr[0] == '\0')
                copy_json_str(elem, "ip", row.addr, sizeof(row.addr));
            copy_json_str(elem, "map", row.map, sizeof(row.map));
            copy_json_str(elem, "mode", row.mode, sizeof(row.mode));

            const cJSON *jport = cJSON_GetObjectItemCaseSensitive(elem, "port");
            if (cJSON_IsNumber(jport) && jport->valueint > 0 && jport->valueint <= 65535) {
                row.port = (uint16_t)jport->valueint;
            }

            const cJSON *jplayers = cJSON_GetObjectItemCaseSensitive(elem, "players");
            if (cJSON_IsObject(jplayers)) {
                const cJSON *jcur = cJSON_GetObjectItemCaseSensitive(jplayers, "cur");
                const cJSON *jmax = cJSON_GetObjectItemCaseSensitive(jplayers, "max");
                if (cJSON_IsNumber(jcur) && jcur->valueint >= 0) {
                    row.players_cur = (uint8_t)(jcur->valueint > 255 ? 255 : jcur->valueint);
                }
                if (cJSON_IsNumber(jmax) && jmax->valueint > 0) {
                    row.players_max = (uint8_t)(jmax->valueint > 255 ? 255 : jmax->valueint);
                }
            } else if (cJSON_IsNumber(jplayers) && jplayers->valueint >= 0) {
                row.players_cur = (uint8_t)(jplayers->valueint > 255 ? 255 : jplayers->valueint);
                const cJSON *jmaxp = cJSON_GetObjectItemCaseSensitive(elem, "max_players");
                if (cJSON_IsNumber(jmaxp) && jmaxp->valueint > 0) {
                    row.players_max = (uint8_t)(jmaxp->valueint > 255 ? 255 : jmaxp->valueint);
                }
            }

            if (row.addr[0] == '\0')
                continue;

            out[count++] = row;
        }
    }

    cJSON_Delete(root);
    return count;
}

/* =========================================================================
 * IPv4 Resolution & Synthetic 1999 MSVC 0x0D Server-Info Packet Builder
 * ========================================================================= */
static bool resolve_ipv4_addr(const char *host_or_ip, struct in_addr *out_addr)
{
    if (!host_or_ip || !*host_or_ip || !out_addr)
        return false;

    /* Fast path: dotted-quad IPv4 literal (no DNS needed) */
    bool digits_only = true;
    for (const char *p = host_or_ip; *p; ++p) {
        if (!((*p >= '0' && *p <= '9') || *p == '.')) {
            digits_only = false;
            break;
        }
    }
    if (digits_only) {
        in_addr_t ip = inet_addr(host_or_ip);
        if (ip == INADDR_NONE && strcmp(host_or_ip, "255.255.255.255") != 0)
            return false;
        out_addr->s_addr = ip;
        return true;
    }

    /* Fallback: DNS lookup via libctru soc:u if socInit() is active */
    if (!nox_3ds_soc_is_ready())
        return false;

    struct hostent *he = gethostbyname(host_or_ip);
    if (!he || !he->h_addr_list || !he->h_addr_list[0])
        return false;

    memcpy(&out_addr->s_addr, he->h_addr_list[0], sizeof(out_addr->s_addr));
    return true;
}

static void flush_fake_packets_locked(int keep_sockfd)
{
    for (int i = 0; i < NOX_3DS_LOBBY_FAKE_QUEUE_CAP; ++i) {
        if (g_fake_packets[i].used && (keep_sockfd < 0 || g_fake_packets[i].sockfd != keep_sockfd)) {
            g_fake_packets[i].used = false;
        }
    }
}

static fake_srv_pkt_t *alloc_fake_slot_locked(void)
{
    for (int i = 0; i < NOX_3DS_LOBBY_FAKE_QUEUE_CAP; ++i) {
        if (!g_fake_packets[i].used) {
            memset(&g_fake_packets[i], 0, sizeof(g_fake_packets[i]));
            g_fake_packets[i].used = true;
            g_fake_packets[i].sockfd = -1;
            return &g_fake_packets[i];
        }
    }
    return NULL;
}

static size_t queue_cached_rows_locked(void)
{
    if (!g_active_ping_valid || g_active_ping_sockfd < 0 || g_srv_cache_count == 0)
        return 0;

    /* Replace any prior queued entries for this ping cycle so every cached
     * server is stamped with the fresh ping cookie (byte_5D4594[814964]). */
    flush_fake_packets_locked(-1);

    size_t queued = 0;
    for (size_t i = 0; i < g_srv_cache_count; ++i) {
        const nox_server_row *row = &g_srv_cache[i].row;
        struct in_addr srv_ip = g_srv_cache[i].resolved_ip;

        fake_srv_pkt_t *slot = alloc_fake_slot_locked();
        if (!slot)
            break;

        uint8_t *out = slot->data;
        memset(out, 0, sizeof(slot->data));

        /* Exact 1999 MSVC 0x0D server-info layout (see GAME5.c:sub_554040 & GAME1.c:sub_4375F0) */
        out[0] = 0x00;
        out[1] = 0x00;
        out[2] = 0x0D;
        out[3] = row->players_cur;
        out[4] = row->players_max ? row->players_max : 31;
        out[5] = 0x0F;
        out[6] = 0x0F;

        /* Session ID placeholder @ offset 7..10 (written before map @ 10, matching sub_554040) */
        {
            uint32_t sid = 0xFFFFFFFFu;
            memcpy(&out[7], &sid, sizeof(sid));
        }

        /* Map name @ offset 10..18: clamped to 8 chars + NUL so out[19..27] and
         * GAME1.c:sub_4375F0's 9-byte map slot (v13[111..119]) never overflow. */
        if (row->map[0] != '\0') {
            size_t mlen = strnlen(row->map, 8);
            memcpy(&out[10], row->map, mlen);
            out[10 + mlen] = '\0';
        }

        /* Game flags @ offset 28..29 (little-endian) */
        uint16_t base = g_last_serverinfo_flags ? g_last_serverinfo_flags : 0x2007u;
        uint16_t modebit = lobby_mode_to_flagbit(row->mode);
        uint16_t flags = (uint16_t)((base & (uint16_t)~NOX_MODE_MASK) | modebit);
        out[28] = (uint8_t)(flags & 0xFFu);
        out[29] = (uint8_t)((flags >> 8) & 0xFFu);

        /* Echo 4-byte ping cookie @ offset 44..47 so sub_4375F0 accepts the reply */
        memcpy(&out[44], g_active_ping_cookie, 4);

        /* Server name @ offset 72: clamped to 31 chars + NUL because
         * GAME1.c:sub_4375F0 copies into `char v14[32]` via strcpy(). */
        size_t name_len = 0;
        if (row->name[0] != '\0') {
            name_len = strnlen(row->name, 31);
            memcpy(&out[72], row->name, name_len);
            out[72 + name_len] = '\0';
        }

        size_t total_len = 72u + name_len + 1u;
        if (total_len < 136u)
            total_len = 136u;
        if (total_len > sizeof(slot->data))
            total_len = sizeof(slot->data);
        slot->len = total_len;

        slot->from.sin_family = AF_INET;
        slot->from.sin_port = htons(row->port ? row->port : (uint16_t)NOX_3DS_LOBBY_DEFAULT_PORT);
        slot->from.sin_addr = srv_ip;
        slot->sockfd = g_active_ping_sockfd;
        slot->used = true;
        queued++;
    }

    if (queued > 0) {
        printf("[lobby3ds] Queued %zu internet server(s) into 0x0D UI bridge (fd=%d)\n",
               queued, g_active_ping_sockfd);
    }
    return queued;
}

/* =========================================================================
 * 3DS http:C Transport (Lazy Init, TLS DisableVerify, Redirects, Timeouts)
 * ========================================================================= */
static bool httpc_ensure_init(void)
{
    if (g_httpc_inited)
        return true;

    /* Check Wi-Fi radio before touching http:C */
    u32 wifi_status = 0;
    if (R_SUCCEEDED(acInit())) {
        ACU_GetWifiStatus(&wifi_status);
        acExit();
    }
    if (wifi_status == 0) {
        printf("[lobby3ds] Wi-Fi offline (Airplane Mode); skipping httpcInit\n");
        return false;
    }

    /* 4 KB shared memory supports both HTTP GET and HTTP POST (Phase 2b). */
    Result rc = httpcInit(0x1000);
    if (R_FAILED(rc)) {
        printf("[lobby3ds] httpcInit(0x1000) failed: 0x%08lX\n", (unsigned long)rc);
        return false;
    }

    g_httpc_inited = true;
    nox_3ds_state_mut()->httpc_ready = true;
    printf("[lobby3ds] httpcInit(0x1000) succeeded\n");
    return true;
}

static bool build_redirect_url(const char *current_url, const char *location, char *out_url, size_t out_sz)
{
    if (!location || !*location || !out_url || out_sz < 16)
        return false;

    if (strncasecmp(location, "http://", 7) == 0 || strncasecmp(location, "https://", 8) == 0) {
        snprintf(out_url, out_sz, "%s", location);
        return true;
    }

    /* Root-relative redirect ("/api/...") -> reuse scheme://host[:port] */
    if (location[0] == '/' && current_url) {
        const char *scheme_end = strstr(current_url, "://");
        if (!scheme_end)
            return false;
        const char *path_start = strchr(scheme_end + 3, '/');
        size_t origin_len = path_start ? (size_t)(path_start - current_url) : strlen(current_url);
        if (origin_len + strlen(location) + 1u >= out_sz)
            return false;
        memcpy(out_url, current_url, origin_len);
        snprintf(out_url + origin_len, out_sz - origin_len, "%s", location);
        return true;
    }

    return false;
}

static int httpc_get_body(const char *initial_url, char *out_buf, size_t out_cap)
{
    if (!initial_url || !out_buf || out_cap < 2)
        return -1;
    out_buf[0] = '\0';

    if (!httpc_ensure_init())
        return -1;

    char url[512];
    snprintf(url, sizeof(url), "%s", initial_url);

    for (int hop = 0; hop <= NOX_3DS_LOBBY_MAX_REDIRECTS && !g_worker_stop; ++hop) {
        httpcContext ctx;
        memset(&ctx, 0, sizeof(ctx));

        Result rc = httpcOpenContext(&ctx, HTTPC_METHOD_GET, url, 0);
        if (R_FAILED(rc)) {
            printf("[lobby3ds] httpcOpenContext(%s) failed: 0x%08lX\n", url, (unsigned long)rc);
            return -1;
        }

        /* Allow HTTPS against modern CAs not present in 3DS's 2016 root store */
        httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
        httpcSetKeepAlive(&ctx, HTTPC_KEEPALIVE_DISABLED);
        httpcAddRequestHeaderField(&ctx, "User-Agent", "nox-decomp-3ds/1");
        httpcAddRequestHeaderField(&ctx, "Accept", "application/json");

        rc = httpcBeginRequest(&ctx);
        if (R_FAILED(rc)) {
            printf("[lobby3ds] httpcBeginRequest(%s) failed: 0x%08lX\n", url, (unsigned long)rc);
            httpcCancelConnection(&ctx);
            httpcCloseContext(&ctx);
            return -1;
        }

        u32 status = 0;
        rc = httpcGetResponseStatusCodeTimeout(&ctx, &status, NOX_3DS_LOBBY_HTTP_TIMEOUT_NS);
        if (R_FAILED(rc)) {
            printf("[lobby3ds] Status timeout/error for %s: 0x%08lX\n", url, (unsigned long)rc);
            httpcCancelConnection(&ctx);
            httpcCloseContext(&ctx);
            return -1;
        }

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            char location[512];
            memset(location, 0, sizeof(location));
            Result hrc = httpcGetResponseHeader(&ctx, "Location", location, sizeof(location));
            httpcCancelConnection(&ctx);
            httpcCloseContext(&ctx);

            if (R_FAILED(hrc) || location[0] == '\0')
                return -1;

            char next_url[512];
            if (!build_redirect_url(url, location, next_url, sizeof(next_url)))
                return -1;

            snprintf(url, sizeof(url), "%s", next_url);
            continue;
        }

        if (status < 200 || status >= 300) {
            printf("[lobby3ds] HTTP %lu from %s\n", (unsigned long)status, url);
            httpcCancelConnection(&ctx);
            httpcCloseContext(&ctx);
            return -1;
        }

        /* Stream response in 4 KB chunks (works with both Content-Length and chunked). */
        u32 total = 0;
        u32 prev_dl = 0;
        bool aborted = false;

        do {
            u32 avail = (u32)(out_cap - 1u) - total;
            if (avail == 0) {
                aborted = true;
                break;
            }
            u32 chunk = (avail > 4096u) ? 4096u : avail;
            rc = httpcReceiveDataTimeout(&ctx, (u8 *)(out_buf + total), chunk,
                                         NOX_3DS_LOBBY_HTTP_TIMEOUT_NS);

            u32 dl_now = 0, content_sz = 0;
            if (R_SUCCEEDED(httpcGetDownloadSizeState(&ctx, &dl_now, &content_sz))) {
                if (dl_now > prev_dl) {
                    u32 delta = dl_now - prev_dl;
                    if (delta > avail) delta = avail;
                    total += delta;
                    prev_dl = dl_now;
                }
            }
        } while (rc == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING && !g_worker_stop);

        if (aborted || g_worker_stop || (rc != 0 && rc != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING)) {
            /* Always cancel before closing an incomplete stream so httpcCloseContext cannot hang. */
            httpcCancelConnection(&ctx);
            httpcCloseContext(&ctx);
            if (rc != 0 && !aborted) {
                printf("[lobby3ds] Download failed for %s: 0x%08lX\n", url, (unsigned long)rc);
                return -1;
            }
            if (g_worker_stop)
                return -1;
        } else {
            httpcCloseContext(&ctx);
        }

        out_buf[total] = '\0';
        return (int)total;
    }

    return -1;
}

static int httpc_post_json(const char *url, const char *json_body)
{
    if (!url || !json_body)
        return -1;
    if (!httpc_ensure_init())
        return -1;

    size_t body_len = strlen(json_body);
    if (body_len == 0 || body_len > 2048u)
        return -1;

    /* 4-byte aligned buffer required by httpcAddPostDataRaw() */
    u32 aligned_body[(2048u + 3u) / 4u];
    memset(aligned_body, 0, sizeof(aligned_body));
    memcpy(aligned_body, json_body, body_len);

    httpcContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    Result rc = httpcOpenContext(&ctx, HTTPC_METHOD_POST, url, 0);
    if (R_FAILED(rc))
        return -1;

    httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
    httpcSetKeepAlive(&ctx, HTTPC_KEEPALIVE_DISABLED);
    httpcAddRequestHeaderField(&ctx, "User-Agent", "nox-decomp-3ds/1");
    httpcAddRequestHeaderField(&ctx, "Accept", "application/json");
    httpcAddRequestHeaderField(&ctx, "Content-Type", "application/json");

    rc = httpcAddPostDataRaw(&ctx, aligned_body, (u32)body_len);
    if (R_FAILED(rc)) {
        httpcCancelConnection(&ctx);
        httpcCloseContext(&ctx);
        return -1;
    }

    rc = httpcBeginRequest(&ctx);
    if (R_FAILED(rc)) {
        httpcCancelConnection(&ctx);
        httpcCloseContext(&ctx);
        return -1;
    }

    u32 status = 0;
    rc = httpcGetResponseStatusCodeTimeout(&ctx, &status, NOX_3DS_LOBBY_HTTP_TIMEOUT_NS);
    httpcCancelConnection(&ctx);
    httpcCloseContext(&ctx);

    if (R_FAILED(rc))
        return -1;
    return (status >= 200 && status < 300) ? 0 : -1;
}

/* =========================================================================
 * Background Worker Thread (32 KB Stack; Heap-Allocated 64 KB JSON Buffer)
 * ========================================================================= */
static void worker_do_fetch_list(char *json_buf, size_t json_cap)
{
    bool disabled = false;
    lobby_endpoint_t endpoints[NOX_3DS_LOBBY_MAX_ENDPOINTS];
    size_t ep_count = lobby_load_endpoints(endpoints, NOX_3DS_LOBBY_MAX_ENDPOINTS, &disabled);
    if (disabled || ep_count == 0) {
        RecursiveLock_Lock(&g_lobby_lock);
        g_fetch_inflight = false;
        RecursiveLock_Unlock(&g_lobby_lock);
        return;
    }

    nox_server_row parsed[NOX_3DS_LOBBY_MAX_SERVERS];
    lobby_cached_row_t resolved[NOX_3DS_LOBBY_MAX_SERVERS];
    size_t parsed_count = 0;
    size_t resolved_count = 0;
    bool got_valid_response = false;
    char good_base[256] = {0};

    for (size_t i = 0; i < ep_count && !g_worker_stop; ++i) {
        char full_url[320];
        snprintf(full_url, sizeof(full_url), "%s/api/v0/games/list", endpoints[i].base_url);
        printf("[lobby3ds] Fetching %s\n", full_url);

        int n = httpc_get_body(full_url, json_buf, json_cap);
        if (n <= 0)
            continue;

        /* Quick sanity check before invoking cJSON */
        const char *p = json_buf;
        while (*p && (unsigned char)*p <= ' ') p++;
        if (*p != '{' && *p != '[') {
            printf("[lobby3ds] Non-JSON response from %s\n", full_url);
            continue;
        }

        parsed_count = lobby_parse_games_json(json_buf, parsed, NOX_3DS_LOBBY_MAX_SERVERS);
        /* Pre-resolve all server IPv4 addresses strictly on this background thread
         * so the 30 Hz main thread never blocks on DNS or holds g_lobby_lock during DNS. */
        resolved_count = 0;
        for (size_t j = 0; j < parsed_count && !g_worker_stop; ++j) {
            struct in_addr srv_ip;
            if (!resolve_ipv4_addr(parsed[j].addr, &srv_ip)) {
                printf("[lobby3ds] Skip unresolvable server '%s' (%s)\n",
                       parsed[j].name, parsed[j].addr);
                continue;
            }
            resolved[resolved_count].row = parsed[j];
            resolved[resolved_count].resolved_ip = srv_ip;
            resolved_count++;
        }

        got_valid_response = true;
        snprintf(good_base, sizeof(good_base), "%s", endpoints[i].base_url);
        printf("[lobby3ds] Parsed %zu server(s) (%zu resolved) from %s (%d bytes)\n",
               parsed_count, resolved_count, endpoints[i].base_url, n);
        break;
    }

    uint32_t now_ms = nox_3ds_get_ticks_ms();
    RecursiveLock_Lock(&g_lobby_lock);
    g_fetch_inflight = false;
    if (got_valid_response) {
        memcpy(g_srv_cache, resolved, resolved_count * sizeof(lobby_cached_row_t));
        g_srv_cache_count = resolved_count;
        g_cache_valid = true;
        g_cache_last_ok_ms = now_ms ? now_ms : 1u;
        g_cache_next_try_ms = 0;
        snprintf(g_last_good_base_url, sizeof(g_last_good_base_url), "%s", good_base);

        /* Immediately inject into the 0x0D queue if the player is still in the browser */
        if (nox_3ds_net_get_mode() == NOX_3DS_NET_MODE_WIFI && g_active_ping_valid) {
            queue_cached_rows_locked();
        }
    } else {
        g_cache_next_try_ms = now_ms + NOX_3DS_LOBBY_RETRY_BACKOFF_MS;
        printf("[lobby3ds] All lobby endpoints failed; backing off %u ms\n",
               (unsigned)NOX_3DS_LOBBY_RETRY_BACKOFF_MS);
    }
    RecursiveLock_Unlock(&g_lobby_lock);
}

static void json_escape_str(const char *src, char *dst, size_t dst_sz)
{
    if (!dst || dst_sz == 0) return;
    size_t n = 0;
    if (src) {
        for (; *src && n + 2 < dst_sz; ++src) {
            unsigned char c = (unsigned char)*src;
            if (c == '"' || c == '\\') {
                dst[n++] = '\\';
                dst[n++] = (char)c;
            } else if (c >= 0x20u && c != 0x7Fu) {
                dst[n++] = (char)c;
            }
        }
    }
    dst[n] = '\0';
}

static void worker_do_host_register(const host_reg_job_t *job)
{
    if (!job) return;

    char esc_name[128];
    char esc_map[64];
    json_escape_str(job->name, esc_name, sizeof(esc_name));
    json_escape_str(job->map, esc_map, sizeof(esc_map));

    char body[512];
    const char *mode = lobby_mode_from_flags(job->flags);
    unsigned max_p = job->max ? job->max : 31u;
    unsigned cur_p = (job->cur > max_p) ? max_p : job->cur;
    unsigned port = job->port ? job->port : NOX_3DS_LOBBY_DEFAULT_PORT;

    int n = snprintf(body, sizeof(body),
                     "{"
                       "\"name\":\"%s\","
                       "\"map\":\"%s\","
                       "\"mode\":\"%s\","
                       "\"vers\":\"1.2\","
                       "\"port\":%u,"
                       "\"players\":{\"cur\":%u,\"max\":%u}"
                     "}",
                     esc_name, esc_map, mode, port, cur_p, max_p);
    if (n <= 0 || (size_t)n >= sizeof(body))
        return;

    /* Prefer last good lobby base URL if known */
    char last_good[256] = {0};
    RecursiveLock_Lock(&g_lobby_lock);
    snprintf(last_good, sizeof(last_good), "%s", g_last_good_base_url);
    RecursiveLock_Unlock(&g_lobby_lock);

    char url[320];
    if (last_good[0] != '\0') {
        snprintf(url, sizeof(url), "%s/api/v0/games/register", last_good);
        if (httpc_post_json(url, body) == 0) {
            printf("[lobby3ds] Registered host '%s' on %s\n", job->name, last_good);
            return;
        }
    }

    bool disabled = false;
    lobby_endpoint_t endpoints[NOX_3DS_LOBBY_MAX_ENDPOINTS];
    size_t ep_count = lobby_load_endpoints(endpoints, NOX_3DS_LOBBY_MAX_ENDPOINTS, &disabled);
    if (disabled) return;

    for (size_t i = 0; i < ep_count && !g_worker_stop; ++i) {
        snprintf(url, sizeof(url), "%s/api/v0/games/register", endpoints[i].base_url);
        if (httpc_post_json(url, body) == 0) {
            RecursiveLock_Lock(&g_lobby_lock);
            snprintf(g_last_good_base_url, sizeof(g_last_good_base_url), "%s", endpoints[i].base_url);
            RecursiveLock_Unlock(&g_lobby_lock);
            printf("[lobby3ds] Registered host '%s' on %s\n", job->name, endpoints[i].base_url);
            return;
        }
    }
}

static void lobby_worker_main(void *arg)
{
    (void)arg;
    char *json_buf = (char *)malloc(NOX_3DS_LOBBY_JSON_CAP);
    if (!json_buf) {
        printf("[lobby3ds] OOM allocating %u KB JSON buffer\n",
               (unsigned)(NOX_3DS_LOBBY_JSON_CAP / 1024u));
        return;
    }

    while (!g_worker_stop) {
        bool do_fetch = false;
        host_reg_job_t reg_copy;
        memset(&reg_copy, 0, sizeof(reg_copy));

        RecursiveLock_Lock(&g_lobby_lock);
        if (g_fetch_requested) {
            g_fetch_requested = false;
            g_fetch_inflight = true;
            do_fetch = true;
        } else if (g_host_reg_job.pending) {
            reg_copy = g_host_reg_job;
            g_host_reg_job.pending = false;
        }
        RecursiveLock_Unlock(&g_lobby_lock);

        if (do_fetch) {
            worker_do_fetch_list(json_buf, NOX_3DS_LOBBY_JSON_CAP);
            continue;
        }
        if (reg_copy.pending) {
            worker_do_host_register(&reg_copy);
            continue;
        }

        /* Sleep efficiently until signaled (250 ms poll cap for clean shutdown) */
        LightEvent_WaitTimeout(&g_lobby_event, 250000000LL);
    }

    free(json_buf);
}

static bool ensure_worker_started_locked(void)
{
    if (g_worker_started)
        return true;

    s32 main_prio = 0x30;
    svcGetThreadPriority(&main_prio, CUR_THREAD_HANDLE);
    s32 worker_prio = main_prio + 1;
    if (worker_prio > 0x3F)
        worker_prio = 0x3F;

    g_worker_stop = false;
    g_worker_thread = threadCreate(lobby_worker_main, NULL,
                                   NOX_3DS_LOBBY_WORKER_STACK,
                                   worker_prio, -2, false);
    if (!g_worker_thread) {
        printf("[lobby3ds] threadCreate() failed\n");
        return false;
    }
    g_worker_started = true;
    printf("[lobby3ds] Background HTTP worker started (stack=32KB prio=0x%lX)\n",
           (unsigned long)worker_prio);
    return true;
}

/* =========================================================================
 * 3DS Lifecycle Hooks (called by port/3ds/net_3ds.c)
 * ========================================================================= */
void nox_3ds_lobby_init(void)
{
    if (g_lobby_inited)
        return;

    RecursiveLock_Init(&g_lobby_lock);
    LightEvent_Init(&g_lobby_event, RESET_ONESHOT);
    memset(g_fake_packets, 0, sizeof(g_fake_packets));
    memset(g_srv_cache, 0, sizeof(g_srv_cache));
    memset(&g_host_reg_job, 0, sizeof(g_host_reg_job));
    g_last_good_base_url[0] = '\0';
    g_srv_cache_count = 0;
    g_cache_valid = false;
    g_fetch_requested = false;
    g_fetch_inflight = false;
    g_active_ping_valid = false;
    g_active_ping_sockfd = -1;
    g_host_active = false;
    g_lobby_inited = true;
    /* NOTE: httpcInit() and worker thread are intentionally NOT started here. */
}

void nox_3ds_lobby_on_mode_change(nox_3ds_net_mode_t mode)
{
    if (!g_lobby_inited)
        return;

    if (mode == NOX_3DS_NET_MODE_LOOPBACK) {
        RecursiveLock_Lock(&g_lobby_lock);
        flush_fake_packets_locked(-1);
        g_active_ping_valid = false;
        g_active_ping_sockfd = -1;
        g_host_active = false;
        g_host_reg_job.pending = false;
        RecursiveLock_Unlock(&g_lobby_lock);
    }
}

void nox_3ds_lobby_shutdown(void)
{
    if (!g_lobby_inited)
        return;

    g_worker_stop = true;
    LightEvent_Signal(&g_lobby_event);

    bool worker_joined = true;
    if (g_worker_started && g_worker_thread) {
        /* Wait up to 5 seconds (> 3s per-request httpc timeout) for clean unwind */
        Result jrc = threadJoin(g_worker_thread, 5000000000ULL);
        if (R_SUCCEEDED(jrc)) {
            threadFree(g_worker_thread);
            g_worker_thread = NULL;
            g_worker_started = false;
        } else {
            worker_joined = false;
            printf("[lobby3ds] WARN: worker threadJoin timed out (0x%08lX); skipping httpcExit\n",
                   (unsigned long)jrc);
        }
    }

    if (worker_joined && g_httpc_inited) {
        httpcExit();
        g_httpc_inited = false;
        nox_3ds_state_mut()->httpc_ready = false;
        printf("[lobby3ds] httpcExit() complete\n");
    }

    RecursiveLock_Lock(&g_lobby_lock);
    flush_fake_packets_locked(-1);
    g_active_ping_valid = false;
    g_host_active = false;
    RecursiveLock_Unlock(&g_lobby_lock);

    g_lobby_inited = false;
}

/* =========================================================================
 * Public Engine Hooks (src/netextras.h — called from src/GAME5.c)
 * ========================================================================= */
void nox_netextras_on_discovery_ping_send(int sockfd,
                                          const void *buf,
                                          size_t len,
                                          unsigned dest_port)
{
    if (!g_lobby_inited)
        nox_3ds_lobby_init();

    /* Strict Solo Campaign / Solo Quest isolation: never touch HTTP in loopback mode */
    if (nox_3ds_net_get_mode() != NOX_3DS_NET_MODE_WIFI)
        return;

    if (!buf || len < 12 || dest_port != NOX_3DS_LOBBY_DEFAULT_PORT)
        return;

    const uint8_t *p = (const uint8_t *)buf;
    if (p[2] != 0x0C)
        return;

    uint32_t now_ms = nox_3ds_get_ticks_ms();

    RecursiveLock_Lock(&g_lobby_lock);

    g_active_ping_sockfd = sockfd;
    memcpy(g_active_ping_cookie, p + 8, 4);
    g_active_ping_valid = true;
    flush_fake_packets_locked(sockfd);

    bool cache_fresh = g_cache_valid &&
                       g_cache_last_ok_ms != 0 &&
                       (now_ms - g_cache_last_ok_ms) < NOX_3DS_LOBBY_CACHE_TTL_MS;

    if (g_srv_cache_count > 0) {
        /* Immediately populate native browser with cached rows (zero wait) */
        queue_cached_rows_locked();
    }

    if (!cache_fresh && !g_fetch_inflight && !g_fetch_requested) {
        if (g_cache_next_try_ms == 0 || (int32_t)(now_ms - g_cache_next_try_ms) >= 0) {
            if (ensure_worker_started_locked()) {
                g_fetch_requested = true;
                LightEvent_Signal(&g_lobby_event);
            }
        }
    }

    RecursiveLock_Unlock(&g_lobby_lock);
}

int nox_netextras_fake_pending(int sockfd)
{
    if (!g_lobby_inited)
        return 0;

    int pending = 0;
    RecursiveLock_Lock(&g_lobby_lock);
    for (int i = 0; i < NOX_3DS_LOBBY_FAKE_QUEUE_CAP; ++i) {
        if (g_fake_packets[i].used && g_fake_packets[i].sockfd == sockfd) {
            pending = 1;
            break;
        }
    }
    RecursiveLock_Unlock(&g_lobby_lock);
    return pending;
}

int nox_netextras_try_fake_recvfrom(int sockfd,
                                    void *buffer,
                                    size_t length,
                                    struct sockaddr *addr,
                                    socklen_t *addrlen)
{
    if (!buffer) {
        errno = EINVAL;
        return -1;
    }
    if (!g_lobby_inited)
        return -2;

    RecursiveLock_Lock(&g_lobby_lock);
    for (int i = 0; i < NOX_3DS_LOBBY_FAKE_QUEUE_CAP; ++i) {
        fake_srv_pkt_t *slot = &g_fake_packets[i];
        if (!slot->used || slot->sockfd != sockfd)
            continue;
        if (slot->len > length) {
            slot->used = false;
            continue;
        }

        memset(buffer, 0, length);
        memcpy(buffer, slot->data, slot->len);

        if (addr && addrlen) {
            if ((size_t)(*addrlen) >= sizeof(struct sockaddr_in)) {
                memcpy(addr, &slot->from, sizeof(struct sockaddr_in));
            }
            *addrlen = (socklen_t)sizeof(struct sockaddr_in);
        }

        int ret = (int)slot->len;
        slot->used = false;
        RecursiveLock_Unlock(&g_lobby_lock);
        return ret;
    }
    RecursiveLock_Unlock(&g_lobby_lock);
    return -2;
}

void nox_netextras_on_host_serverinfo(const void *packet, size_t len)
{
    if (!packet || len < 73)
        return;

    const uint8_t *pkt = (const uint8_t *)packet;
    if (pkt[2] != 0x0D)
        return;

    uint16_t flags = (uint16_t)((uint16_t)pkt[28] | ((uint16_t)pkt[29] << 8));
    g_last_serverinfo_flags = flags;

    if (nox_3ds_net_get_mode() != NOX_3DS_NET_MODE_WIFI)
        return;
    if (!lobby_should_register_host())
        return;
    if (!g_lobby_inited)
        nox_3ds_lobby_init();

    uint32_t now_ms = nox_3ds_get_ticks_ms();

    RecursiveLock_Lock(&g_lobby_lock);
    if (g_host_reg_job.pending ||
        (g_host_last_reg_ms != 0 && (now_ms - g_host_last_reg_ms) < NOX_3DS_LOBBY_REG_PERIOD_MS)) {
        RecursiveLock_Unlock(&g_lobby_lock);
        return;
    }

    memset(&g_host_reg_job, 0, sizeof(g_host_reg_job));
    g_host_reg_job.cur = (unsigned)pkt[3];
    g_host_reg_job.max = (unsigned)pkt[4];
    g_host_reg_job.port = g_host_bound_port ? g_host_bound_port : NOX_3DS_LOBBY_DEFAULT_PORT;
    g_host_reg_job.flags = flags;

    size_t mlen = strnlen((const char *)&pkt[10], sizeof(g_host_reg_job.map) - 1u);
    memcpy(g_host_reg_job.map, &pkt[10], mlen);
    g_host_reg_job.map[mlen] = '\0';

    size_t nlen = strnlen((const char *)&pkt[72], sizeof(g_host_reg_job.name) - 1u);
    memcpy(g_host_reg_job.name, &pkt[72], nlen);
    g_host_reg_job.name[nlen] = '\0';

    if (ensure_worker_started_locked()) {
        g_host_reg_job.pending = true;
        g_host_last_reg_ms = now_ms ? now_ms : 1u;
        LightEvent_Signal(&g_lobby_event);
    }
    RecursiveLock_Unlock(&g_lobby_lock);
}

void nox_netextras_on_host_bind_success(int sockfd, unsigned bound_port)
{
    (void)sockfd;
    if (nox_3ds_net_get_mode() != NOX_3DS_NET_MODE_WIFI)
        return;
    if (!lobby_should_register_host())
        return;
    if (!g_lobby_inited)
        nox_3ds_lobby_init();

    RecursiveLock_Lock(&g_lobby_lock);
    g_host_bound_port = bound_port ? bound_port : NOX_3DS_LOBBY_DEFAULT_PORT;
    g_host_active = true;
    g_host_last_snapshot_ms = 0;
    g_host_last_reg_ms = 0;
    RecursiveLock_Unlock(&g_lobby_lock);
}

void nox_netextras_host_tick(void)
{
    if (!g_lobby_inited || !g_host_active)
        return;
    if (nox_3ds_net_get_mode() != NOX_3DS_NET_MODE_WIFI)
        return;
    if (!lobby_should_register_host())
        return;

    uint32_t now_ms = nox_3ds_get_ticks_ms();
    bool do_snapshot = false;

    RecursiveLock_Lock(&g_lobby_lock);
    if (g_host_active &&
        (g_host_last_snapshot_ms == 0 || (now_ms - g_host_last_snapshot_ms) >= 1000u)) {
        g_host_last_snapshot_ms = now_ms ? now_ms : 1u;
        do_snapshot = true;
    }
    RecursiveLock_Unlock(&g_lobby_lock);

    if (!do_snapshot)
        return;

    char buf[256];
    int dummy[3] = { 0, 0, 0 };
    unsigned int len = sub_554040((int)(uintptr_t)dummy, (int)sizeof(buf), buf);
    if (len >= 73u && (uint8_t)buf[2] == 0x0D) {
        nox_netextras_on_host_serverinfo(buf, (size_t)len);
    }
}
