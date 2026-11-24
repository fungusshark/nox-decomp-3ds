/*
 * Dual-transport network dispatcher for New 3DS.
 *
 * Owns all Winsock / POSIX compat* entry points remapped from src/windows.h
 * and routes cleanly between:
 *   - net_loopback_3ds.c (vFD 1024..1039): Solo Campaign & Solo Quest (zero socInit)
 *   - net_sockets_3ds.c  (vFD 2048..2079): Multiplayer menus / Direct IPv4 via soc:u
 */
#include "platform_3ds.h"
#include "../../src/windows.h"

/* Undefine src/windows.h socket-remapping macros so struct member names on
 * nox_3ds_net_ops_t (e.g. ops->socket, ops->shutdown) are not macro-expanded.
 * We define the compat* symbols explicitly below. */
#undef in_addr
#undef inet_ntoa
#undef WSAStartup
#undef WSACleanup
#undef socket
#undef setsockopt
#undef bind
#undef closesocket
#undef ioctlsocket
#undef WSAGetLastError
#undef recvfrom
#undef sendto
#undef gethostname
#undef gethostbyname
#undef listen
#undef accept
#undef shutdown
#undef send
#undef recv

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define NOX_WSAENOTSOCK 10038u

extern DWORD last_socket_error;

static nox_3ds_net_mode_t g_net_mode = NOX_3DS_NET_MODE_LOOPBACK;

static const nox_3ds_net_ops_t *backend_for_mode(void)
{
    if (g_net_mode == NOX_3DS_NET_MODE_WIFI)
        return &g_nox_3ds_sockets_ops;
    return &g_nox_3ds_loopback_ops;
}

static const nox_3ds_net_ops_t *backend_for_fd(int fd)
{
    if (fd >= NOX_3DS_LOOPBACK_FD_BASE &&
        fd <  NOX_3DS_LOOPBACK_FD_BASE + NOX_3DS_LOOPBACK_FD_MAX) {
        return &g_nox_3ds_loopback_ops;
    }
    if (fd >= NOX_3DS_SOCKETS_FD_BASE &&
        fd <  NOX_3DS_SOCKETS_FD_BASE + NOX_3DS_SOCKETS_FD_MAX) {
        return &g_nox_3ds_sockets_ops;
    }
    return NULL;
}

void nox_3ds_net_init(void)
{
    g_net_mode = NOX_3DS_NET_MODE_LOOPBACK;
    g_nox_3ds_loopback_ops.init();
    /* NOTE: g_nox_3ds_sockets_ops.init() is intentionally NOT called here.
     * socInit() + 1 MB memalign buffer are acquired lazily only when the
     * player actively enters Multiplayer menus. */
}

void nox_3ds_net_shutdown(void)
{
    g_nox_3ds_sockets_ops.shutdown();
    g_nox_3ds_loopback_ops.shutdown();
    g_net_mode = NOX_3DS_NET_MODE_LOOPBACK;
}

void nox_3ds_net_set_mode(nox_3ds_net_mode_t mode)
{
    if (g_net_mode == mode)
        return;
    g_net_mode = mode;
    printf("[net3ds] Transport mode -> %s\n",
           (mode == NOX_3DS_NET_MODE_WIFI) ? "WIFI (soc:u)" : "LOOPBACK (offline)");
}

nox_3ds_net_mode_t nox_3ds_net_get_mode(void)
{
    return g_net_mode;
}

int nox_3ds_net_socket(int domain, int type, int protocol)
{
    return backend_for_mode()->socket(domain, type, protocol);
}

int nox_3ds_net_closesocket(int fd)
{
    const nox_3ds_net_ops_t *ops = backend_for_fd(fd);
    if (!ops) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        return -1;
    }
    return ops->closesocket(fd);
}

int nox_3ds_net_bind(int fd, const void *addr, unsigned int addrlen)
{
    const nox_3ds_net_ops_t *ops = backend_for_fd(fd);
    if (!ops) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        return -1;
    }
    return ops->bind(fd, addr, addrlen);
}

int nox_3ds_net_setsockopt(int fd, int level, int opt, const void *val, unsigned int len)
{
    const nox_3ds_net_ops_t *ops = backend_for_fd(fd);
    if (!ops) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        return -1;
    }
    return ops->setsockopt(fd, level, opt, val, len);
}

int nox_3ds_net_ioctlsocket(int fd, long cmd, unsigned long *argp)
{
    const nox_3ds_net_ops_t *ops = backend_for_fd(fd);
    if (!ops) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        return -1;
    }
    return ops->ioctlsocket(fd, cmd, argp);
}

int nox_3ds_net_sendto(int fd, const void *buf, unsigned int len, int flags,
                       const void *to, unsigned int tolen)
{
    const nox_3ds_net_ops_t *ops = backend_for_fd(fd);
    if (!ops) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        return -1;
    }
    return ops->sendto(fd, buf, len, flags, to, tolen);
}

int nox_3ds_net_recvfrom(int fd, void *buf, unsigned int len, int flags,
                         void *from, unsigned int *fromlen)
{
    const nox_3ds_net_ops_t *ops = backend_for_fd(fd);
    if (!ops) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        return -1;
    }
    return ops->recvfrom(fd, buf, len, flags, from, fromlen);
}

int nox_3ds_net_last_error(void)
{
    return (int)last_socket_error;
}

/* =========================================================================
 * Winsock / POSIX Compat Entry Points (mapped from src/windows.h macros)
 * ========================================================================= */
int WINAPI compatWSAStartup(WORD wVersionRequested, struct WSAData *lpWSAData)
{
    if (lpWSAData) {
        memset(lpWSAData, 0, sizeof(*lpWSAData));
        lpWSAData->wVersion = wVersionRequested;
        lpWSAData->wHighVersion = 0x0202u;
        snprintf(lpWSAData->szDescription, sizeof(lpWSAData->szDescription),
                 (g_net_mode == NOX_3DS_NET_MODE_WIFI) ? "Nox 3DS soc:u Wi-Fi" : "Nox 3DS Loopback");
        snprintf(lpWSAData->szSystemStatus, sizeof(lpWSAData->szSystemStatus), "Running");
        lpWSAData->iMaxSockets = (g_net_mode == NOX_3DS_NET_MODE_WIFI)
                                 ? NOX_3DS_SOCKETS_FD_MAX : NOX_3DS_LOOPBACK_FD_MAX;
        lpWSAData->iMaxUdpDg = 2048u;
    }
    last_socket_error = 0;
    return 0;
}

int WINAPI compatWSACleanup(void)
{
    last_socket_error = 0;
    return 0;
}

SOCKET WINAPI compatsocket(int domain, int type, int protocol)
{
    return (SOCKET)nox_3ds_net_socket(domain, type, protocol);
}

int WINAPI compatclosesocket(SOCKET s)
{
    return nox_3ds_net_closesocket((int)s);
}

int WINAPI compatbind(int sockfd, const struct sockaddr *addr, unsigned int addrlen)
{
    return nox_3ds_net_bind(sockfd, addr, addrlen);
}

int WINAPI compatsetsockopt(SOCKET s, int level, int opt, const void *value, unsigned int len)
{
    return nox_3ds_net_setsockopt((int)s, level, opt, value, len);
}

int WINAPI compatioctlsocket(SOCKET s, long cmd, unsigned long *argp)
{
    return nox_3ds_net_ioctlsocket((int)s, cmd, argp);
}

int WINAPI compatsendto(int sockfd, void *buffer, unsigned int length, int flags,
                        const struct sockaddr *addr, unsigned int addrlen)
{
    return nox_3ds_net_sendto(sockfd, buffer, length, flags, addr, addrlen);
}

int WINAPI compatrecvfrom(int sockfd, void *buffer, unsigned int length, int flags,
                          struct sockaddr *addr, unsigned int *addrlen)
{
    return nox_3ds_net_recvfrom(sockfd, buffer, length, flags, addr, addrlen);
}

int WINAPI compatWSAGetLastError(void)
{
    return nox_3ds_net_last_error();
}

char *WINAPI compatinet_ntoa(struct compatin_addr in)
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

int compatgethostname(char *name, size_t len)
{
    return backend_for_mode()->gethostname(name, len);
}

struct hostent *compatgethostbyname(const char *name)
{
    return backend_for_mode()->gethostbyname(name);
}

int compatlisten(int sockfd, int backlog)
{
    const nox_3ds_net_ops_t *ops = backend_for_fd(sockfd);
    if (!ops) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        return -1;
    }
    return ops->listen(sockfd, backlog);
}

int compataccept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    const nox_3ds_net_ops_t *ops = backend_for_fd(sockfd);
    if (!ops) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        return -1;
    }
    unsigned int alen = addrlen ? (unsigned int)*addrlen : 0u;
    int r = ops->accept(sockfd, addr, addrlen ? &alen : NULL);
    if (addrlen)
        *addrlen = (socklen_t)alen;
    return r;
}

int compatshutdown(int sockfd, int how)
{
    const nox_3ds_net_ops_t *ops = backend_for_fd(sockfd);
    if (!ops) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        return -1;
    }
    return ops->shutdown_sock(sockfd, how);
}

ssize_t compatsend(int sockfd, const void *buf, size_t len, int flags)
{
    const nox_3ds_net_ops_t *ops = backend_for_fd(sockfd);
    if (!ops) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        return -1;
    }
    return (ssize_t)ops->send(sockfd, buf, (unsigned int)len, flags);
}

ssize_t compatrecv(int sockfd, void *buf, size_t len, int flags)
{
    const nox_3ds_net_ops_t *ops = backend_for_fd(sockfd);
    if (!ops) {
        last_socket_error = NOX_WSAENOTSOCK;
        errno = EBADF;
        return -1;
    }
    return (ssize_t)ops->recv(sockfd, buf, (unsigned int)len, flags);
}
