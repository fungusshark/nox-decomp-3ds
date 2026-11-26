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
#include "nox_3ds_version.h"
#include "third_party/cJSON.h"

#ifndef NOX_3DS_VERSION_STR
#define NOX_3DS_VERSION_STR "3ds-dev"
#endif
#include "../../src/netextras.h"
#include "../../src/netextras_types.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

/* Externs from engine / port */
extern unsigned int sub_554040(int a1, int a2, char *a3);
extern int nox_env_truthy(const char *s);

#define NOX_3DS_LOBBY_MAX_SERVERS       64
#define NOX_3DS_LOBBY_FAKE_QUEUE_CAP    64
#define NOX_3DS_LOBBY_JSON_CAP          (128u * 1024u)
#define NOX_3DS_LOBBY_WORKER_STACK      (64u * 1024u)
#define NOX_3DS_LOBBY_CACHE_TTL_MS      60000u
#define NOX_3DS_LOBBY_REPING_TTL_MS     10000u
#define NOX_3DS_LOBBY_RETRY_BACKOFF_MS  5000u
#define NOX_3DS_LOBBY_HTTP_TIMEOUT_MS   5000
#define NOX_3DS_LOBBY_HTTP_TIMEOUT_NS   5000000000ULL /* 5.0 seconds */
#define NOX_3DS_LOBBY_MAX_REDIRECTS     4
#define NOX_3DS_LOBBY_DEFAULT_PORT      18590u
#define NOX_3DS_LOBBY_REG_PERIOD_MS     20000u
#define NOX_3DS_LOBBY_MAX_ENDPOINTS     8
#define NOX_3DS_NET_LOG_CAP             65536u

static RecursiveLock g_netlog_lock;
static bool          g_netlog_lock_inited = false;
static bool          g_netlog_started     = false;
static size_t        g_netlog_bytes       = 0;

void nox_3ds_net_log(const char *fmt, ...)
{
    if (!fmt)
        return;

    static int s_file_log_enabled = -1;
    if (s_file_log_enabled < 0)
        s_file_log_enabled = (nox_env_truthy(getenv("NOX_3DS_NET_LOG")) != 0) ? 1 : 0;

    if (!g_netlog_lock_inited) {
        RecursiveLock_Init(&g_netlog_lock);
        g_netlog_lock_inited = true;
    }

    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;

    uint32_t ms = nox_3ds_get_ticks_ms();
    fprintf(stderr, "[%ums] %s\n", (unsigned)ms, msg);

    if (!s_file_log_enabled)
        return;

    RecursiveLock_Lock(&g_netlog_lock);
    if (g_netlog_bytes < NOX_3DS_NET_LOG_CAP) {
        const char *mode = g_netlog_started ? "a" : "w";
        FILE *f = fopen(NOX_3DS_DATA_ROOT "/net.log", mode);
        if (f) {
            g_netlog_started = true;
            int wrote = fprintf(f, "[%ums] %s\n", (unsigned)ms, msg);
            if (wrote > 0)
                g_netlog_bytes += (size_t)wrote;
            fflush(f);
            fclose(f);
        }
    }
    RecursiveLock_Unlock(&g_netlog_lock);
}

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
    uint16_t       access_flags;
    uint32_t       proto_ver;
} lobby_cached_row_t;

/* Discovery fetch state & server cache */
static bool g_fetch_requested = false;
static bool g_fetch_inflight = false;
static bool g_cache_valid = false;
static bool g_cache_consumed_by_ping = false;
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
static uint32_t g_host_client_proto_ver = NOX_PROTO_VER_LEGACY;
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
    static bool s_eps_cached = false;
    static bool s_eps_disabled = false;
    static lobby_endpoint_t s_cached_eps[NOX_3DS_LOBBY_MAX_ENDPOINTS];
    static size_t s_cached_ep_count = 0;

    if (!s_eps_cached) {
        const char *no_inet = getenv("NOX_NO_INTERNET_SERVERS");
        if (no_inet && nox_env_truthy(no_inet)) {
            s_eps_disabled = true;
            s_eps_cached = true;
        } else {
            const char *inj = getenv("NOX_INJECT_INTERNET_SERVERS");
            if (inj && strcmp(inj, "0") == 0) {
                s_eps_disabled = true;
                s_eps_cached = true;
            }
        }

        if (!s_eps_cached) {
            /* Optional SD card config: sdmc:/3ds/nox/lobby.txt
             * One endpoint per line: "nox.nwca.xyz:8088", "https://...", or "disabled". */
            FILE *f = fopen(NOX_3DS_DATA_ROOT "/lobby.txt", "r");
            if (f) {
                char line[256];
                while (s_cached_ep_count < NOX_3DS_LOBBY_MAX_ENDPOINTS && fgets(line, sizeof(line), f)) {
                    char *p = line;
                    while (*p && (unsigned char)*p <= ' ') p++;
                    if (*p == '\0' || *p == '#')
                        continue;
                    char *end = p + strlen(p);
                    while (end > p && (unsigned char)end[-1] <= ' ')
                        *--end = '\0';
                    if (strcasecmp(p, "disabled") == 0 || strcasecmp(p, "off") == 0 || strcmp(p, "0") == 0) {
                        s_eps_disabled = true;
                        s_cached_ep_count = 0;
                        break;
                    }
                    if (lobby_endpoint_normalize(p, s_cached_eps[s_cached_ep_count].base_url,
                                                 sizeof(s_cached_eps[s_cached_ep_count].base_url))) {
                        s_cached_ep_count++;
                    }
                }
                fclose(f);
            }

            if (!s_eps_disabled && s_cached_ep_count == 0) {
                for (size_t i = 0;
                     i < sizeof(k_default_lobby_endpoints) / sizeof(k_default_lobby_endpoints[0]) &&
                     s_cached_ep_count < NOX_3DS_LOBBY_MAX_ENDPOINTS;
                     ++i) {
                    if (lobby_endpoint_normalize(k_default_lobby_endpoints[i],
                                                 s_cached_eps[s_cached_ep_count].base_url,
                                                 sizeof(s_cached_eps[s_cached_ep_count].base_url))) {
                        s_cached_ep_count++;
                    }
                }
            }
            s_eps_cached = true;
        }
    }

    if (out_disabled)
        *out_disabled = s_eps_disabled;
    if (s_eps_disabled || s_cached_ep_count == 0)
        return 0;

    size_t copy_n = (s_cached_ep_count < max_eps) ? s_cached_ep_count : max_eps;
    memcpy(eps, s_cached_eps, copy_n * sizeof(lobby_endpoint_t));
    return copy_n;
}

static bool lobby_should_register_host(void)
{
    static int s_cached = -1;
    if (s_cached >= 0)
        return s_cached != 0;

    const char *env_reg = getenv("NOX_LOBBY_REGISTER_ENABLE");
    if (env_reg && *env_reg) {
        s_cached = (nox_env_truthy(env_reg) != 0) ? 1 : 0;
        return s_cached != 0;
    }

    /* Enabled by default when hosting over Wi-Fi; create sdmc:/3ds/nox/lobby_host.txt
     * with "0" / "off" to keep a LAN host private. */
    FILE *f = fopen(NOX_3DS_DATA_ROOT "/lobby_host.txt", "r");
    if (f) {
        char buf[32] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        s_cached = (n == 0 || nox_env_truthy(buf) != 0) ? 1 : 0;
        return s_cached != 0;
    }
    s_cached = 1;
    return true;
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

static size_t lobby_parse_games_json(const char *json_text, lobby_cached_row_t *out, size_t out_cap)
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

            lobby_cached_row_t item;
            memset(&item, 0, sizeof(item));
            item.row.port = (uint16_t)NOX_3DS_LOBBY_DEFAULT_PORT;
            item.row.players_max = 31;
            item.proto_ver = NOX_PROTO_VER_LEGACY;

            copy_json_str(elem, "name", item.row.name, sizeof(item.row.name));
            copy_json_str(elem, "addr", item.row.addr, sizeof(item.row.addr));
            if (item.row.addr[0] == '\0')
                copy_json_str(elem, "ip", item.row.addr, sizeof(item.row.addr));
            copy_json_str(elem, "map", item.row.map, sizeof(item.row.map));
            copy_json_str(elem, "mode", item.row.mode, sizeof(item.row.mode));

            const cJSON *jres = cJSON_GetObjectItemCaseSensitive(elem, "res");
            if (cJSON_IsObject(jres)) {
                const cJSON *jhires = cJSON_GetObjectItemCaseSensitive(jres, "high_res");
                const cJSON *jw     = cJSON_GetObjectItemCaseSensitive(jres, "width");
                const cJSON *jh     = cJSON_GetObjectItemCaseSensitive(jres, "height");
                if (cJSON_IsTrue(jhires) ||
                    (cJSON_IsNumber(jw) && jw->valueint > 1024) ||
                    (cJSON_IsNumber(jh) && jh->valueint > 768)) {
                    item.proto_ver = NOX_PROTO_VER_HIGHRES;
                }
            }

            char vers_str[64] = {0};
            copy_json_str(elem, "vers", vers_str, sizeof(vers_str));
            if (vers_str[0] != '\0' && item.proto_ver != NOX_PROTO_VER_HIGHRES) {
                /* OpenNox v1.8/v1.9 servers (even at 1024x768) speak 0x000F039A unless lowres/1.02. */
                if ((strncmp(vers_str, "v1.", 3) == 0 ||
                     strncmp(vers_str, "1.8", 3) == 0 ||
                     strncmp(vers_str, "1.9", 3) == 0) &&
                    strstr(vers_str, "lowres") == NULL &&
                    strstr(vers_str, "1.02") == NULL) {
                    item.proto_ver = NOX_PROTO_VER_HIGHRES;
                }
            }

            char access_str[32] = {0};
            copy_json_str(elem, "access", access_str, sizeof(access_str));
            if (strcasecmp(access_str, "closed") == 0) {
                item.access_flags |= 0x10u;
            } else if (strcasecmp(access_str, "password") == 0) {
                item.access_flags |= 0x20u;
            }

            const cJSON *jport = cJSON_GetObjectItemCaseSensitive(elem, "port");
            if (cJSON_IsNumber(jport) && jport->valueint > 0 && jport->valueint <= 65535) {
                item.row.port = (uint16_t)jport->valueint;
            }

            const cJSON *jplayers = cJSON_GetObjectItemCaseSensitive(elem, "players");
            if (cJSON_IsObject(jplayers)) {
                const cJSON *jcur = cJSON_GetObjectItemCaseSensitive(jplayers, "cur");
                const cJSON *jmax = cJSON_GetObjectItemCaseSensitive(jplayers, "max");
                if (cJSON_IsNumber(jcur) && jcur->valueint >= 0) {
                    item.row.players_cur = (uint8_t)(jcur->valueint > 255 ? 255 : jcur->valueint);
                }
                if (cJSON_IsNumber(jmax) && jmax->valueint > 0) {
                    item.row.players_max = (uint8_t)(jmax->valueint > 255 ? 255 : jmax->valueint);
                }
            } else if (cJSON_IsNumber(jplayers) && jplayers->valueint >= 0) {
                item.row.players_cur = (uint8_t)(jplayers->valueint > 255 ? 255 : jplayers->valueint);
                const cJSON *jmaxp = cJSON_GetObjectItemCaseSensitive(elem, "max_players");
                if (cJSON_IsNumber(jmaxp) && jmaxp->valueint > 0) {
                    item.row.players_max = (uint8_t)(jmaxp->valueint > 255 ? 255 : jmaxp->valueint);
                }
            }

            if (item.row.addr[0] == '\0')
                continue;

            out[count++] = item;
        }
    }

    cJSON_Delete(root);
    return count;
}

/* =========================================================================
 * IPv4 Resolution & Synthetic 1999 MSVC 0x0D Server-Info Packet Builder
 * ========================================================================= */
#define LOBBY_DNS_CACHE_CAP 16

typedef struct {
    bool           used;
    char           host[256];
    struct in_addr ip;
} lobby_dns_cache_entry_t;

static lobby_dns_cache_entry_t s_dns_cache[LOBBY_DNS_CACHE_CAP];
static size_t s_dns_cache_cursor = 0;
static char s_last_logged_reg_url[256] = "";
static bool s_last_reg_failed = false;

static void dns_cache_clear(void)
{
    RecursiveLock_Lock(&g_lobby_lock);
    memset(s_dns_cache, 0, sizeof(s_dns_cache));
    s_dns_cache_cursor = 0;
    RecursiveLock_Unlock(&g_lobby_lock);
}

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

    RecursiveLock_Lock(&g_lobby_lock);
    for (size_t i = 0; i < LOBBY_DNS_CACHE_CAP; ++i) {
        if (s_dns_cache[i].used && strcasecmp(s_dns_cache[i].host, host_or_ip) == 0) {
            *out_addr = s_dns_cache[i].ip;
            RecursiveLock_Unlock(&g_lobby_lock);
            return true;
        }
    }
    RecursiveLock_Unlock(&g_lobby_lock);

    /* Fallback: ensure soc:u is initialized, then DNS lookup (serialized) */
    if (!nox_3ds_soc_ensure_ready())
        return false;

    uint32_t ip_be = 0;
    if (!nox_3ds_soc_resolve_ipv4(host_or_ip, &ip_be))
        return false;

    out_addr->s_addr = (in_addr_t)ip_be;

    RecursiveLock_Lock(&g_lobby_lock);
    size_t idx = s_dns_cache_cursor % LOBBY_DNS_CACHE_CAP;
    s_dns_cache_cursor = (idx + 1u) % LOBBY_DNS_CACHE_CAP;
    s_dns_cache[idx].used = true;
    snprintf(s_dns_cache[idx].host, sizeof(s_dns_cache[idx].host), "%s", host_or_ip);
    s_dns_cache[idx].ip = *out_addr;
    RecursiveLock_Unlock(&g_lobby_lock);

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

static size_t queue_cached_rows_locked(struct sockaddr_in *out_addrs,
                                       size_t max_addrs,
                                       int *out_sockfd,
                                       uint8_t out_cookie[4])
{
    if (out_sockfd)
        *out_sockfd = -1;
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

        /* Exact 1999 MSVC 0x0D server-info layout (see GAME5.c:sub_554040 & GAME1.c:sub_4375F0).
         * Mark out[1] = 0x3D so sub_4375F0 initializes HTTP rows with ping=9999 ('???')
         * until a live UDP 0x0D reply arrives from the host. */
        out[0] = 0x00;
        out[1] = 0x3Du;
        out[2] = 0x0D;
        out[3] = row->players_cur;
        out[4] = row->players_max ? row->players_max : 31;
        out[5] = 0x0F;
        out[6] = 0x0F;

        /* Session ID placeholder @ offset 7..10 (written before map @ 10, matching sub_554040).
         * NOTE: byte 10 overlaps map[0], so always clear out[10] unconditionally. */
        {
            uint32_t sid = 0xFFFFFFFFu;
            memcpy(&out[7], &sid, sizeof(sid));
            out[10] = '\0';
        }

        /* Map name @ offset 10..18: clamped to 8 chars + NUL so out[19..27] and
         * GAME1.c:sub_4375F0's 9-byte map slot (v13[111..119]) never overflow. */
        if (row->map[0] != '\0') {
            size_t mlen = strnlen(row->map, 8);
            memcpy(&out[10], row->map, mlen);
            out[10 + mlen] = '\0';
        }

        /* Access flags @ offset 20 (closed=0x10, password=0x20) */
        out[20] = (uint8_t)(g_srv_cache[i].access_flags & 0xFFu);
        out[21] = (uint8_t)((g_srv_cache[i].access_flags >> 8) & 0xFFu);

        /* Protocol/Game Version @ offset 24..27 (0x0001039A = 1.2b, 0x000F039A = OpenNox HD). */
        {
            uint32_t ver = NOX_IS_COMPAT_PROTO_VER(g_srv_cache[i].proto_ver)
                         ? g_srv_cache[i].proto_ver
                         : NOX_PROTO_VER_LEGACY;
            memcpy(&out[24], &ver, sizeof(ver));
        }

        /* Game flags @ offset 28..29 (little-endian) */
        uint16_t base = g_last_serverinfo_flags ? g_last_serverinfo_flags : 0x0007u;
        uint16_t modebit = lobby_mode_to_flagbit(row->mode);
        uint16_t flags = (uint16_t)((base & (uint16_t)~NOX_MODE_MASK) | modebit);
        out[28] = (uint8_t)(flags & 0xFFu);
        out[29] = (uint8_t)((flags >> 8) & 0xFFu);

        /* Rule bitmasks & ping bounds (match sub_43AA70 defaults: 0xFF = unrestricted / no ping cap) */
        memset(&out[32], 0xFF, 8);
        memset(&out[48], 0xFF, 20);

        /* Map View pin coordinates @ offset 40..43 (valid inside Quadrant 0: x 36..347, y 36..387) */
        {
            uint32_t h = ntohl((uint32_t)srv_ip.s_addr) ^
                         ((uint32_t)row->port * 2654435761u) ^
                         ((uint32_t)i * 40503u);
            uint16_t px = (uint16_t)(40u + (h % 300u));
            uint16_t py = (uint16_t)(40u + ((h >> 11) % 340u));
            memcpy(&out[40], &px, sizeof(px));
            memcpy(&out[42], &py, sizeof(py));
        }

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
        if (out_addrs && queued < max_addrs)
            out_addrs[queued] = slot->from;
        queued++;
    }

    if (queued > 0) {
        if (out_sockfd)
            *out_sockfd = g_active_ping_sockfd;
        if (out_cookie)
            memcpy(out_cookie, g_active_ping_cookie, 4);
        nox_3ds_net_log("[lobby3ds] Queued %zu internet server(s) into 0x0D UI bridge (fd=%d)",
                        queued, g_active_ping_sockfd);
    }
    return queued;
}

static void ping_cached_rows_udp(int sockfd,
                                 const uint8_t cookie[4],
                                 const struct sockaddr_in *addrs,
                                 size_t count)
{
    if (sockfd < 0 || !cookie || !addrs || count == 0)
        return;
    if (nox_3ds_net_get_mode() != NOX_3DS_NET_MODE_WIFI)
        return;

    uint8_t ping_pkt[12];
    memset(ping_pkt, 0, sizeof(ping_pkt));
    ping_pkt[2] = 0x0C;
    memcpy(&ping_pkt[8], cookie, 4);

    for (size_t i = 0; i < count; ++i) {
        nox_3ds_net_sendto(sockfd, ping_pkt, (unsigned int)sizeof(ping_pkt), 0,
                           &addrs[i], (unsigned int)sizeof(addrs[i]));
    }
}

/* =========================================================================
 * 3DS http:C Transport (HTTPS & Fallback) + Pure soc:u HTTP/1.1 Primary
 * ========================================================================= */
static bool httpc_ensure_init(void)
{
    if (g_httpc_inited)
        return true;

    /* Probe Wi-Fi status for logging only; let httpcInit() itself determine
     * connectivity so emulators and setups reporting wifi_status==0 work. */
    u32 wifi_status = 0;
    if (R_SUCCEEDED(acInit())) {
        ACU_GetWifiStatus(&wifi_status);
        acExit();
        if (wifi_status == 0) {
            nox_3ds_net_log("[lobby3ds] WARN: ACU_GetWifiStatus=0; attempting httpcInit anyway");
        }
    }

    /* 4 KB shared memory supports both HTTP GET and HTTP POST; fallback to 0 (GET-only). */
    Result rc = httpcInit(0x1000);
    if (R_FAILED(rc)) {
        nox_3ds_net_log("[lobby3ds] httpcInit(0x1000) failed: 0x%08lX; trying httpcInit(0)",
                        (unsigned long)rc);
        rc = httpcInit(0);
    }
    if (R_FAILED(rc)) {
        nox_3ds_net_log("[lobby3ds] httpcInit failed: 0x%08lX", (unsigned long)rc);
        return false;
    }

    g_httpc_inited = true;
    nox_3ds_state_mut()->httpc_ready = true;
    nox_3ds_net_log("[lobby3ds] httpcInit succeeded");
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

static int httpc_get_body_with_proxy(const char *initial_url, char *out_buf, size_t out_cap, u32 use_proxy)
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

        Result rc = httpcOpenContext(&ctx, HTTPC_METHOD_GET, url, use_proxy);
        if (R_FAILED(rc)) {
            nox_3ds_net_log("[lobby3ds] httpcOpenContext(%s, proxy=%lu) failed: 0x%08lX",
                            url, (unsigned long)use_proxy, (unsigned long)rc);
            return -1;
        }

        /* Allow HTTPS against modern CAs not present in 3DS's 2016 root store */
        httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
        httpcSetKeepAlive(&ctx, HTTPC_KEEPALIVE_DISABLED);
        httpcAddRequestHeaderField(&ctx, "User-Agent", "nox-decomp-3ds/1");
        httpcAddRequestHeaderField(&ctx, "Accept", "application/json");

        rc = httpcBeginRequest(&ctx);
        if (R_FAILED(rc)) {
            nox_3ds_net_log("[lobby3ds] httpcBeginRequest(%s, proxy=%lu) failed: 0x%08lX",
                            url, (unsigned long)use_proxy, (unsigned long)rc);
            httpcCancelConnection(&ctx);
            httpcCloseContext(&ctx);
            return -1;
        }

        u32 status = 0;
        rc = httpcGetResponseStatusCodeTimeout(&ctx, &status, NOX_3DS_LOBBY_HTTP_TIMEOUT_NS);
        if (R_FAILED(rc)) {
            nox_3ds_net_log("[lobby3ds] http:C status timeout/error for %s (proxy=%lu): 0x%08lX",
                            url, (unsigned long)use_proxy, (unsigned long)rc);
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
            nox_3ds_net_log("[lobby3ds] http:C HTTP %lu from %s", (unsigned long)status, url);
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
                nox_3ds_net_log("[lobby3ds] http:C download failed for %s: 0x%08lX", url, (unsigned long)rc);
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

static int httpc_get_body(const char *initial_url, char *out_buf, size_t out_cap)
{
    int n = httpc_get_body_with_proxy(initial_url, out_buf, out_cap, 0);
    if (n > 0 || g_worker_stop)
        return n;
    return httpc_get_body_with_proxy(initial_url, out_buf, out_cap, 1);
}

static int httpc_post_json_with_proxy(const char *url, const char *json_body, u32 use_proxy)
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
    Result rc = httpcOpenContext(&ctx, HTTPC_METHOD_POST, url, use_proxy);
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

static int httpc_post_json(const char *url, const char *json_body)
{
    int r = httpc_post_json_with_proxy(url, json_body, 0);
    if (r >= 0 || g_worker_stop)
        return r;
    return httpc_post_json_with_proxy(url, json_body, 1);
}

/* =========================================================================
 * Primary Plain-HTTP Transport via soc:u TCP Sockets (HTTP/1.1 + Chunked)
 * ========================================================================= */
static bool parse_http_url(const char *url, bool *out_https,
                           char *host, size_t host_sz,
                           uint16_t *out_port,
                           char *path, size_t path_sz)
{
    if (!url || !host || host_sz < 4 || !out_port || !path || path_sz < 2)
        return false;

    bool is_https = false;
    const char *p = NULL;
    if (strncasecmp(url, "http://", 7) == 0) {
        p = url + 7;
    } else if (strncasecmp(url, "https://", 8) == 0) {
        is_https = true;
        p = url + 8;
    } else {
        return false;
    }
    if (out_https)
        *out_https = is_https;

    const char *slash = strchr(p, '/');
    size_t auth_len = slash ? (size_t)(slash - p) : strlen(p);
    if (auth_len == 0 || auth_len >= host_sz)
        return false;

    char auth[256];
    if (auth_len >= sizeof(auth))
        return false;
    memcpy(auth, p, auth_len);
    auth[auth_len] = '\0';

    uint16_t port = is_https ? 443u : 80u;
    char *colon = strrchr(auth, ':');
    if (colon) {
        *colon = '\0';
        long pv = strtol(colon + 1, NULL, 10);
        if (pv <= 0 || pv > 65535)
            return false;
        port = (uint16_t)pv;
    }
    if (auth[0] == '\0')
        return false;

    snprintf(host, host_sz, "%s", auth);
    *out_port = port;
    snprintf(path, path_sz, "%s", (slash && *slash) ? slash : "/");
    return true;
}

static bool find_header_ci(const char *hdrs, size_t hdrs_len, const char *key,
                           char *out_val, size_t out_sz)
{
    if (!hdrs || !key || !out_val || out_sz == 0)
        return false;
    out_val[0] = '\0';

    size_t key_len = strlen(key);
    const char *cur = hdrs;
    const char *end = hdrs + hdrs_len;
    while (cur < end) {
        const char *eol = (const char *)memchr(cur, '\n', (size_t)(end - cur));
        size_t line_len = eol ? (size_t)(eol - cur) : (size_t)(end - cur);
        while (line_len > 0 && cur[line_len - 1] == '\r')
            line_len--;

        if (line_len > key_len + 1u &&
            strncasecmp(cur, key, key_len) == 0 &&
            cur[key_len] == ':') {
            const char *v = cur + key_len + 1u;
            size_t vlen = line_len - (key_len + 1u);
            while (vlen > 0 && (*v == ' ' || *v == '\t')) {
                v++;
                vlen--;
            }
            if (vlen >= out_sz)
                vlen = out_sz - 1u;
            memcpy(out_val, v, vlen);
            out_val[vlen] = '\0';
            return true;
        }
        if (!eol)
            break;
        cur = eol + 1;
    }
    return false;
}

static int decode_chunked_body(const char *src, size_t src_len, char *dst, size_t dst_cap)
{
    if (!src || !dst || dst_cap < 2)
        return -1;

    size_t pos = 0;
    size_t out_len = 0;
    while (pos < src_len) {
        const char *nl = (const char *)memchr(src + pos, '\n', src_len - pos);
        if (!nl)
            return -1;
        size_t line_len = (size_t)(nl - (src + pos));
        char line[64];
        if (line_len >= sizeof(line))
            line_len = sizeof(line) - 1u;
        memcpy(line, src + pos, line_len);
        line[line_len] = '\0';

        char *endp = NULL;
        unsigned long chunk_sz = strtoul(line, &endp, 16);
        if (endp == line)
            return -1;

        pos = (size_t)(nl - src) + 1u;
        if (chunk_sz == 0)
            break;

        if (pos + chunk_sz > src_len)
            return -1;
        if (out_len + chunk_sz >= dst_cap)
            return -1;

        memcpy(dst + out_len, src + pos, chunk_sz);
        out_len += chunk_sz;
        pos += chunk_sz;

        if (pos < src_len && src[pos] == '\r')
            pos++;
        if (pos < src_len && src[pos] == '\n')
            pos++;
    }
    dst[out_len] = '\0';
    return (int)out_len;
}

static int soc_connect_with_timeout(struct in_addr ip, uint16_t port, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        nox_3ds_net_log("[lobby3ds] soc socket() failed: errno=%d", errno);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr = ip;

    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc == 0)
        return fd;
    if (errno == EISCONN)
        return fd;
    if (errno != EINPROGRESS && errno != EALREADY && errno != EWOULDBLOCK && errno != EAGAIN) {
        nox_3ds_net_log("[lobby3ds] soc initial connect(%s:%u) failed: errno=%d",
                        inet_ntoa(ip), (unsigned)port, errno);
        close(fd);
        return -1;
    }

    uint32_t start_ms = nox_3ds_get_ticks_ms();
    while (!g_worker_stop) {
        uint32_t elapsed = nox_3ds_get_ticks_ms() - start_ms;
        if ((int)elapsed >= timeout_ms) {
            nox_3ds_net_log("[lobby3ds] soc connect(%s:%u) timed out after %d ms",
                            inet_ntoa(ip), (unsigned)port, timeout_ms);
            close(fd);
            return -1;
        }
        int slice = timeout_ms - (int)elapsed;
        if (slice > 50) slice = 50;

        uint32_t poll_t0 = nox_3ds_get_ticks_ms();
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, slice);
        if (pr < 0 && errno != EINTR) {
            nox_3ds_net_log("[lobby3ds] soc connect poll() error: errno=%d", errno);
            close(fd);
            return -1;
        }
        if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            nox_3ds_net_log("[lobby3ds] soc connect failed revents=0x%x", (unsigned)pfd.revents);
            close(fd);
            return -1;
        }

        /* Retry non-blocking connect() first before falling back to getpeername();
         * calling getpeername() while connect is still pending leaves sa_family==0
         * and triggers Azahar's CTRSockAddr::ToPlatform assertion. */
        rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
        if (rc == 0 || errno == EISCONN)
            return fd;
        if (errno == EINPROGRESS || errno == EALREADY || errno == EWOULDBLOCK || errno == EAGAIN) {
            uint32_t poll_dt = nox_3ds_get_ticks_ms() - poll_t0;
            if (pr != 0 || poll_dt < 5u)
                nox_3ds_sleep_ms(15);
            continue;
        }

        if (errno == EINVAL) {
            struct sockaddr_in peer;
            socklen_t plen = (socklen_t)sizeof(peer);
            memset(&peer, 0, sizeof(peer));
            if (getpeername(fd, (struct sockaddr *)&peer, &plen) == 0 &&
                peer.sin_family == AF_INET && peer.sin_addr.s_addr != 0) {
                return fd;
            }
        }

        nox_3ds_net_log("[lobby3ds] soc connect(%s:%u) failed: errno=%d",
                        inet_ntoa(ip), (unsigned)port, errno);
        close(fd);
        return -1;
    }

    close(fd);
    return -1;
}

static bool soc_send_all(int fd, const char *buf, size_t len, int timeout_ms)
{
    size_t sent = 0;
    uint32_t start_ms = nox_3ds_get_ticks_ms();
    while (sent < len && !g_worker_stop) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
            return false;

        uint32_t elapsed = nox_3ds_get_ticks_ms() - start_ms;
        if ((int)elapsed >= timeout_ms)
            return false;

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        poll(&pfd, 1, 200);
    }
    return sent == len;
}

static int soc_http_request(const char *method, const char *initial_url,
                            const char *req_body, char *out_buf, size_t out_cap)
{
    if (!method || !initial_url || !out_buf || out_cap < 2)
        return -1;
    out_buf[0] = '\0';

    if (!nox_3ds_soc_ensure_ready())
        return -1;

    char url[512];
    snprintf(url, sizeof(url), "%s", initial_url);

    size_t raw_cap = out_cap + 4096u;
    char *raw = (char *)malloc(raw_cap);
    if (!raw)
        return -1;

    int final_ret = -1;
    for (int hop = 0; hop <= NOX_3DS_LOBBY_MAX_REDIRECTS && !g_worker_stop; ++hop) {
        bool is_https = false;
        char host[256];
        char path[256];
        uint16_t port = 80;
        if (!parse_http_url(url, &is_https, host, sizeof(host), &port, path, sizeof(path)))
            break;

        if (is_https) {
            free(raw);
            if (strcmp(method, "POST") == 0)
                return httpc_post_json(url, req_body);
            return httpc_get_body(url, out_buf, out_cap);
        }

        struct in_addr ip;
        if (!resolve_ipv4_addr(host, &ip)) {
            nox_3ds_net_log("[lobby3ds] soc DNS resolve failed for %s", host);
            break;
        }

        int fd = soc_connect_with_timeout(ip, port, NOX_3DS_LOBBY_HTTP_TIMEOUT_MS);
        if (fd < 0)
            break;

        char host_hdr[272];
        if (port != 80u)
            snprintf(host_hdr, sizeof(host_hdr), "%s:%u", host, (unsigned)port);
        else
            snprintf(host_hdr, sizeof(host_hdr), "%s", host);

        char req_hdr[1024];
        int req_len = 0;
        size_t body_len = req_body ? strlen(req_body) : 0u;
        if (strcmp(method, "POST") == 0 && body_len > 0) {
            req_len = snprintf(req_hdr, sizeof(req_hdr),
                               "POST %s HTTP/1.1\r\n"
                               "Host: %s\r\n"
                               "User-Agent: nox-decomp-3ds/1\r\n"
                               "Accept: application/json\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: %zu\r\n"
                               "Connection: close\r\n\r\n",
                               path, host_hdr, body_len);
        } else {
            req_len = snprintf(req_hdr, sizeof(req_hdr),
                               "GET %s HTTP/1.1\r\n"
                               "Host: %s\r\n"
                               "User-Agent: nox-decomp-3ds/1\r\n"
                               "Accept: application/json\r\n"
                               "Connection: close\r\n\r\n",
                               path, host_hdr);
        }

        bool send_ok = (req_len > 0 && (size_t)req_len < sizeof(req_hdr)) &&
                       soc_send_all(fd, req_hdr, (size_t)req_len, NOX_3DS_LOBBY_HTTP_TIMEOUT_MS) &&
                       (body_len == 0 || soc_send_all(fd, req_body, body_len, NOX_3DS_LOBBY_HTTP_TIMEOUT_MS));
        if (!send_ok) {
            nox_3ds_net_log("[lobby3ds] soc send failed for %s", url);
            close(fd);
            break;
        }

        size_t raw_len = 0;
        bool headers_parsed = false;
        size_t cached_body_off = 0;
        bool has_content_length = false;
        size_t content_length = 0;
        bool is_chunked = false;
        uint32_t start_ms = nox_3ds_get_ticks_ms();
        while (!g_worker_stop && raw_len + 1u < raw_cap) {
            uint32_t elapsed = nox_3ds_get_ticks_ms() - start_ms;
            if ((int)elapsed >= NOX_3DS_LOBBY_HTTP_TIMEOUT_MS)
                break;

            int slice = NOX_3DS_LOBBY_HTTP_TIMEOUT_MS - (int)elapsed;
            if (slice > 50) slice = 50;

            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, slice);
            if (pr < 0 && errno != EINTR)
                break;

            ssize_t r = recv(fd, raw + raw_len, (raw_cap - 1u) - raw_len, MSG_DONTWAIT);
            if (r > 0) {
                raw_len += (size_t)r;
                raw[raw_len] = '\0';

                if (!headers_parsed) {
                    const char *hdr_end = strstr(raw, "\r\n\r\n");
                    size_t sep_len = 4u;
                    if (!hdr_end) {
                        hdr_end = strstr(raw, "\n\n");
                        sep_len = 2u;
                    }
                    if (hdr_end) {
                        headers_parsed = true;
                        size_t hdrs_len = (size_t)(hdr_end - raw);
                        cached_body_off = hdrs_len + sep_len;

                        char te_val[64];
                        if (find_header_ci(raw, hdrs_len, "Transfer-Encoding", te_val, sizeof(te_val)) &&
                            strncasecmp(te_val, "chunked", 7) == 0) {
                            is_chunked = true;
                        } else {
                            char cl_val[32];
                            if (find_header_ci(raw, hdrs_len, "Content-Length", cl_val, sizeof(cl_val))) {
                                content_length = (size_t)strtoul(cl_val, NULL, 10);
                                has_content_length = true;
                            }
                        }
                    }
                }

                if (headers_parsed) {
                    const char *cur_body = raw + cached_body_off;
                    size_t cur_body_len = raw_len - cached_body_off;
                    if (has_content_length && cur_body_len >= content_length)
                        break;
                    if (is_chunked &&
                        (strstr(cur_body, "\r\n0\r\n") != NULL || strncmp(cur_body, "0\r\n", 3) == 0) &&
                        decode_chunked_body(cur_body, cur_body_len, out_buf, out_cap) >= 0) {
                        break;
                    }
                }
                continue;
            }
            if (r == 0)
                break;
            if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
                break;
            if (pr > 0)
                nox_3ds_sleep_ms(5);
        }
        close(fd);

        if (raw_len == 0 || g_worker_stop) {
            nox_3ds_net_log("[lobby3ds] soc recv empty/stopped for %s", url);
            break;
        }
        raw[raw_len] = '\0';

        const char *hdr_end = strstr(raw, "\r\n\r\n");
        size_t sep_len = 4u;
        if (!hdr_end) {
            hdr_end = strstr(raw, "\n\n");
            sep_len = 2u;
        }
        if (!hdr_end)
            break;

        const char *sp = strchr(raw, ' ');
        int status = sp ? atoi(sp + 1) : 0;
        size_t hdrs_len = (size_t)(hdr_end - raw);
        const char *body = hdr_end + sep_len;
        size_t recv_body_len = raw_len - (size_t)(body - raw);

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            char location[512];
            char next_url[512];
            if (!find_header_ci(raw, hdrs_len, "Location", location, sizeof(location)) ||
                !build_redirect_url(url, location, next_url, sizeof(next_url)))
                break;
            snprintf(url, sizeof(url), "%s", next_url);
            continue;
        }

        if (status < 200 || status >= 300) {
            nox_3ds_net_log("[lobby3ds] soc HTTP %d from %s", status, url);
            break;
        }

        char te_val[64];
        if (find_header_ci(raw, hdrs_len, "Transfer-Encoding", te_val, sizeof(te_val)) &&
            strncasecmp(te_val, "chunked", 7) == 0) {
            final_ret = decode_chunked_body(body, recv_body_len, out_buf, out_cap);
        } else {
            size_t copy_len = recv_body_len;
            char cl_val[32];
            if (find_header_ci(raw, hdrs_len, "Content-Length", cl_val, sizeof(cl_val))) {
                unsigned long cl = strtoul(cl_val, NULL, 10);
                if (cl > 0 && cl < copy_len)
                    copy_len = (size_t)cl;
            }
            if (copy_len >= out_cap)
                copy_len = out_cap - 1u;
            memcpy(out_buf, body, copy_len);
            out_buf[copy_len] = '\0';
            final_ret = (int)copy_len;
        }
        break;
    }

    free(raw);
    return final_ret;
}

static int http_get_body(const char *url, char *out_buf, size_t out_cap)
{
    if (!url || !out_buf || out_cap < 2)
        return -1;
    if (strncasecmp(url, "http://", 7) == 0) {
        int n = soc_http_request("GET", url, NULL, out_buf, out_cap);
        if (n > 0)
            return n;
        if (g_worker_stop)
            return -1;
        nox_3ds_net_log("[lobby3ds] soc GET failed for %s; falling back to http:C", url);
    }
    return httpc_get_body(url, out_buf, out_cap);
}

static int http_post_json(const char *url, const char *json_body)
{
    if (!url || !json_body)
        return -1;
    if (strncasecmp(url, "http://", 7) == 0) {
        char resp_tmp[1024];
        int r = soc_http_request("POST", url, json_body, resp_tmp, sizeof(resp_tmp));
        if (r >= 0)
            return 0;
        if (g_worker_stop)
            return -1;
    }
    return httpc_post_json(url, json_body);
}

/* =========================================================================
 * Background Worker Thread (32 KB Stack; Heap-Allocated 128 KB JSON Buffer)
 * ========================================================================= */
static void worker_do_fetch_list(char *json_buf,
                                 size_t json_cap,
                                 lobby_cached_row_t *parsed,
                                 lobby_cached_row_t *resolved)
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

    size_t parsed_count = 0;
    size_t resolved_count = 0;
    bool got_valid_response = false;
    char good_base[256] = {0};

    for (size_t i = 0; i < ep_count && !g_worker_stop; ++i) {
        char full_url[320];
        snprintf(full_url, sizeof(full_url), "%s/api/v0/games/list", endpoints[i].base_url);
        nox_3ds_net_log("[lobby3ds] Fetching %s", full_url);

        int n = http_get_body(full_url, json_buf, json_cap);
        if (n <= 0)
            continue;

        /* Quick sanity check before invoking cJSON */
        const char *p = json_buf;
        while (*p && (unsigned char)*p <= ' ') p++;
        if (*p != '{' && *p != '[') {
            nox_3ds_net_log("[lobby3ds] Non-JSON response from %s", full_url);
            continue;
        }

        parsed_count = lobby_parse_games_json(json_buf, parsed, NOX_3DS_LOBBY_MAX_SERVERS);
        /* Pre-resolve all server IPv4 addresses strictly on this background thread
         * so the 30 Hz main thread never blocks on DNS or holds g_lobby_lock during DNS. */
        resolved_count = 0;
        for (size_t j = 0; j < parsed_count && !g_worker_stop; ++j) {
            struct in_addr srv_ip;
            if (!resolve_ipv4_addr(parsed[j].row.addr, &srv_ip)) {
                nox_3ds_net_log("[lobby3ds] Skip unresolvable server '%s' (%s)",
                                parsed[j].row.name, parsed[j].row.addr);
                continue;
            }
            resolved[resolved_count] = parsed[j];
            resolved[resolved_count].resolved_ip = srv_ip;
            resolved_count++;
        }

        got_valid_response = true;
        snprintf(good_base, sizeof(good_base), "%s", endpoints[i].base_url);
        nox_3ds_net_log("[lobby3ds] Parsed %zu server(s) (%zu resolved) from %s (%d bytes)",
                        parsed_count, resolved_count, endpoints[i].base_url, n);
        if (resolved_count > 0)
            break;
    }

    struct sockaddr_in ping_addrs[NOX_3DS_LOBBY_MAX_SERVERS];
    size_t ping_count = 0;
    int ping_sockfd = -1;
    uint8_t ping_cookie[4] = {0};

    uint32_t now_ms = nox_3ds_get_ticks_ms();
    RecursiveLock_Lock(&g_lobby_lock);
    if (got_valid_response) {
        memcpy(g_srv_cache, resolved, resolved_count * sizeof(lobby_cached_row_t));
        g_srv_cache_count = resolved_count;
        g_cache_valid = true;
        g_cache_consumed_by_ping = false;
        g_cache_last_ok_ms = now_ms ? now_ms : 1u;
        g_cache_next_try_ms = 0;
        snprintf(g_last_good_base_url, sizeof(g_last_good_base_url), "%s", good_base);

        /* Immediately inject into the 0x0D queue if the player is still in the browser */
        if (nox_3ds_net_get_mode() == NOX_3DS_NET_MODE_WIFI && g_active_ping_valid) {
            ping_count = queue_cached_rows_locked(ping_addrs, NOX_3DS_LOBBY_MAX_SERVERS,
                                                  &ping_sockfd, ping_cookie);
            g_cache_consumed_by_ping = true;
        }
    } else {
        g_cache_next_try_ms = now_ms + NOX_3DS_LOBBY_RETRY_BACKOFF_MS;
        nox_3ds_net_log("[lobby3ds] All lobby endpoints failed; backing off %u ms",
                        (unsigned)NOX_3DS_LOBBY_RETRY_BACKOFF_MS);
    }
    g_fetch_inflight = false;
    RecursiveLock_Unlock(&g_lobby_lock);

    if (ping_count > 0 && ping_sockfd >= 0) {
        ping_cached_rows_udp(ping_sockfd, ping_cookie, ping_addrs, ping_count);
    }
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
                       "\"vers\":\"v1.9.0-3ds\","
                       "\"port\":%u,"
                       "\"res\":{\"width\":1280,\"height\":800,\"high_res\":true},"
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
        if (http_post_json(url, body) == 0) {
            if (s_last_reg_failed || strcmp(s_last_logged_reg_url, last_good) != 0) {
                snprintf(s_last_logged_reg_url, sizeof(s_last_logged_reg_url), "%s", last_good);
                s_last_reg_failed = false;
                nox_3ds_net_log("[lobby3ds] Registered host '%s' (port=%u) on %s",
                                job->name, port, last_good);
            }
            return;
        }
    }

    bool disabled = false;
    lobby_endpoint_t endpoints[NOX_3DS_LOBBY_MAX_ENDPOINTS];
    size_t ep_count = lobby_load_endpoints(endpoints, NOX_3DS_LOBBY_MAX_ENDPOINTS, &disabled);
    if (disabled) return;

    for (size_t i = 0; i < ep_count && !g_worker_stop; ++i) {
        snprintf(url, sizeof(url), "%s/api/v0/games/register", endpoints[i].base_url);
        if (http_post_json(url, body) == 0) {
            RecursiveLock_Lock(&g_lobby_lock);
            snprintf(g_last_good_base_url, sizeof(g_last_good_base_url), "%s", endpoints[i].base_url);
            RecursiveLock_Unlock(&g_lobby_lock);
            if (s_last_reg_failed || strcmp(s_last_logged_reg_url, endpoints[i].base_url) != 0) {
                snprintf(s_last_logged_reg_url, sizeof(s_last_logged_reg_url), "%s", endpoints[i].base_url);
                s_last_reg_failed = false;
                nox_3ds_net_log("[lobby3ds] Registered host '%s' (port=%u) on %s",
                                job->name, port, endpoints[i].base_url);
            }
            return;
        }
    }
    if (!s_last_reg_failed) {
        s_last_reg_failed = true;
        s_last_logged_reg_url[0] = '\0';
        nox_3ds_net_log("[lobby3ds] Host registration failed for '%s'", job->name);
    }
}

static void lobby_worker_main(void *arg)
{
    (void)arg;
    char *json_buf = (char *)malloc(NOX_3DS_LOBBY_JSON_CAP);
    lobby_cached_row_t *parsed = (lobby_cached_row_t *)malloc(NOX_3DS_LOBBY_MAX_SERVERS * sizeof(lobby_cached_row_t));
    lobby_cached_row_t *resolved = (lobby_cached_row_t *)malloc(NOX_3DS_LOBBY_MAX_SERVERS * sizeof(lobby_cached_row_t));
    if (!json_buf || !parsed || !resolved) {
        free(json_buf);
        free(parsed);
        free(resolved);
        printf("[lobby3ds] OOM allocating worker buffers\n");
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
            worker_do_fetch_list(json_buf, NOX_3DS_LOBBY_JSON_CAP, parsed, resolved);
            continue;
        }
        if (reg_copy.pending) {
            worker_do_host_register(&reg_copy);
            continue;
        }

        /* Block with zero periodic wakeup until signaled by main thread or shutdown. */
        LightEvent_Wait(&g_lobby_event);
        if (g_worker_stop)
            break;
    }

    free(resolved);
    free(parsed);
    free(json_buf);
}

static bool ensure_worker_started_locked(void)
{
    if (g_worker_started)
        return true;

    s32 main_prio = 0x30;
    svcGetThreadPriority(&main_prio, CUR_THREAD_HANDLE);
    /* Run HTTP worker strictly lower priority than Core 0 main thread (and prefer Core 2
     * on New 3DS) so it never preempts active GUI/socket teardown. */
    s32 worker_prio = (main_prio + 2 <= 0x3E) ? (main_prio + 2) : 0x3E;
    if (worker_prio < 0x18)
        worker_prio = 0x18;

    g_worker_stop = false;
    bool n3ds = nox_3ds_get_state()->is_new_3ds;
    g_worker_thread = n3ds
        ? threadCreate(lobby_worker_main, NULL, NOX_3DS_LOBBY_WORKER_STACK, worker_prio, 2, false)
        : NULL;
    if (!g_worker_thread) {
        g_worker_thread = threadCreate(lobby_worker_main, NULL,
                                       NOX_3DS_LOBBY_WORKER_STACK,
                                       worker_prio, -2, false);
    }
    if (!g_worker_thread) {
        nox_3ds_net_log("[lobby3ds] threadCreate() failed");
        return false;
    }
    g_worker_started = true;
    nox_3ds_net_log("[lobby3ds] Background HTTP worker started (stack=64KB prio=0x%lX main=0x%lX)",
                    (unsigned long)worker_prio, (unsigned long)main_prio);
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
    nox_3ds_net_log("[boot] version=%s", NOX_3DS_VERSION_STR);
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
        s_last_logged_reg_url[0] = '\0';
        s_last_reg_failed = false;
        RecursiveLock_Unlock(&g_lobby_lock);
        dns_cache_clear();
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
    s_last_logged_reg_url[0] = '\0';
    s_last_reg_failed = false;
    RecursiveLock_Unlock(&g_lobby_lock);
    dns_cache_clear();

    g_lobby_inited = false;
}

/* =========================================================================
 * Public Prefetch & Async Discovery Status Helpers (port/3ds/platform_3ds.h)
 * ========================================================================= */
void nox_3ds_lobby_prefetch(void)
{
    if (nox_3ds_net_get_mode() != NOX_3DS_NET_MODE_WIFI)
        return;

    if (!g_lobby_inited)
        nox_3ds_lobby_init();

    uint32_t now_ms = nox_3ds_get_ticks_ms();

    RecursiveLock_Lock(&g_lobby_lock);
    bool cache_fresh = g_cache_valid &&
                       g_cache_last_ok_ms != 0 &&
                       (now_ms - g_cache_last_ok_ms) < NOX_3DS_LOBBY_CACHE_TTL_MS;

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

bool nox_3ds_lobby_has_pending_work(void)
{
    if (!g_lobby_inited || nox_3ds_net_get_mode() != NOX_3DS_NET_MODE_WIFI)
        return false;

    RecursiveLock_Lock(&g_lobby_lock);
    bool pending = g_fetch_requested || g_fetch_inflight;
    if (!pending) {
        for (int i = 0; i < NOX_3DS_LOBBY_FAKE_QUEUE_CAP; ++i) {
            if (g_fake_packets[i].used) {
                pending = true;
                break;
            }
        }
    }
    RecursiveLock_Unlock(&g_lobby_lock);
    return pending;
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

    bool cache_usable = g_cache_valid &&
                        g_cache_last_ok_ms != 0 &&
                        (now_ms - g_cache_last_ok_ms) < NOX_3DS_LOBBY_CACHE_TTL_MS;
    bool skip_refetch = cache_usable &&
                        (!g_cache_consumed_by_ping ||
                         (now_ms - g_cache_last_ok_ms) < NOX_3DS_LOBBY_REPING_TTL_MS);

    struct sockaddr_in ping_addrs[NOX_3DS_LOBBY_MAX_SERVERS];
    size_t ping_count = 0;
    int ping_sockfd = -1;
    uint8_t ping_cookie[4] = {0};

    if (cache_usable && g_srv_cache_count > 0) {
        /* Immediately populate native browser with cached rows (zero wait) */
        ping_count = queue_cached_rows_locked(ping_addrs, NOX_3DS_LOBBY_MAX_SERVERS,
                                              &ping_sockfd, ping_cookie);
        g_cache_consumed_by_ping = true;
    }

    if (!skip_refetch && !g_fetch_inflight && !g_fetch_requested) {
        if (g_cache_next_try_ms == 0 || (int32_t)(now_ms - g_cache_next_try_ms) >= 0) {
            if (ensure_worker_started_locked()) {
                g_fetch_requested = true;
                LightEvent_Signal(&g_lobby_event);
            }
        }
    }

    RecursiveLock_Unlock(&g_lobby_lock);

    if (ping_count > 0 && ping_sockfd >= 0) {
        ping_cached_rows_udp(ping_sockfd, ping_cookie, ping_addrs, ping_count);
    }
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

void nox_3ds_set_host_client_proto_ver(uint32_t ver)
{
    if (NOX_IS_COMPAT_PROTO_VER(ver))
        g_host_client_proto_ver = ver;
}

uint32_t nox_3ds_get_host_client_proto_ver(void)
{
    return NOX_IS_COMPAT_PROTO_VER(g_host_client_proto_ver)
               ? g_host_client_proto_ver
               : NOX_PROTO_VER_LEGACY;
}

void nox_3ds_lobby_on_host_close(int sockfd)
{
    if (g_lobby_inited) {
        RecursiveLock_Lock(&g_lobby_lock);
        g_host_active = false;
        g_host_reg_job.pending = false;
        s_last_logged_reg_url[0] = '\0';
        s_last_reg_failed = false;
        RecursiveLock_Unlock(&g_lobby_lock);
    }
    nox_3ds_net_log("[host3ds] Host UDP socket closed (vfd=%d)", sockfd);
}

void nox_netextras_on_host_bind_success(int sockfd, unsigned bound_port)
{
    (void)sockfd;
    g_host_client_proto_ver = NOX_PROTO_VER_LEGACY;
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
    s_last_logged_reg_url[0] = '\0';
    s_last_reg_failed = false;
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
    if (g_host_active && !g_host_reg_job.pending) {
        bool due = (g_host_last_reg_ms != 0)
            ? ((now_ms - g_host_last_reg_ms) >= NOX_3DS_LOBBY_REG_PERIOD_MS)
            : (g_host_last_snapshot_ms == 0 || (now_ms - g_host_last_snapshot_ms) >= 2000u);
        if (due) {
            g_host_last_snapshot_ms = now_ms ? now_ms : 1u;
            do_snapshot = true;
        }
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
