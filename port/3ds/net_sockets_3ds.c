/*
 * Real Wi-Fi socket transport for New 3DS via libctru soc:u service.
 *
 * Design guarantees:
 *   - Lazy socInit(): never called at boot or in Solo Campaign / Solo Quest.
 *     Initialized strictly on first Multiplayer socket/DNS call after verifying
 *     Wi-Fi availability via ACU_GetWifiStatus().
 *   - 1 MB (0x100000) context buffer allocated via memalign(0x1000, 0x100000)
 *     on standard FCRAM heap (preserving scarce linear heap for PICA200/ndsp).
 *   - Strictly non-blocking sockets: O_NONBLOCK via fcntl() at socket() time
 *     plus defensive MSG_DONTWAIT on every sendto/recvfrom/send/recv, with
 *     poll(&pfd, 1, 0) emulating Winsock FIONREAD (0x4004667F).
 *   - Clean teardown in shutdown(): close all active real FDs -> socExit() ->
 *     free(g_soc_buffer) (strictly after socExit(), since Horizon OS revokes
 *     userland permissions on the buffer while soc:u is active).
 *
 * Exports only g_nox_3ds_sockets_ops + nox_3ds_soc_is_ready() +
 * nox_3ds_net_on_apt_suspend(). Does NOT include src/windows.h so libctru
 * BSD socket symbols are called directly without macro remapping.
 */
#include "platform_3ds.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SOC_ALIGN               0x1000u
#define SOC_BUFFERSIZE          0x100000u /* 1 MB page-aligned FCRAM context buffer */
#define MAX_SOC_SOCKS           NOX_3DS_SOCKETS_FD_MAX
#define SOCKETS_FD_BASE         NOX_3DS_SOCKETS_FD_BASE
#define SOC_MAX_DRAIN_PER_TICK  256u
#define SOC_DRAIN_WINDOW_MS     16u       /* Half of a 30 Hz Nox tick (~33 ms) */
/* libctru sys/socket.h rejects any send/recv flag > 0x4 with EOPNOTSUPP. */
#define SOC_SAFE_MSG_FLAGS(f)   (((f) & (MSG_OOB | MSG_PEEK)) | MSG_DONTWAIT)

#define NOX_FIONREAD            0x4004667Fu
#define NOX_FIONBIO             0x8004667Eu

#define NOX_WSAEINTR            10004u
#define NOX_WSAEBADF            10009u
#define NOX_WSAEINVAL           10022u
#define NOX_WSAEMFILE           10024u
#define NOX_WSAEWOULDBLOCK      10035u
#define NOX_WSAEINPROGRESS      10036u
#define NOX_WSAEALREADY         10037u
#define NOX_WSAENOTSOCK         10038u
#define NOX_WSAEADDRINUSE       10048u
#define NOX_WSAENETDOWN         10050u
#define NOX_WSAENETUNREACH      10051u
#define NOX_WSAENETRESET        10052u
#define NOX_WSAECONNRESET       10054u
#define NOX_WSAENOBUFS          10055u
#define NOX_WSAENOTCONN         10057u
#define NOX_WSAETIMEDOUT        10060u
#define NOX_WSAECONNREFUSED     10061u
#define NOX_WSAEHOSTUNREACH     10065u

typedef struct {
    bool     in_use;
    int      real_fd;
    int      type;
    bool     nonblocking;
    bool     broadcast;
    uint16_t bound_port;
    uint32_t last_rx_tick_ms;
    uint32_t rx_count_this_tick;
} soc_slot_t;

extern unsigned int last_socket_error;

static soc_slot_t        g_soc_slots[MAX_SOC_SOCKS];
static nox_3ds_mutex_t  *g_soc_mutex     = NULL;
static RecursiveLock     g_dns_lock;
static u32              *g_soc_buffer    = NULL;
static bool              g_soc_inited    = false;
static volatile bool     g_soc_suspended = false;

static unsigned int wsa_from_errno(int e)
{
    switch (e) {
#if defined(EWOULDBLOCK)
    case EWOULDBLOCK:
#endif
#if defined(EAGAIN) && (!defined(EWOULDBLOCK) || (EAGAIN != EWOULDBLOCK))
    case EAGAIN:
#endif
        return NOX_WSAEWOULDBLOCK;
    case EINPROGRESS:   return NOX_WSAEINPROGRESS;
    case EALREADY:      return NOX_WSAEALREADY;
    case EINTR:         return NOX_WSAEINTR;
    case EBADF:         return NOX_WSAEBADF;
    case EINVAL:        return NOX_WSAEINVAL;
    case EMFILE:        return NOX_WSAEMFILE;
    case EADDRINUSE:    return NOX_WSAEADDRINUSE;
    case ENETDOWN:      return NOX_WSAENETDOWN;
    case ENETUNREACH:   return NOX_WSAENETUNREACH;
    case ENETRESET:     return NOX_WSAENETRESET;
    case ECONNRESET:    return NOX_WSAECONNRESET;
    case ENOBUFS:       return NOX_WSAENOBUFS;
    case ENOTCONN:      return NOX_WSAENOTCONN;
    case ETIMEDOUT:     return NOX_WSAETIMEDOUT;
    case ECONNREFUSED:  return NOX_WSAECONNREFUSED;
    case EHOSTUNREACH:  return NOX_WSAEHOSTUNREACH;
    default:            return (unsigned int)(10000 + e);
    }
}

static void soc_lock(void)
{
    if (!g_soc_mutex)
        g_soc_mutex = nox_3ds_mutex_create();
    if (g_soc_mutex)
        nox_3ds_mutex_lock(g_soc_mutex);
}

static void soc_unlock(void)
{
    if (g_soc_mutex)
        nox_3ds_mutex_unlock(g_soc_mutex);
}

static soc_slot_t *lookup_slot(int vfd)
{
    int idx = vfd - SOCKETS_FD_BASE;
    if (idx < 0 || idx >= MAX_SOC_SOCKS)
        return NULL;
    if (!g_soc_slots[idx].in_use || g_soc_slots[idx].real_fd < 0)
        return NULL;
    return &g_soc_slots[idx];
}

static bool soc_ensure_init_locked(void)
{
    if (g_soc_inited)
        return true;

    /* Probe Wi-Fi status for diagnostics, but do not hard-abort on wifi_status==0:
     * emulators (Citra/Azahar/Lime3DS) and some CFW states report 0 even when
     * socInit() and sockets succeed. True Airplane Mode still fails in socInit(). */
    u32 wifi_status = 0;
    if (R_SUCCEEDED(acInit())) {
        Result wrc = ACU_GetWifiStatus(&wifi_status);
        acExit();
        if (R_FAILED(wrc) || wifi_status == 0u) {
            nox_3ds_net_log("[net3ds:soc] WARN: ACU_GetWifiStatus rc=0x%08X status=%u; attempting socInit anyway",
                            (unsigned)wrc, (unsigned)wifi_status);
        }
    }

    g_soc_buffer = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!g_soc_buffer) {
        nox_3ds_net_log("[net3ds:soc] ERROR: memalign(0x%x, 0x%x) failed",
                        (unsigned)SOC_ALIGN, (unsigned)SOC_BUFFERSIZE);
        last_socket_error = NOX_WSAENOBUFS;
        errno = ENOMEM;
        return false;
    }

    Result rc = socInit(g_soc_buffer, SOC_BUFFERSIZE);
    if (R_FAILED(rc)) {
        nox_3ds_net_log("[net3ds:soc] ERROR: socInit failed (rc=0x%08X)", (unsigned)rc);
        free(g_soc_buffer);
        g_soc_buffer = NULL;
        last_socket_error = NOX_WSAENETDOWN;
        errno = ENETDOWN;
        return false;
    }

    RecursiveLock_Init(&g_dns_lock);

    for (int i = 0; i < MAX_SOC_SOCKS; ++i) {
        g_soc_slots[i].in_use = false;
        g_soc_slots[i].real_fd = -1;
    }

    g_soc_inited = true;
    g_soc_suspended = false;
    nox_3ds_state_mut()->soc_ready = true;

    uint32_t ip = 0, mask = 0, bcast = 0;
    nox_3ds_soc_get_ip_info(&ip, &mask, &bcast);
    nox_3ds_net_log("[net3ds:soc] socInit OK (%u KB memalign buf, IP=%u.%u.%u.%u mask=%u.%u.%u.%u bcast=%u.%u.%u.%u)",
                    (unsigned)(SOC_BUFFERSIZE / 1024u),
                    (unsigned)(ip & 0xFFu), (unsigned)((ip >> 8) & 0xFFu),
                    (unsigned)((ip >> 16) & 0xFFu), (unsigned)((ip >> 24) & 0xFFu),
                    (unsigned)(mask & 0xFFu), (unsigned)((mask >> 8) & 0xFFu),
                    (unsigned)((mask >> 16) & 0xFFu), (unsigned)((mask >> 24) & 0xFFu),
                    (unsigned)(bcast & 0xFFu), (unsigned)((bcast >> 8) & 0xFFu),
                    (unsigned)((bcast >> 16) & 0xFFu), (unsigned)((bcast >> 24) & 0xFFu));
    return true;
}

bool nox_3ds_soc_get_ip_info(uint32_t *out_ip_be, uint32_t *out_mask_be, uint32_t *out_bcast_be)
{
    if (!g_soc_inited) {
        if (out_ip_be)    *out_ip_be = 0;
        if (out_mask_be)  *out_mask_be = 0;
        if (out_bcast_be) *out_bcast_be = 0;
        return false;
    }

    struct in_addr ip_a = {0}, mask_a = {0}, bcast_a = {0};
    int rc = SOCU_GetIPInfo(&ip_a, &mask_a, &bcast_a);
    uint32_t ip = (uint32_t)ip_a.s_addr;
    uint32_t mask = (uint32_t)mask_a.s_addr;
    uint32_t bcast = (uint32_t)bcast_a.s_addr;
    uint32_t hid_be = (uint32_t)gethostid();

    if (rc != 0 || ip == 0u) {
        ip = hid_be;
        mask = htonl(0xFFFFFF00u);
        bcast = (ip != 0u) ? ((ip & mask) | ~mask) : htonl(INADDR_BROADCAST);
    } else {
        /* Horizon soc:u SOCU_GetIPInfo returns IPv4/mask/bcast in host (little-endian)
         * byte order whereas libctru gethostid() is guaranteed network byte order. */
        bool host_order = false;
        if (hid_be != 0u) {
            if (ip != hid_be && htonl(ip) == hid_be)
                host_order = true;
        } else {
            uint8_t b0 = (uint8_t)(ip & 0xFFu);
            uint8_t b3 = (uint8_t)((ip >> 24) & 0xFFu);
            if (b0 != 10u && b0 != 172u && b0 != 192u &&
                (b3 == 10u || b3 == 172u || b3 == 192u)) {
                host_order = true;
            }
        }
        if (host_order) {
            ip    = htonl(ip);
            mask  = htonl(mask);
            bcast = htonl(bcast);
        }

        if (mask == 0u || mask == 0xFFFFFFFFu)
            mask = htonl(0xFFFFFF00u);

        uint32_t calc_bcast = (ip & mask) | ~mask;
        if (bcast == 0u || bcast == htonl(INADDR_BROADCAST) || (bcast & mask) != (ip & mask))
            bcast = calc_bcast;
    }

    if (out_ip_be)    *out_ip_be = ip;
    if (out_mask_be)  *out_mask_be = mask;
    if (out_bcast_be) *out_bcast_be = bcast;
    return ip != 0u;
}

bool nox_3ds_soc_is_ready(void)
{
    return g_soc_inited;
}

bool nox_3ds_soc_ensure_ready(void)
{
    if (nox_3ds_net_get_mode() != NOX_3DS_NET_MODE_WIFI)
        return false;
    soc_lock();
    bool ok = soc_ensure_init_locked();
    soc_unlock();
    return ok;
}

bool nox_3ds_soc_resolve_ipv4(const char *hostname, uint32_t *out_ip_be)
{
    if (!hostname || !*hostname || !out_ip_be || !g_soc_inited)
        return false;

    RecursiveLock_Lock(&g_dns_lock);
    struct hostent *res = gethostbyname(hostname);
    bool ok = false;
    if (res && res->h_addr_list && res->h_addr_list[0] && res->h_length == 4) {
        memcpy(out_ip_be, res->h_addr_list[0], 4);
        ok = true;
    }
    RecursiveLock_Unlock(&g_dns_lock);
    return ok;
}

void nox_3ds_net_on_apt_suspend(bool suspended)
{
    g_soc_suspended = suspended;
}

static void sockets_init(void)
{
    /* Deliberately no-op: socInit() is strictly lazy on Multiplayer menu entry. */
}

static void sockets_shutdown(void)
{
    if (!g_soc_inited && !g_soc_buffer && !g_soc_mutex)
        return;

    if (g_soc_mutex)
        nox_3ds_mutex_lock(g_soc_mutex);

    for (int i = 0; i < MAX_SOC_SOCKS; ++i) {
        if (g_soc_slots[i].in_use && g_soc_slots[i].real_fd >= 0) {
            close(g_soc_slots[i].real_fd);
        }
        g_soc_slots[i].in_use = false;
        g_soc_slots[i].real_fd = -1;
    }

    if (g_soc_inited) {
        socExit();
        g_soc_inited = false;
        nox_3ds_state_mut()->soc_ready = false;
        printf("[net3ds:soc] socExit() complete\n");
    }

    /* Strictly free buffer ONLY after socExit() restores userland page perms. */
    if (g_soc_buffer) {
        free(g_soc_buffer);
        g_soc_buffer = NULL;
    }

    if (g_soc_mutex) {
        nox_3ds_mutex_unlock(g_soc_mutex);
        nox_3ds_mutex_destroy(g_soc_mutex);
        g_soc_mutex = NULL;
    }
}

static int sockets_socket(int domain, int type, int protocol)
{
    soc_lock();
    if (!soc_ensure_init_locked()) {
        soc_unlock();
        return -1;
    }

    int slot_idx = -1;
    for (int i = 0; i < MAX_SOC_SOCKS; ++i) {
        if (!g_soc_slots[i].in_use) {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx < 0) {
        last_socket_error = NOX_WSAEMFILE;
        errno = EMFILE;
        soc_unlock();
        return -1;
    }

    int rfd = socket(domain, type, protocol);
    if (rfd < 0) {
        last_socket_error = wsa_from_errno(errno);
        printf("[net3ds:soc] socket(dom=%d, type=%d, proto=%d) FAILED errno=%d wsa=%u\n",
               domain, type, protocol, errno, last_socket_error);
        soc_unlock();
        return -1;
    }

    /* Enforce strictly non-blocking mode immediately so dropped UDP packets
     * or TOCTOU races can NEVER hang the ARM11 30 Hz main loop. */
    int fl = fcntl(rfd, F_GETFL, 0);
    if (fl >= 0) {
        if (fcntl(rfd, F_SETFL, fl | O_NONBLOCK) < 0) {
            printf("[net3ds:soc] WARN: fcntl(O_NONBLOCK) failed on rfd=%d errno=%d\n", rfd, errno);
        }
    }

    soc_slot_t *s = &g_soc_slots[slot_idx];
    memset(s, 0, sizeof(*s));
    s->in_use = true;
    s->real_fd = rfd;
    s->type = type;
    s->nonblocking = true;

    last_socket_error = 0;
    soc_unlock();
    printf("[net3ds:soc] socket(dom=%d, type=%d, proto=%d) -> vfd=%d (rfd=%d, O_NONBLOCK)\n",
           domain, type, protocol, SOCKETS_FD_BASE + slot_idx, rfd);
    return SOCKETS_FD_BASE + slot_idx;
}

static int sockets_closesocket(int vfd)
{
    soc_lock();
    soc_slot_t *s = lookup_slot(vfd);
    if (!s) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        soc_unlock();
        return -1;
    }

    int rfd = s->real_fd;
    s->in_use = false;
    s->real_fd = -1;
    soc_unlock();

    int r = close(rfd);
    if (r < 0) {
        last_socket_error = wsa_from_errno(errno);
        return -1;
    }
    last_socket_error = 0;
    return 0;
}

static int sockets_bind(int vfd, const void *addr, unsigned int addrlen)
{
    if (!addr || addrlen < sizeof(struct sockaddr_in)) {
        last_socket_error = NOX_WSAEINVAL;
        errno = EINVAL;
        return -1;
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    memcpy(&sin, addr, sizeof(sin));
    sin.sin_family = AF_INET;
    memset(sin.sin_zero, 0, sizeof(sin.sin_zero));

    soc_lock();
    soc_slot_t *s = lookup_slot(vfd);
    if (!s) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        soc_unlock();
        return -1;
    }
    int rfd = s->real_fd;
    int stype = s->type;
    soc_unlock();

    int r = bind(rfd, (const struct sockaddr *)&sin, (socklen_t)sizeof(sin));
    if (r < 0 && stype == SOCK_DGRAM && sin.sin_port == 0) {
        static uint16_t s_next_ephem = 49152u;
        int orig_errno = errno;
        for (uint16_t k = 0; k < 64u; ++k) {
            uint16_t ephem = (uint16_t)(49152u + (((s_next_ephem - 49152u) + k) & 63u));
            sin.sin_port = htons(ephem);
            r = bind(rfd, (const struct sockaddr *)&sin, (socklen_t)sizeof(sin));
            if (r == 0) {
                s_next_ephem = (uint16_t)(49152u + (((ephem - 49152u) + 1u) & 63u));
                nox_3ds_net_log("[net3ds:soc] bind(port=0) fallback succeeded on ephemeral UDP port %u",
                                (unsigned)ephem);
                break;
            }
        }
        if (r < 0) {
            sin.sin_port = 0;
            errno = orig_errno;
        }
    }
    if (r < 0) {
        last_socket_error = wsa_from_errno(errno);
        nox_3ds_net_log("[net3ds:soc] bind(vfd=%d, rfd=%d, port=%u) FAILED errno=%d wsa=%u",
                        vfd, rfd, (unsigned)ntohs(sin.sin_port), errno, last_socket_error);
        return -1;
    }

    soc_lock();
    s = lookup_slot(vfd);
    uint16_t bport = ntohs(sin.sin_port);
    if (s)
        s->bound_port = bport;
    soc_unlock();

    last_socket_error = 0;
    if (bport == 18590u || bport < 49152u) {
        nox_3ds_net_log("[net3ds:soc] bind(vfd=%d, rfd=%d, port=%u) OK",
                        vfd, rfd, (unsigned)bport);
    }
    printf("[net3ds:soc] bind(vfd=%d, rfd=%d, port=%u) -> ok\n",
           vfd, rfd, (unsigned)bport);
    return 0;
}

static int sockets_setsockopt(int vfd, int level, int opt, const void *val, unsigned int len)
{
    soc_lock();
    soc_slot_t *s = lookup_slot(vfd);
    if (!s) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        soc_unlock();
        return -1;
    }
    int rfd = s->real_fd;
    soc_unlock();

    int orig_level = level;
    if (level == 0xFFFF && opt == 0x20) {
        /* libctru defines SO_BROADCAST as 0x0000 (broadcast is always enabled on soc:u).
         * Short-circuit here to avoid a guaranteed-failing setsockopt() IPC. */
        if (val && len >= sizeof(int)) {
            int on = 0;
            memcpy(&on, val, sizeof(int));
            soc_lock();
            s = lookup_slot(vfd);
            if (s)
                s->broadcast = (on != 0);
            soc_unlock();
        }
        last_socket_error = 0;
        return 0;
    }

    int r = setsockopt(rfd, level, opt, val, (socklen_t)len);
    if (r < 0) {
        /* Gracefully absorb unsupported Windows/BSD options (matching src/compat.c:629)
         * so LAN/discovery socket setup never aborts on soc:u quirks. */
        if (orig_level == 0xFFFF || errno == ENOPROTOOPT || errno == EINVAL) {
            printf("[net3ds:soc] setsockopt(vfd=%d, lvl=%d, opt=%d) unsupported (errno=%d); ignoring\n",
                   vfd, level, opt, errno);
            last_socket_error = 0;
            return 0;
        }
        last_socket_error = wsa_from_errno(errno);
        return -1;
    }

    if (level == SOL_SOCKET && opt == SO_BROADCAST && val && len >= sizeof(int)) {
        int on = 0;
        memcpy(&on, val, sizeof(int));
        soc_lock();
        s = lookup_slot(vfd);
        if (s)
            s->broadcast = (on != 0);
        soc_unlock();
    }

    last_socket_error = 0;
    return 0;
}

static int sockets_ioctlsocket(int vfd, long cmd, unsigned long *argp)
{
    uint32_t ucmd = (uint32_t)cmd;
    if (!argp) {
        last_socket_error = NOX_WSAEINVAL;
        errno = EINVAL;
        return -1;
    }

    soc_lock();
    soc_slot_t *s = lookup_slot(vfd);
    if (!s) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        soc_unlock();
        return -1;
    }
    int rfd = s->real_fd;
    int stype = s->type;
    bool capped = false;
    if (s->rx_count_this_tick > 0u) {
        uint32_t now_ms = nox_3ds_get_ticks_ms();
        if ((uint32_t)(now_ms - s->last_rx_tick_ms) >= SOC_DRAIN_WINDOW_MS) {
            s->last_rx_tick_ms = now_ms;
            s->rx_count_this_tick = 0;
        } else {
            capped = (s->rx_count_this_tick >= SOC_MAX_DRAIN_PER_TICK);
        }
    }
    soc_unlock();

    if (ucmd == NOX_FIONBIO) {
        /* Always keep UDP sockets non-blocking on ARM11 to prevent thread hangs. */
        int nonblock = (stype == SOCK_DGRAM) ? 1 : (*argp != 0ul ? 1 : 0);
        int fl = fcntl(rfd, F_GETFL, 0);
        if (fl >= 0) {
            if (nonblock) fl |= O_NONBLOCK;
            else          fl &= ~O_NONBLOCK;
            (void)fcntl(rfd, F_SETFL, fl);
        }
        soc_lock();
        s = lookup_slot(vfd);
        if (s)
            s->nonblocking = (nonblock != 0);
        soc_unlock();
        last_socket_error = 0;
        return 0;
    }

    if (ucmd == NOX_FIONREAD) {
        if (g_soc_suspended || capped) {
            *argp = 0ul;
            last_socket_error = 0;
            return 0;
        }

        /* libctru lacks <sys/ioctl.h> / ioctl(FIONREAD); emulate via zero-timeout poll(). */
        struct pollfd pfd;
        pfd.fd = rfd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, 0);
        if (pr < 0) {
            last_socket_error = wsa_from_errno(errno);
            *argp = 0ul;
            return -1;
        }
        if (pr == 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) || !(pfd.revents & POLLIN)) {
            *argp = 0ul;
            last_socket_error = 0;
            return 0;
        }

        char peek[1024];
        ssize_t n = recv(rfd, peek, sizeof(peek), MSG_PEEK | MSG_DONTWAIT);
        if (n > 0) {
            *argp = (unsigned long)n;
        } else if (n < 0 && errno == EMSGSIZE) {
            *argp = (unsigned long)sizeof(peek);
        } else {
            *argp = 0ul;
        }
        last_socket_error = 0;
        return 0;
    }

    last_socket_error = NOX_WSAEINVAL;
    errno = EINVAL;
    return -1;
}

static int sockets_sendto(int vfd, const void *buf, unsigned int len, int flags,
                          const void *to, unsigned int tolen)
{
    if (!buf || !to || tolen < sizeof(struct sockaddr_in)) {
        last_socket_error = NOX_WSAEINVAL;
        errno = EINVAL;
        return -1;
    }
    if (g_soc_suspended) {
        last_socket_error = NOX_WSAENETRESET;
        errno = ENETRESET;
        return -1;
    }

    soc_lock();
    soc_slot_t *s = lookup_slot(vfd);
    if (!s) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        soc_unlock();
        return -1;
    }
    int rfd = s->real_fd;
    int stype = s->type;
    soc_unlock();

    struct sockaddr_in dst_sin;
    memcpy(&dst_sin, to, sizeof(dst_sin));
    dst_sin.sin_family = AF_INET;
    memset(dst_sin.sin_zero, 0, sizeof(dst_sin.sin_zero));

    if (stype == SOCK_DGRAM && dst_sin.sin_addr.s_addr == htonl(INADDR_BROADCAST)) {
        uint32_t ip_be = 0, mask_be = 0, bcast_be = 0;
        ssize_t r_dir = -1;
        int err_dir = 0;
        if (nox_3ds_soc_get_ip_info(&ip_be, &mask_be, &bcast_be) &&
            bcast_be != 0u && bcast_be != htonl(INADDR_BROADCAST)) {
            struct sockaddr_in dir_sin = dst_sin;
            dir_sin.sin_addr.s_addr = bcast_be;
            r_dir = sendto(rfd, buf, (size_t)len, SOC_SAFE_MSG_FLAGS(flags),
                           (const struct sockaddr *)&dir_sin, (socklen_t)sizeof(dir_sin));
            err_dir = errno;
            nox_3ds_net_log("[net3ds:soc] Directed LAN bcast -> %u.%u.%u.%u:%u rc=%d errno=%d",
                            (unsigned)(bcast_be & 0xFFu), (unsigned)((bcast_be >> 8) & 0xFFu),
                            (unsigned)((bcast_be >> 16) & 0xFFu), (unsigned)((bcast_be >> 24) & 0xFFu),
                            (unsigned)ntohs(dst_sin.sin_port), (int)r_dir,
                            (r_dir < 0) ? err_dir : 0);
        }

        ssize_t r_lim = sendto(rfd, buf, (size_t)len, SOC_SAFE_MSG_FLAGS(flags),
                               (const struct sockaddr *)&dst_sin, (socklen_t)sizeof(dst_sin));
        if (r_dir >= 0 || r_lim >= 0) {
            last_socket_error = 0;
            return (int)((r_dir >= 0) ? r_dir : r_lim);
        }
        last_socket_error = wsa_from_errno(errno ? errno : err_dir);
        return -1;
    }

    ssize_t r = sendto(rfd, buf, (size_t)len, SOC_SAFE_MSG_FLAGS(flags),
                       (const struct sockaddr *)&dst_sin, (socklen_t)sizeof(dst_sin));
    if (r < 0) {
        last_socket_error = wsa_from_errno(errno);
        return -1;
    }

    last_socket_error = 0;
    return (int)r;
}

static int sockets_recvfrom(int vfd, void *buf, unsigned int len, int flags,
                            void *from, unsigned int *fromlen)
{
    if (!buf) {
        last_socket_error = NOX_WSAEINVAL;
        errno = EINVAL;
        return -1;
    }
    if (g_soc_suspended) {
        last_socket_error = NOX_WSAEWOULDBLOCK;
        errno = EWOULDBLOCK;
        return -1;
    }

    soc_lock();
    soc_slot_t *s = lookup_slot(vfd);
    if (!s) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        soc_unlock();
        return -1;
    }
    int rfd = s->real_fd;
    int stype = s->type;
    soc_unlock();

    struct sockaddr_in src_sin;
    socklen_t src_len = (socklen_t)sizeof(src_sin);

    ssize_t r = recvfrom(rfd, buf, (size_t)len, SOC_SAFE_MSG_FLAGS(flags),
                         from ? (struct sockaddr *)&src_sin : NULL,
                         from ? &src_len : NULL);
    if (r < 0 && stype == SOCK_DGRAM && (errno == ECONNRESET || errno == ECONNREFUSED)) {
        /* Unconnected UDP sockets may receive ICMP Port Unreachable during LAN scans;
         * retry once and otherwise report EWOULDBLOCK so the drain loop does not abort. */
        src_len = (socklen_t)sizeof(src_sin);
        r = recvfrom(rfd, buf, (size_t)len, SOC_SAFE_MSG_FLAGS(flags),
                     from ? (struct sockaddr *)&src_sin : NULL,
                     from ? &src_len : NULL);
        if (r < 0 && (errno == ECONNRESET || errno == ECONNREFUSED)) {
            last_socket_error = NOX_WSAEWOULDBLOCK;
            errno = EWOULDBLOCK;
            return -1;
        }
    }
    if (r < 0) {
        last_socket_error = wsa_from_errno(errno);
        return -1;
    }

    /* Always restore full 16-byte sockaddr_in + *fromlen=16 for GAME5.c's drain loop. */
    if (from && fromlen && *fromlen >= sizeof(struct sockaddr_in)) {
        src_sin.sin_family = AF_INET;
        memset(src_sin.sin_zero, 0, sizeof(src_sin.sin_zero));
        memcpy(from, &src_sin, sizeof(src_sin));
        *fromlen = (unsigned int)sizeof(struct sockaddr_in);
    }

    soc_lock();
    s = lookup_slot(vfd);
    if (s) {
        uint32_t now_ms = nox_3ds_get_ticks_ms();
        if (s->rx_count_this_tick == 0u ||
            (uint32_t)(now_ms - s->last_rx_tick_ms) >= SOC_DRAIN_WINDOW_MS) {
            s->last_rx_tick_ms = now_ms;
            s->rx_count_this_tick = 1u;
        } else {
            s->rx_count_this_tick++;
        }
    }
    soc_unlock();

    last_socket_error = 0;
    return (int)r;
}

static int sockets_listen(int vfd, int backlog)
{
    soc_lock();
    soc_slot_t *s = lookup_slot(vfd);
    if (!s) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        soc_unlock();
        return -1;
    }
    int rfd = s->real_fd;
    soc_unlock();

    int r = listen(rfd, backlog);
    if (r < 0) {
        last_socket_error = wsa_from_errno(errno);
        return -1;
    }
    last_socket_error = 0;
    return 0;
}

static int sockets_accept(int vfd, void *addr, unsigned int *addrlen)
{
    if (g_soc_suspended) {
        last_socket_error = NOX_WSAEWOULDBLOCK;
        errno = EWOULDBLOCK;
        return -1;
    }

    soc_lock();
    soc_slot_t *s = lookup_slot(vfd);
    if (!s) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        soc_unlock();
        return -1;
    }
    int rfd = s->real_fd;
    soc_unlock();

    struct sockaddr_in peer_sin;
    memset(&peer_sin, 0, sizeof(peer_sin));
    socklen_t peer_len = (socklen_t)sizeof(peer_sin);

    int cfd = accept(rfd, addr ? (struct sockaddr *)&peer_sin : NULL, addr ? &peer_len : NULL);
    if (cfd < 0) {
        last_socket_error = wsa_from_errno(errno);
        return -1;
    }

    int fl = fcntl(cfd, F_GETFL, 0);
    if (fl >= 0)
        (void)fcntl(cfd, F_SETFL, fl | O_NONBLOCK);

    soc_lock();
    int slot_idx = -1;
    for (int i = 0; i < MAX_SOC_SOCKS; ++i) {
        if (!g_soc_slots[i].in_use) {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx < 0) {
        soc_unlock();
        close(cfd);
        last_socket_error = NOX_WSAEMFILE;
        errno = EMFILE;
        return -1;
    }
    soc_slot_t *cs = &g_soc_slots[slot_idx];
    memset(cs, 0, sizeof(*cs));
    cs->in_use = true;
    cs->real_fd = cfd;
    cs->type = SOCK_STREAM;
    cs->nonblocking = true;
    soc_unlock();

    if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        peer_sin.sin_family = AF_INET;
        memset(peer_sin.sin_zero, 0, sizeof(peer_sin.sin_zero));
        memcpy(addr, &peer_sin, sizeof(peer_sin));
        *addrlen = (unsigned int)sizeof(struct sockaddr_in);
    }

    last_socket_error = 0;
    return SOCKETS_FD_BASE + slot_idx;
}

static int sockets_shutdown_sock(int vfd, int how)
{
    soc_lock();
    soc_slot_t *s = lookup_slot(vfd);
    if (!s) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        soc_unlock();
        return -1;
    }
    int rfd = s->real_fd;
    soc_unlock();

    int r = shutdown(rfd, how);
    if (r < 0) {
        last_socket_error = wsa_from_errno(errno);
        return -1;
    }
    last_socket_error = 0;
    return 0;
}

static int sockets_send(int vfd, const void *buf, unsigned int len, int flags)
{
    if (!buf) {
        last_socket_error = NOX_WSAEINVAL;
        errno = EINVAL;
        return -1;
    }
    if (g_soc_suspended) {
        last_socket_error = NOX_WSAENETRESET;
        errno = ENETRESET;
        return -1;
    }

    soc_lock();
    soc_slot_t *s = lookup_slot(vfd);
    if (!s) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        soc_unlock();
        return -1;
    }
    int rfd = s->real_fd;
    soc_unlock();

    ssize_t r = send(rfd, buf, (size_t)len, SOC_SAFE_MSG_FLAGS(flags));
    if (r < 0) {
        last_socket_error = wsa_from_errno(errno);
        return -1;
    }
    last_socket_error = 0;
    return (int)r;
}

static int sockets_recv(int vfd, void *buf, unsigned int len, int flags)
{
    if (!buf) {
        last_socket_error = NOX_WSAEINVAL;
        errno = EINVAL;
        return -1;
    }
    if (g_soc_suspended) {
        last_socket_error = NOX_WSAEWOULDBLOCK;
        errno = EWOULDBLOCK;
        return -1;
    }

    soc_lock();
    soc_slot_t *s = lookup_slot(vfd);
    if (!s) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        soc_unlock();
        return -1;
    }
    int rfd = s->real_fd;
    soc_unlock();

    ssize_t r = recv(rfd, buf, (size_t)len, SOC_SAFE_MSG_FLAGS(flags));
    if (r < 0) {
        last_socket_error = wsa_from_errno(errno);
        return -1;
    }
    last_socket_error = 0;
    return (int)r;
}

static int sockets_gethostname(char *name, size_t len)
{
    if (name && len > 0) {
        snprintf(name, len, "n3ds-wifi");
    }
    return 0;
}

static struct hostent *sockets_gethostbyname(const char *name)
{
    static struct hostent he;
    static uint32_t addr = 0;
    static char *addr_list[2] = { NULL, NULL };
    static char hname[64];

    /* Fast-path IPv4 literals without blocking DNS. */
    if (name && *name) {
        in_addr_t parsed = inet_addr(name);
        if (parsed != INADDR_NONE || strcmp(name, "255.255.255.255") == 0) {
            addr = (uint32_t)parsed;
            addr_list[0] = (char *)&addr;
            addr_list[1] = NULL;
            snprintf(hname, sizeof(hname), "%s", name);
            he.h_name = hname;
            he.h_aliases = NULL;
            he.h_addrtype = AF_INET;
            he.h_length = 4;
            he.h_addr_list = addr_list;
            return &he;
        }
    }

    soc_lock();
    bool ok = soc_ensure_init_locked();
    soc_unlock();
    if (!ok)
        return NULL;

    /* Fast-path local hostname queries via 3DS LAN IP (gethostid()). */
    if (!name || !*name ||
        strcmp(name, "n3ds-wifi") == 0 ||
        strcmp(name, "n3ds-local") == 0 ||
        strcmp(name, "localhost") == 0) {
        uint32_t local_ip = (uint32_t)gethostid();
        if (local_ip == 0u)
            local_ip = htonl(0x7F000001u);
        addr = local_ip;
        addr_list[0] = (char *)&addr;
        addr_list[1] = NULL;
        snprintf(hname, sizeof(hname), "n3ds-wifi");
        he.h_name = hname;
        he.h_aliases = NULL;
        he.h_addrtype = AF_INET;
        he.h_length = 4;
        he.h_addr_list = addr_list;
        return &he;
    }

    RecursiveLock_Lock(&g_dns_lock);
    struct hostent *res = gethostbyname(name);
    bool found = false;
    if (res && res->h_addr_list && res->h_addr_list[0] && res->h_length == 4) {
        memcpy(&addr, res->h_addr_list[0], 4);
        addr_list[0] = (char *)&addr;
        addr_list[1] = NULL;
        snprintf(hname, sizeof(hname), "%s", res->h_name ? res->h_name : name);
        he.h_name = hname;
        he.h_aliases = NULL;
        he.h_addrtype = AF_INET;
        he.h_length = 4;
        he.h_addr_list = addr_list;
        found = true;
    }
    RecursiveLock_Unlock(&g_dns_lock);
    return found ? &he : NULL;
}

const nox_3ds_net_ops_t g_nox_3ds_sockets_ops = {
    .init          = sockets_init,
    .shutdown      = sockets_shutdown,
    .socket        = sockets_socket,
    .closesocket   = sockets_closesocket,
    .bind          = sockets_bind,
    .setsockopt    = sockets_setsockopt,
    .ioctlsocket   = sockets_ioctlsocket,
    .sendto        = sockets_sendto,
    .recvfrom      = sockets_recvfrom,
    .listen        = sockets_listen,
    .accept        = sockets_accept,
    .shutdown_sock = sockets_shutdown_sock,
    .send          = sockets_send,
    .recv          = sockets_recv,
    .gethostname   = sockets_gethostname,
    .gethostbyname = sockets_gethostbyname,
};
