/*
 * In-memory UDP loopback transport for New 3DS (Milestone 3).
 *
 * Does NOT call socInit() or touch Wi-Fi/SOC hardware, so Solo Campaign,
 * Solo Quest, and local client/server communication work cleanly even in
 * airplane mode with zero 1 MB SOC shared-buffer overhead.
 */
#include "platform_3ds.h"
#include "../../src/windows.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define MAX_LOOPBACK_SOCKS  16
#define LOOPBACK_FD_BASE    1024
#define LOOPBACK_RING_CAP   32
#define LOOPBACK_MAX_DGRAM  2048

#define NOX_FIONREAD        0x4004667Fu
#define NOX_FIONBIO         0x8004667Eu

#define NOX_WSAEBADF        10009u
#define NOX_WSAEINVAL       10022u
#define NOX_WSAEMFILE       10024u
#define NOX_WSAEWOULDBLOCK  10035u
#define NOX_WSAENOTSOCK     10038u
#define NOX_WSAEADDRINUSE   10048u
#define NOX_WSAEOPNOTSUPP   10045u

typedef struct {
    uint32_t src_ip;   /* network byte order */
    uint16_t src_port; /* network byte order */
    uint16_t len;
    uint8_t  data[LOOPBACK_MAX_DGRAM];
} loopback_pkt_t;

typedef struct {
    bool            in_use;
    int             domain;
    int             type;
    int             protocol;
    bool            bound;
    bool            nonblocking;
    bool            broadcast;
    uint32_t        bound_ip;   /* network byte order (0 = INADDR_ANY) */
    uint16_t        bound_port; /* host byte order */
    uint32_t        head;
    uint32_t        tail;
    uint32_t        count;
    loopback_pkt_t *rx_ring;    /* allocated on socket(), freed on closesocket() */
} loopback_sock_t;

extern DWORD last_socket_error;

static loopback_sock_t  g_socks[MAX_LOOPBACK_SOCKS];
static nox_3ds_mutex_t *g_net_mutex = NULL;
static bool             g_net_inited = false;
static uint16_t         g_next_ephemeral = 49152u;

static inline uint32_t loopback_ip_nbo(void)
{
    return htonl(0x7F000001u); /* 127.0.0.1 in network byte order */
}

static bool is_loopback_or_local_ip(uint32_t ip_nbo)
{
    if (ip_nbo == 0u || ip_nbo == 0xFFFFFFFFu || ip_nbo == loopback_ip_nbo())
        return true;
    /* Any 127.x.x.x address (host byte order high byte == 127) */
    if ((ntohl(ip_nbo) >> 24) == 127u)
        return true;
    return false;
}

static void net_lock(void)
{
    if (!g_net_inited)
        nox_3ds_net_init();
    if (g_net_mutex)
        nox_3ds_mutex_lock(g_net_mutex);
}

static void net_unlock(void)
{
    if (g_net_mutex)
        nox_3ds_mutex_unlock(g_net_mutex);
}

static loopback_sock_t *lookup_sock(int fd)
{
    int idx = fd - LOOPBACK_FD_BASE;
    if (idx < 0 || idx >= MAX_LOOPBACK_SOCKS)
        return NULL;
    if (!g_socks[idx].in_use)
        return NULL;
    return &g_socks[idx];
}

static bool port_in_use(int sock_type, uint32_t ip_nbo, uint16_t port_hbo, const loopback_sock_t *skip)
{
    for (int i = 0; i < MAX_LOOPBACK_SOCKS; ++i) {
        const loopback_sock_t *s = &g_socks[i];
        if (!s->in_use || !s->bound || s == skip)
            continue;
        if (s->type != sock_type)
            continue;
        if (s->bound_port != port_hbo)
            continue;
        if (s->bound_ip == 0u || ip_nbo == 0u || s->bound_ip == ip_nbo)
            return true;
    }
    return false;
}

static uint16_t alloc_ephemeral_port(int sock_type)
{
    for (unsigned tries = 0; tries < 16384u; ++tries) {
        uint16_t p = g_next_ephemeral++;
        if (g_next_ephemeral < 49152u)
            g_next_ephemeral = 49152u;
        if (!port_in_use(sock_type, 0u, p, NULL))
            return p;
    }
    return 49152u;
}

void nox_3ds_net_init(void)
{
    if (g_net_inited)
        return;
    memset(g_socks, 0, sizeof(g_socks));
    g_net_mutex = nox_3ds_mutex_create();
    g_next_ephemeral = 49152u;
    last_socket_error = 0;
    g_net_inited = true;
    printf("[net3ds] In-memory UDP loopback initialized (no socInit, %d slots x %u KB)\n",
           MAX_LOOPBACK_SOCKS,
           (unsigned)((LOOPBACK_RING_CAP * sizeof(loopback_pkt_t)) / 1024u));
}

void nox_3ds_net_shutdown(void)
{
    if (!g_net_inited)
        return;
    if (g_net_mutex)
        nox_3ds_mutex_lock(g_net_mutex);
    for (int i = 0; i < MAX_LOOPBACK_SOCKS; ++i) {
        if (g_socks[i].rx_ring) {
            free(g_socks[i].rx_ring);
            g_socks[i].rx_ring = NULL;
        }
        g_socks[i].in_use = false;
    }
    if (g_net_mutex) {
        nox_3ds_mutex_unlock(g_net_mutex);
        nox_3ds_mutex_destroy(g_net_mutex);
        g_net_mutex = NULL;
    }
    g_net_inited = false;
}

static bool ensure_rx_ring(loopback_sock_t *s)
{
    if (!s)
        return false;
    if (s->rx_ring)
        return true;
    s->rx_ring = (loopback_pkt_t *)calloc(LOOPBACK_RING_CAP, sizeof(loopback_pkt_t));
    return s->rx_ring != NULL;
}

int nox_3ds_net_socket(int domain, int type, int protocol)
{
    net_lock();
    int slot = -1;
    for (int i = 0; i < MAX_LOOPBACK_SOCKS; ++i) {
        if (!g_socks[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        last_socket_error = NOX_WSAEMFILE;
        errno = EMFILE;
        net_unlock();
        return -1;
    }

    loopback_sock_t *s = &g_socks[slot];
    memset(s, 0, sizeof(*s));
    s->in_use = true;
    s->domain = domain;
    s->type = type;
    s->protocol = protocol;
    s->rx_ring = NULL;

    last_socket_error = 0;
    net_unlock();
    printf("[net3ds] socket(dom=%d, type=%d, proto=%d) -> fd=%d\n",
           domain, type, protocol, LOOPBACK_FD_BASE + slot);
    return LOOPBACK_FD_BASE + slot;
}

int nox_3ds_net_closesocket(int fd)
{
    net_lock();
    loopback_sock_t *s = lookup_sock(fd);
    if (!s) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        net_unlock();
        return -1;
    }

    if (s->rx_ring) {
        free(s->rx_ring);
        s->rx_ring = NULL;
    }
    memset(s, 0, sizeof(*s));
    last_socket_error = 0;
    net_unlock();
    return 0;
}

int nox_3ds_net_bind(int fd, const void *addr, unsigned int addrlen)
{
    if (!addr || addrlen < sizeof(struct sockaddr_in)) {
        last_socket_error = NOX_WSAEINVAL;
        errno = EINVAL;
        return -1;
    }

    struct sockaddr_in sin;
    memcpy(&sin, addr, sizeof(sin));
    uint32_t ip_nbo = sin.sin_addr.s_addr;
    uint16_t port_hbo = ntohs(sin.sin_port);

    net_lock();
    loopback_sock_t *s = lookup_sock(fd);
    if (!s) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        net_unlock();
        return -1;
    }

    if (port_hbo == 0u) {
        port_hbo = alloc_ephemeral_port(s->type);
    } else if (port_in_use(s->type, ip_nbo, port_hbo, s)) {
        last_socket_error = NOX_WSAEADDRINUSE;
        errno = EADDRINUSE;
        net_unlock();
        return -1;
    }

    if (!ensure_rx_ring(s)) {
        last_socket_error = NOX_WSAEMFILE;
        errno = ENOMEM;
        net_unlock();
        return -1;
    }

    s->bound = true;
    s->bound_ip = ip_nbo;
    s->bound_port = port_hbo;
    last_socket_error = 0;
    net_unlock();
    printf("[net3ds] bind(fd=%d, ip=0x%08x, port=%u) -> ok\n",
           fd, (unsigned)ntohl(ip_nbo), (unsigned)port_hbo);
    return 0;
}

int nox_3ds_net_setsockopt(int fd, int level, int opt, const void *val, unsigned int len)
{
    net_lock();
    loopback_sock_t *s = lookup_sock(fd);
    if (!s) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        net_unlock();
        return -1;
    }

    if (((level == 0xFFFF && opt == 0x20) || (level == SOL_SOCKET && opt == SO_BROADCAST)) &&
        val && len >= sizeof(int)) {
        int on = 0;
        memcpy(&on, val, sizeof(int));
        s->broadcast = (on != 0);
    }

    last_socket_error = 0;
    net_unlock();
    return 0;
}

int nox_3ds_net_ioctlsocket(int fd, long cmd, unsigned long *argp)
{
    uint32_t ucmd = (uint32_t)cmd;

    net_lock();
    loopback_sock_t *s = lookup_sock(fd);
    if (!s) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        net_unlock();
        return -1;
    }

    if (!argp) {
        last_socket_error = NOX_WSAEINVAL;
        errno = EINVAL;
        net_unlock();
        return -1;
    }

    if (ucmd == NOX_FIONREAD) {
        if (s->count > 0u && s->rx_ring) {
            *argp = (unsigned long)s->rx_ring[s->head].len;
        } else {
            *argp = 0ul;
        }
        last_socket_error = 0;
        net_unlock();
        return 0;
    }

    if (ucmd == NOX_FIONBIO) {
        s->nonblocking = (*argp != 0ul);
        last_socket_error = 0;
        net_unlock();
        return 0;
    }

    last_socket_error = NOX_WSAEINVAL;
    errno = EINVAL;
    net_unlock();
    return -1;
}

static void enqueue_datagram(loopback_sock_t *dst,
                             uint32_t src_ip_nbo, uint16_t src_port_nbo,
                             const void *buf, uint16_t copy_len)
{
    if (!ensure_rx_ring(dst))
        return;

    /* Drop oldest datagram if ring is saturated so latest game state always delivers. */
    if (dst->count >= LOOPBACK_RING_CAP) {
        dst->head = (dst->head + 1u) % LOOPBACK_RING_CAP;
        dst->count--;
    }

    loopback_pkt_t *slot = &dst->rx_ring[dst->tail];
    slot->src_ip = src_ip_nbo;
    slot->src_port = src_port_nbo;
    slot->len = copy_len;
    if (copy_len > 0u) {
        memcpy(slot->data, buf, copy_len);
    }
    dst->tail = (dst->tail + 1u) % LOOPBACK_RING_CAP;
    dst->count++;
}

int nox_3ds_net_sendto(int fd, const void *buf, unsigned int len, int flags,
                       const void *to, unsigned int tolen)
{
    (void)flags;
    if (!buf || !to || tolen < sizeof(struct sockaddr_in)) {
        last_socket_error = NOX_WSAEINVAL;
        errno = EINVAL;
        return -1;
    }

    struct sockaddr_in dst_sin;
    memcpy(&dst_sin, to, sizeof(dst_sin));
    uint32_t dest_ip = dst_sin.sin_addr.s_addr;
    uint16_t dest_port = ntohs(dst_sin.sin_port);

    net_lock();
    loopback_sock_t *sender = lookup_sock(fd);
    if (!sender) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        net_unlock();
        return -1;
    }

    /* Auto-bind sender to an ephemeral port on first sendto if unbound. */
    if (!sender->bound) {
        sender->bound_ip = 0u;
        sender->bound_port = alloc_ephemeral_port(sender->type);
        sender->bound = true;
    }

    /* Silently absorb external non-loopback datagrams with no error or stall. */
    if (!is_loopback_or_local_ip(dest_ip)) {
        last_socket_error = 0;
        net_unlock();
        return (int)len;
    }

    uint32_t src_ip = (sender->bound_ip != 0u) ? sender->bound_ip : loopback_ip_nbo();
    uint16_t src_port = htons(sender->bound_port);
    uint16_t copy_len = (len > LOOPBACK_MAX_DGRAM) ? (uint16_t)LOOPBACK_MAX_DGRAM : (uint16_t)len;
    bool is_bcast = (dest_ip == 0xFFFFFFFFu);

    for (int i = 0; i < MAX_LOOPBACK_SOCKS; ++i) {
        loopback_sock_t *dst = &g_socks[i];
        if (!dst->in_use || !dst->bound || dst->type != SOCK_DGRAM)
            continue;
        if (dst->bound_port != dest_port)
            continue;
        if (is_bcast && dst == sender)
            continue;

        bool ip_match = is_bcast ||
                        dst->bound_ip == 0u ||
                        dst->bound_ip == dest_ip ||
                        dest_ip == 0u ||
                        dest_ip == loopback_ip_nbo();
        if (!ip_match)
            continue;

        enqueue_datagram(dst, src_ip, src_port, buf, copy_len);
    }

    last_socket_error = 0;
    net_unlock();
    return (int)len;
}

int nox_3ds_net_recvfrom(int fd, void *buf, unsigned int len, int flags,
                         void *from, unsigned int *fromlen)
{
    if (!buf) {
        last_socket_error = NOX_WSAEINVAL;
        errno = EINVAL;
        return -1;
    }

    net_lock();
    loopback_sock_t *s = lookup_sock(fd);
    if (!s) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        net_unlock();
        return -1;
    }

    if (s->count == 0u || !s->rx_ring) {
        last_socket_error = NOX_WSAEWOULDBLOCK;
        errno = EWOULDBLOCK;
        net_unlock();
        return -1;
    }

    const loopback_pkt_t *pkt = &s->rx_ring[s->head];
    unsigned int copy_len = (pkt->len < len) ? (unsigned int)pkt->len : len;
    if (copy_len > 0u) {
        memcpy(buf, pkt->data, copy_len);
    }

    if (from && fromlen && *fromlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = pkt->src_port;
        sin.sin_addr.s_addr = pkt->src_ip;
        memcpy(from, &sin, sizeof(sin));
        *fromlen = (unsigned int)sizeof(struct sockaddr_in);
    }

#ifdef MSG_PEEK
    if (!(flags & MSG_PEEK))
#endif
    {
        s->head = (s->head + 1u) % LOOPBACK_RING_CAP;
        s->count--;
    }

    last_socket_error = 0;
    net_unlock();
    return (int)copy_len;
}

int nox_3ds_net_last_error(void)
{
    return (int)last_socket_error;
}

/* =========================================================================
 * Winsock / Compat Entry Points (mapped from src/windows.h macros)
 * ========================================================================= */
int WINAPI WSAStartup(WORD wVersionRequested, struct WSAData *lpWSAData)
{
    if (!g_net_inited)
        nox_3ds_net_init();
    if (lpWSAData) {
        memset(lpWSAData, 0, sizeof(*lpWSAData));
        lpWSAData->wVersion = wVersionRequested;
        lpWSAData->wHighVersion = 0x0202u;
        snprintf(lpWSAData->szDescription, sizeof(lpWSAData->szDescription), "Nox 3DS Loopback");
        snprintf(lpWSAData->szSystemStatus, sizeof(lpWSAData->szSystemStatus), "Running");
        lpWSAData->iMaxSockets = MAX_LOOPBACK_SOCKS;
        lpWSAData->iMaxUdpDg = LOOPBACK_MAX_DGRAM;
    }
    last_socket_error = 0;
    return 0;
}

int WINAPI WSACleanup(void)
{
    last_socket_error = 0;
    return 0;
}

SOCKET WINAPI socket(int domain, int type, int protocol)
#undef socket
{
    return (SOCKET)nox_3ds_net_socket(domain, type, protocol);
}

int WINAPI closesocket(SOCKET s)
{
    return nox_3ds_net_closesocket((int)s);
}

int WINAPI bind(int sockfd, const struct sockaddr *addr, unsigned int addrlen)
#undef bind
{
    return nox_3ds_net_bind(sockfd, addr, addrlen);
}

int WINAPI setsockopt(SOCKET s, int level, int opt, const void *value, unsigned int len)
#undef setsockopt
{
    return nox_3ds_net_setsockopt((int)s, level, opt, value, len);
}

int WINAPI ioctlsocket(SOCKET s, long cmd, unsigned long *argp)
{
    return nox_3ds_net_ioctlsocket((int)s, cmd, argp);
}

int WINAPI sendto(int sockfd, void *buffer, unsigned int length, int flags,
                  const struct sockaddr *addr, unsigned int addrlen)
#undef sendto
{
    return nox_3ds_net_sendto(sockfd, buffer, length, flags, addr, addrlen);
}

int WINAPI recvfrom(int sockfd, void *buffer, unsigned int length, int flags,
                    struct sockaddr *addr, unsigned int *addrlen)
#undef recvfrom
{
    return nox_3ds_net_recvfrom(sockfd, buffer, length, flags, addr, addrlen);
}

int WINAPI WSAGetLastError(void)
{
    return nox_3ds_net_last_error();
}

char *WINAPI inet_ntoa(struct in_addr in)
#undef in_addr
#undef inet_ntoa
{
    static char buf[16];
    uint32_t ip = in.S_un.S_addr;
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             (unsigned)(ip & 0xFFu),
             (unsigned)((ip >> 8) & 0xFFu),
             (unsigned)((ip >> 16) & 0xFFu),
             (unsigned)((ip >> 24) & 0xFFu));
    return buf;
}

int gethostname(char *name, size_t len)
{
    if (name && len > 0) {
        snprintf(name, len, "n3ds-local");
    }
    return 0;
}

struct hostent *gethostbyname(const char *name)
{
    static struct hostent he;
    static uint32_t addr = 0;
    static char *addr_list[2] = { NULL, NULL };
    static char hname[] = "n3ds-local";
    (void)name;

    addr = loopback_ip_nbo();
    addr_list[0] = (char *)&addr;
    addr_list[1] = NULL;

    he.h_name = hname;
    he.h_aliases = NULL;
    he.h_addrtype = AF_INET;
    he.h_length = 4;
    he.h_addr_list = addr_list;
    return &he;
}

/*
 * Override POSIX TCP helpers referenced by GAME5.c's optional rcon/telnet
 * console (sub_578E00..sub_579600) so libctru's soc service is never linked
 * or invoked on 3DS.
 */
int listen(int sockfd, int backlog)
{
    (void)backlog;
    net_lock();
    loopback_sock_t *s = lookup_sock(sockfd);
    net_unlock();
    if (!s) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        return -1;
    }
    last_socket_error = 0;
    return 0;
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    (void)sockfd; (void)addr; (void)addrlen;
    last_socket_error = NOX_WSAEWOULDBLOCK;
    errno = EWOULDBLOCK;
    return -1;
}

int shutdown(int sockfd, int how)
{
    (void)sockfd; (void)how;
    last_socket_error = 0;
    return 0;
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
    (void)sockfd; (void)buf; (void)flags;
    last_socket_error = 0;
    return (ssize_t)len;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    (void)sockfd; (void)buf; (void)len; (void)flags;
    last_socket_error = NOX_WSAEWOULDBLOCK;
    errno = EWOULDBLOCK;
    return -1;
}
