/*
 * Real Wi-Fi socket transport for Nintendo 3DS via libctru soc:u service.
 *
 * Design guarantees:
 *   - Lazy socInit(): never called at boot or in Solo Campaign / Solo Quest.
 *     Initialized strictly on first Multiplayer socket/DNS call after verifying
 *     Wi-Fi availability via ACU_GetWifiStatus().
 *   - 1 MB (0x100000) context buffer allocated via memalign(0x1000, 0x100000)
 *     on standard FCRAM heap (preserving scarce linear heap for PICA200/ndsp).
 *   - New 3DS (4 cores @ 804 MHz): Single-owner Core 2 UDP SPSC packet pump.
 *     Once a SOCK_DGRAM socket binds (or first sends), exclusive ownership of
 *     the real soc:u FD transfers via __ATOMIC_RELEASE to the Core 2 worker
 *     thread behind lock-free cache-line-aligned RX/TX SPSC rings:
 *       * Core 0 FIONREAD / recvfrom / closesocket execute in ~5-50 cycles
 *         with ZERO svcSendSyncRequest IPC stalls on the 30 Hz main loop.
 *       * Core 0 sendto enqueues into the TX SPSC ring + signals a LightEvent
 *         in ~1 us instead of blocking 200-5,000 us in soc:u IPC.
 *       * Core 2 is the SOLE thread calling sendto/recvfrom/close on pumped
 *         UDP FDs, eliminating cross-core soc:u / closesocket() races by
 *         construction. Ring buffers are heap-allocated per active UDP socket
 *         and freed on close/teardown (0 B overhead in Solo mode).
 *   - Old 3DS (2 cores @ 268 MHz): Direct synchronous Core 0 non-blocking path
 *     (64 KB SO_RCVBUF + single-copy FIONREAD prefetch) with zero extra worker
 *     threads so Core 0 never suffers single-core context-switch thrashing.
 *   - Safe teardown in shutdown(): stop & join Core 2 pump outside g_soc_mutex;
 *     never execute socExit() or free(g_soc_buffer) if any worker failed to join.
 *
 * Exports only g_nox_3ds_sockets_ops + nox_3ds_soc_*() +
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

#if defined(__GNUC__) || defined(__clang__)
#define NOX_ALIGN32 __attribute__((aligned(32)))
#else
#define NOX_ALIGN32
#endif

#define SOC_ALIGN               0x1000u
#define SOC_BUFFERSIZE          0x100000u /* 1 MB page-aligned FCRAM context buffer */
#define MAX_SOC_SOCKS           NOX_3DS_SOCKETS_FD_MAX
#define SOCKETS_FD_BASE         NOX_3DS_SOCKETS_FD_BASE
#define SOCKETS_SLOT_MASK       (MAX_SOC_SOCKS - 1)
#define SOCKETS_GEN_SHIFT       5
#define SOCKETS_GEN_MAX         NOX_3DS_SOCKETS_GEN_MAX
#define SOC_MAX_DRAIN_PER_TICK  256u
#define SOC_DRAIN_WINDOW_MS     16u       /* Half of a 30 Hz Nox tick (~33 ms) */

#define UDP_MAX_DGRAM           2048u
#define UDP_RX_RING_CAP         128u      /* Power of 2: Core 2 -> Core 0 */
#define UDP_RX_RING_MASK        (UDP_RX_RING_CAP - 1u)
#define UDP_TX_RING_CAP         64u       /* Power of 2: Core 0 -> Core 2 */
#define UDP_TX_RING_MASK        (UDP_TX_RING_CAP - 1u)
#define UDP_PUMP_STACK_SIZE     (32u * 1024u)
#define UDP_PUMP_POLL_NS        1500000LL /* 1.5 ms (~22x per 30 Hz tick) */
#define UDP_PUMP_SUSPEND_NS     20000000LL
#define SOC_IP_CACHE_TTL_MS     2000u

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert((MAX_SOC_SOCKS & (MAX_SOC_SOCKS - 1)) == 0, "MAX_SOC_SOCKS must be power of 2");
_Static_assert((1 << SOCKETS_GEN_SHIFT) == MAX_SOC_SOCKS, "SOCKETS_GEN_SHIFT must match MAX_SOC_SOCKS");
_Static_assert((UDP_RX_RING_CAP & UDP_RX_RING_MASK) == 0u, "UDP_RX_RING_CAP must be power of 2");
_Static_assert((UDP_TX_RING_CAP & UDP_TX_RING_MASK) == 0u, "UDP_TX_RING_CAP must be power of 2");
#endif

/* Sockets are already configured O_NONBLOCK via fcntl() at creation time.
 * Never force MSG_DONTWAIT onto sendto()/send() (real soc:u rejects unsupported
 * per-call flags on SendTo); only fall back to MSG_DONTWAIT on recv if fcntl failed. */
#define SOC_SAFE_SEND_FLAGS(f)      ((f) & MSG_OOB)
#define SOC_SAFE_RECV_FLAGS(f, nb)  (((f) & (MSG_OOB | MSG_PEEK)) | ((nb) ? 0 : MSG_DONTWAIT))

/* Real 3DS soc:u CTRPollFD bitmasks (libctru <poll.h> misdefines POLLERR=0x08 / POLLNVAL=0x20). */
#define CTR_SOC_POLLRDNORM      0x01
#define CTR_SOC_POLLRDBAND      0x02
#define CTR_SOC_POLLPRI         0x04
#define CTR_SOC_POLLWRNORM      0x08
#define CTR_SOC_POLLWRBAND      0x10
#define CTR_SOC_POLLERR         0x20
#define CTR_SOC_POLLHUP         0x40
#define CTR_SOC_POLLNVAL        0x80

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
#define NOX_WSAEMSGSIZE         10040u
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

/* Slot ownership states (published/observed via __ATOMIC_RELEASE/__ATOMIC_ACQUIRE):
 *   SOC_SLOT_FREE    (0): Unused slot.
 *   SOC_SLOT_SETUP   (1): N3DS UDP socket pre-bind; owned exclusively by Core 0.
 *   SOC_SLOT_PUMPED  (2): N3DS UDP socket handed off; real_fd owned exclusively by Core 2!
 *   SOC_SLOT_DIRECT  (3): O3DS UDP or TCP socket; owned exclusively by Core 0.
 *   SOC_SLOT_CLOSING (4): Core 0 invalidated vfd; Core 2 drains final TX, closes real_fd, frees rings. */
enum {
    SOC_SLOT_FREE    = 0u,
    SOC_SLOT_SETUP   = 1u,
    SOC_SLOT_PUMPED  = 2u,
    SOC_SLOT_DIRECT  = 3u,
    SOC_SLOT_CLOSING = 4u,
};

typedef struct {
    uint32_t src_ip;   /* network byte order */
    uint16_t src_port; /* network byte order */
    uint16_t len;
    uint8_t  payload[UDP_MAX_DGRAM];
} udp_rx_pkt_t;

typedef struct {
    uint32_t dst_ip;   /* network byte order */
    uint16_t dst_port; /* network byte order */
    uint16_t len;
    int32_t  flags;
    uint8_t  payload[UDP_MAX_DGRAM];
} udp_tx_pkt_t;

/* Separate 32-byte ARM11 MPCore cache lines for producer/consumer indices
 * to avoid SCU/L1 D-cache line ping-pong between Core 0 and Core 2. */
typedef struct {
    NOX_ALIGN32 uint32_t rx_write_idx; /* Core 2 writes (RELEASE), Core 0 reads (ACQUIRE) */
    NOX_ALIGN32 uint32_t rx_read_idx;  /* Core 0 writes (RELEASE), Core 2 reads (ACQUIRE) */
    NOX_ALIGN32 uint32_t tx_write_idx; /* Core 0 writes (RELEASE), Core 2 reads (ACQUIRE) */
    NOX_ALIGN32 uint32_t tx_read_idx;  /* Core 2 writes (RELEASE), Core 0 reads (ACQUIRE) */
    udp_rx_pkt_t         rx_pkts[UDP_RX_RING_CAP];
    udp_tx_pkt_t         tx_pkts[UDP_TX_RING_CAP];
} udp_spsc_rings_t;

typedef struct {
    uint8_t            state;
    uint16_t           gen;
    int                real_fd;
    int                type;
    bool               nonblocking;
    bool               broadcast;
    uint16_t           bound_port;
    uint32_t           last_rx_tick_ms;
    uint32_t           rx_count_this_tick;
    /* Pumped N3DS UDP path (heap-allocated on bind/promote, freed on close): */
    udp_spsc_rings_t  *rings;
    /* Direct O3DS UDP fallback prefetch buffer (heap-allocated per UDP socket): */
    bool               rx_valid;
    uint16_t           rx_len;
    struct sockaddr_in rx_from;
    uint8_t           *rx_buf;
} soc_slot_t;

static soc_slot_t        g_soc_slots[MAX_SOC_SOCKS];
static nox_3ds_mutex_t  *g_soc_mutex       = NULL;
static RecursiveLock     g_dns_lock;
static u32              *g_soc_buffer      = NULL;
static bool              g_soc_inited      = false;
static bool              g_soc_suspended   = false;

/* Cached LAN IPv4/subnet/broadcast info (updated atomically, avoids per-packet SOCU IPC). */
static uint32_t          g_cached_ip_be    = 0u;
static uint32_t          g_cached_mask_be  = 0u;
static uint32_t          g_cached_bcast_be = 0u;
static uint32_t          g_cached_ip_ms    = 0u;

/* New 3DS Core 2 single-owner UDP SPSC pump state. */
static Thread            g_pump_thread     = NULL;
static LightEvent        g_udp_wake_event;
static bool              g_pump_running    = false;
static bool              g_pump_stop       = false;

static void udp_pump_main(void *arg);

static inline uint16_t next_slot_gen(uint16_t cur)
{
    uint16_t next = (uint16_t)(cur + 1u);
    if (next == 0u || next > (uint16_t)SOCKETS_GEN_MAX)
        next = 1u;
    return next;
}

static inline int make_vfd(int slot_idx, uint16_t gen)
{
    return SOCKETS_FD_BASE + (((int)gen << SOCKETS_GEN_SHIFT) | (slot_idx & SOCKETS_SLOT_MASK));
}

static inline bool decode_vfd(int vfd, int *out_slot_idx, uint16_t *out_gen)
{
    if (vfd < SOCKETS_FD_BASE || vfd >= NOX_3DS_SOCKETS_VFD_END)
        return false;
    uint32_t raw = (uint32_t)(vfd - SOCKETS_FD_BASE);
    uint16_t gen = (uint16_t)(raw >> SOCKETS_GEN_SHIFT);
    if (gen == 0u || gen > (uint16_t)SOCKETS_GEN_MAX)
        return false;
    *out_slot_idx = (int)(raw & (uint32_t)SOCKETS_SLOT_MASK);
    *out_gen = gen;
    return true;
}

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
    case EMSGSIZE:      return NOX_WSAEMSGSIZE;
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

static soc_slot_t *lookup_active_slot(int vfd, uint8_t *out_state)
{
    int idx = -1;
    uint16_t gen = 0u;
    if (!decode_vfd(vfd, &idx, &gen))
        return NULL;

    soc_slot_t *s = &g_soc_slots[idx];
    uint8_t st = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
    if (st != SOC_SLOT_SETUP && st != SOC_SLOT_PUMPED && st != SOC_SLOT_DIRECT)
        return NULL;
    if (s->gen != gen || s->real_fd < 0)
        return NULL;
    if (out_state)
        *out_state = st;
    return s;
}

static inline void record_rx_packet_core0(soc_slot_t *s)
{
    uint32_t now_ms = nox_3ds_get_ticks_ms();
    if (s->rx_count_this_tick == 0u ||
        (uint32_t)(now_ms - s->last_rx_tick_ms) >= SOC_DRAIN_WINDOW_MS) {
        s->last_rx_tick_ms = now_ms;
        s->rx_count_this_tick = 1u;
    } else {
        s->rx_count_this_tick++;
    }
}

static inline bool is_rx_capped_core0(soc_slot_t *s)
{
    if (s->rx_count_this_tick < SOC_MAX_DRAIN_PER_TICK)
        return false;
    uint32_t now_ms = nox_3ds_get_ticks_ms();
    if ((uint32_t)(now_ms - s->last_rx_tick_ms) >= SOC_DRAIN_WINDOW_MS) {
        s->last_rx_tick_ms = now_ms;
        s->rx_count_this_tick = 0u;
        return false;
    }
    return true;
}

/* =========================================================================
 * New 3DS Core 2 Single-Owner UDP SPSC Worker
 *   - Never touches last_socket_error or SDMC files.
 *   - Sole thread calling sendto/recvfrom/close on SOC_SLOT_PUMPED/CLOSING FDs.
 * ========================================================================= */
static bool udp_pump_drain_tx_slot(soc_slot_t *s)
{
    udp_spsc_rings_t *rings = s->rings;
    int rfd = s->real_fd;
    if (!rings || rfd < 0)
        return false;

    uint32_t r = rings->tx_read_idx;
    uint32_t w = __atomic_load_n(&rings->tx_write_idx, __ATOMIC_ACQUIRE);
    if (r == w)
        return false;

    bool sent_any = false;
    bool suspended = __atomic_load_n(&g_soc_suspended, __ATOMIC_ACQUIRE);
    uint32_t bcast_be = __atomic_load_n(&g_cached_bcast_be, __ATOMIC_ACQUIRE);

    while (r != w) {
        const udp_tx_pkt_t *pkt = &rings->tx_pkts[r & UDP_TX_RING_MASK];
        if (!suspended) {
            struct sockaddr_in dst_sin;
            memset(&dst_sin, 0, sizeof(dst_sin));
            dst_sin.sin_family = AF_INET;
            dst_sin.sin_port = pkt->dst_port;
            dst_sin.sin_addr.s_addr = pkt->dst_ip;

            ssize_t r_dir = -1;
            if (pkt->dst_ip == htonl(INADDR_BROADCAST)) {
                if (bcast_be == 0u) {
                    uint32_t ip_tmp = 0u, mask_tmp = 0u;
                    (void)nox_3ds_soc_get_ip_info(&ip_tmp, &mask_tmp, &bcast_be);
                }
                if (bcast_be != 0u && bcast_be != htonl(INADDR_BROADCAST)) {
                    struct sockaddr_in dir_sin = dst_sin;
                    dir_sin.sin_addr.s_addr = bcast_be;
                    r_dir = sendto(rfd, pkt->payload, (size_t)pkt->len, pkt->flags,
                                   (const struct sockaddr *)&dir_sin, (socklen_t)sizeof(dir_sin));
                }
            }

            ssize_t rc = sendto(rfd, pkt->payload, (size_t)pkt->len, pkt->flags,
                                (const struct sockaddr *)&dst_sin, (socklen_t)sizeof(dst_sin));
            if (rc < 0 && r_dir < 0) {
                int e = (errno > 0) ? errno : -errno;
                if (e == 0 && rc < -1)
                    e = -(int)rc;
                /* If soc:u kernel SNDBUF is momentarily saturated on an active socket,
                 * leave remaining datagrams in the TX ring and retry after ~1.5 ms. */
                if ((e == EWOULDBLOCK || e == EAGAIN || e == ENOBUFS) &&
                    __atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == SOC_SLOT_PUMPED) {
                    break;
                }
            }
        }
        r++;
        sent_any = true;
    }

    if (sent_any)
        __atomic_store_n(&rings->tx_read_idx, r, __ATOMIC_RELEASE);
    return sent_any;
}

static bool udp_pump_drain_rx_slot(soc_slot_t *s)
{
    udp_spsc_rings_t *rings = s->rings;
    int rfd = s->real_fd;
    if (!rings || rfd < 0)
        return false;

    uint32_t w = rings->rx_write_idx;
    uint32_t r = __atomic_load_n(&rings->rx_read_idx, __ATOMIC_ACQUIRE);
    int rx_flags = SOC_SAFE_RECV_FLAGS(0, s->nonblocking);
    bool got_any = false;

    /* Drain up to 64 datagrams per pass per socket so multi-player bursts clear
     * soc:u's kernel queue immediately while keeping multiple sockets fair. */
    for (uint32_t pass = 0u; pass < 64u; ++pass) {
        if ((uint32_t)(w - r) >= UDP_RX_RING_CAP) {
            r = __atomic_load_n(&rings->rx_read_idx, __ATOMIC_ACQUIRE);
            if ((uint32_t)(w - r) >= UDP_RX_RING_CAP)
                break; /* Backpressure: hold extra burst in 64 KB kernel SO_RCVBUF */
        }

        udp_rx_pkt_t *pkt = &rings->rx_pkts[w & UDP_RX_RING_MASK];
        struct sockaddr_in src_sin;
        socklen_t src_len = (socklen_t)sizeof(src_sin);
        memset(&src_sin, 0, sizeof(src_sin));

        ssize_t got = recvfrom(rfd, pkt->payload, sizeof(pkt->payload), rx_flags,
                               (struct sockaddr *)&src_sin, &src_len);
        if (got >= 0) {
            size_t store_n = ((size_t)got <= UDP_MAX_DGRAM) ? (size_t)got : UDP_MAX_DGRAM;
            pkt->src_ip = (uint32_t)src_sin.sin_addr.s_addr;
            pkt->src_port = (uint16_t)src_sin.sin_port;
            pkt->len = (uint16_t)store_n;
            w++;
            __atomic_store_n(&rings->rx_write_idx, w, __ATOMIC_RELEASE);
            got_any = true;
            continue;
        }

        int e = (errno > 0) ? errno : -errno;
        if (e == 0 && got < -1)
            e = -(int)got;
        if (e == ECONNRESET || e == ECONNREFUSED || e == EHOSTUNREACH || e == ENETUNREACH) {
            /* Clear transient ICMP Port/Host Unreachable (e.g. LAN sweep probes). */
            continue;
        }
        break;
    }

    return got_any;
}

static void udp_pump_close_slot(soc_slot_t *s)
{
    /* Transmit any final queued packets (e.g. op=10/op=11 disconnect) before close(). */
    if (s->rings && !__atomic_load_n(&g_pump_stop, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&g_soc_suspended, __ATOMIC_ACQUIRE)) {
        (void)udp_pump_drain_tx_slot(s);
    }

    if (s->real_fd >= 0) {
        close(s->real_fd);
        s->real_fd = -1;
    }

    if (s->rings) {
        free(s->rings);
        s->rings = NULL;
    }
    if (s->rx_buf) {
        free(s->rx_buf);
        s->rx_buf = NULL;
    }
    s->rx_valid = false;
    s->rx_len = 0u;
    s->bound_port = 0u;

    __atomic_store_n(&s->state, SOC_SLOT_FREE, __ATOMIC_RELEASE);
}

static void udp_pump_main(void *arg)
{
    (void)arg;

    while (!__atomic_load_n(&g_pump_stop, __ATOMIC_ACQUIRE)) {
        int active_pumped = 0;
        bool did_work = false;
        bool suspended = __atomic_load_n(&g_soc_suspended, __ATOMIC_ACQUIRE);

        for (int i = 0; i < MAX_SOC_SOCKS; ++i) {
            soc_slot_t *s = &g_soc_slots[i];
            uint8_t st = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);

            if (st == SOC_SLOT_CLOSING) {
                udp_pump_close_slot(s);
                did_work = true;
                continue;
            }

            if (st == SOC_SLOT_PUMPED) {
                active_pumped++;
                if (udp_pump_drain_tx_slot(s))
                    did_work = true;

                /* Re-check in case Core 0 requested close concurrently during TX drain. */
                if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == SOC_SLOT_CLOSING) {
                    udp_pump_close_slot(s);
                    active_pumped--;
                    did_work = true;
                    continue;
                }

                if (!suspended && udp_pump_drain_rx_slot(s))
                    did_work = true;
            }
        }

        if (__atomic_load_n(&g_pump_stop, __ATOMIC_ACQUIRE))
            break;

        if (active_pumped == 0) {
            /* Zero active UDP sockets: park in infinite kernel wait (0.0% CPU, 0 IPCs). */
            LightEvent_Wait(&g_udp_wake_event);
        } else if (suspended) {
            LightEvent_WaitTimeout(&g_udp_wake_event, UDP_PUMP_SUSPEND_NS);
        } else if (!did_work) {
            /* Sleep up to 1.5 ms unless woken immediately (<5 us) by Core 0 TX/close/bind. */
            LightEvent_WaitTimeout(&g_udp_wake_event, UDP_PUMP_POLL_NS);
        }
    }
}

static bool promote_udp_slot_to_pumped_locked(soc_slot_t *s)
{
    if (!__atomic_load_n(&g_pump_running, __ATOMIC_ACQUIRE))
        return false;
    if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != SOC_SLOT_SETUP)
        return (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == SOC_SLOT_PUMPED);

    udp_spsc_rings_t *rings = (udp_spsc_rings_t *)memalign(32u, sizeof(udp_spsc_rings_t));
    if (!rings) {
        /* Graceful OOM fallback: switch this socket to direct Core 0 mode. */
        s->rx_buf = (uint8_t *)malloc(UDP_MAX_DGRAM);
        s->rx_valid = false;
        s->rx_len = 0u;
        __atomic_store_n(&s->state, SOC_SLOT_DIRECT, __ATOMIC_RELEASE);
        nox_3ds_net_log("[net3ds:soc] WARN: SPSC ring alloc failed; falling back to Core 0 direct UDP");
        return false;
    }

    rings->rx_write_idx = 0u;
    rings->rx_read_idx  = 0u;
    rings->tx_write_idx = 0u;
    rings->tx_read_idx  = 0u;
    s->rings = rings;

    /* Hand exclusive soc:u FD ownership to Core 2 with full release barrier. */
    __atomic_store_n(&s->state, SOC_SLOT_PUMPED, __ATOMIC_RELEASE);
    LightEvent_Signal(&g_udp_wake_event);
    return true;
}

static int bind_ephemeral_udp_port(int rfd)
{
    static uint16_t s_ephem_cursor = 0u;
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = 0u;

    int last_err = EADDRINUSE;
    for (uint16_t k = 0u; k < 64u; ++k) {
        uint16_t cand = 0u;
        soc_lock();
        if (s_ephem_cursor == 0u) {
            s_ephem_cursor = (uint16_t)(49152u + ((uint32_t)svcGetSystemTick() & 8191u));
        }
        for (uint16_t probe = 0u; probe < 8192u; ++probe) {
            uint16_t p = (uint16_t)(49152u + (((s_ephem_cursor - 49152u) + probe) & 8191u));
            bool used = false;
            for (int i = 0; i < MAX_SOC_SOCKS; ++i) {
                uint8_t st = __atomic_load_n(&g_soc_slots[i].state, __ATOMIC_ACQUIRE);
                if (st != SOC_SLOT_FREE && g_soc_slots[i].bound_port == p) {
                    used = true;
                    break;
                }
            }
            if (!used) {
                cand = p;
                s_ephem_cursor = (uint16_t)(49152u + (((p - 49152u) + 1u) & 8191u));
                break;
            }
        }
        soc_unlock();
        if (cand == 0u)
            cand = (uint16_t)(49152u + (k & 8191u));

        sin.sin_port = htons(cand);
        if (bind(rfd, (const struct sockaddr *)&sin, (socklen_t)sizeof(sin)) == 0)
            return (int)cand;

        last_err = (errno > 0) ? errno : -errno;
        if (last_err != EADDRINUSE) {
            errno = last_err;
            return -1;
        }
    }
    errno = last_err;
    return -1;
}

/* =========================================================================
 * Lazy soc:u Initialization & Teardown
 * ========================================================================= */
static bool soc_ensure_init_locked(void)
{
    if (__atomic_load_n(&g_soc_inited, __ATOMIC_ACQUIRE))
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
        nox_3ds_net_set_last_error(NOX_WSAENOBUFS);
        errno = ENOMEM;
        return false;
    }

    Result rc = socInit(g_soc_buffer, SOC_BUFFERSIZE);
    if (R_FAILED(rc)) {
        nox_3ds_net_log("[net3ds:soc] ERROR: socInit failed (rc=0x%08X)", (unsigned)rc);
        free(g_soc_buffer);
        g_soc_buffer = NULL;
        nox_3ds_net_set_last_error(NOX_WSAENETDOWN);
        errno = ENETDOWN;
        return false;
    }

    RecursiveLock_Init(&g_dns_lock);

    for (int i = 0; i < MAX_SOC_SOCKS; ++i) {
        uint16_t keep_gen = g_soc_slots[i].gen;
        memset(&g_soc_slots[i], 0, sizeof(g_soc_slots[i]));
        g_soc_slots[i].state = SOC_SLOT_FREE;
        g_soc_slots[i].gen = keep_gen;
        g_soc_slots[i].real_fd = -1;
    }

    __atomic_store_n(&g_cached_ip_be, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_cached_mask_be, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_cached_bcast_be, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_cached_ip_ms, 0u, __ATOMIC_RELEASE);

    __atomic_store_n(&g_soc_suspended, false, __ATOMIC_RELEASE);
    __atomic_store_n(&g_soc_inited, true, __ATOMIC_RELEASE);
    __atomic_store_n(&nox_3ds_state_mut()->soc_ready, true, __ATOMIC_RELEASE);

    /* Spawn the dedicated Core 2 UDP SPSC pump on New 3DS (804 MHz 4-core KTR).
     * Never fall back to Core 0 (-2) on Old 3DS or if Core 2 creation is denied,
     * keeping Old 3DS strictly on the zero-context-switch synchronous Core 0 path. */
    if (nox_3ds_is_new_3ds() && !__atomic_load_n(&g_pump_running, __ATOMIC_ACQUIRE)) {
        LightEvent_Init(&g_udp_wake_event, RESET_ONESHOT);
        __atomic_store_n(&g_pump_stop, false, __ATOMIC_RELEASE);

        s32 main_prio = 0x30;
        svcGetThreadPriority(&main_prio, CUR_THREAD_HANDLE);
        s32 pump_prio = (main_prio + 1 <= 0x3D) ? (main_prio + 1) : 0x3D;
        if (pump_prio < 0x18)
            pump_prio = 0x18;

        g_pump_thread = threadCreate(udp_pump_main, NULL, UDP_PUMP_STACK_SIZE,
                                     pump_prio, 2, false);
        if (g_pump_thread) {
            __atomic_store_n(&g_pump_running, true, __ATOMIC_RELEASE);
            nox_3ds_net_log("[net3ds:soc] Core 2 single-owner UDP SPSC pump started (stack=32KB prio=0x%lX)",
                            (unsigned long)pump_prio);
        } else {
            nox_3ds_net_log("[net3ds:soc] WARN: Core 2 threadCreate failed; using direct Core 0 UDP fallback");
        }
    } else if (!nox_3ds_is_new_3ds()) {
        nox_3ds_net_log("[net3ds:soc] Old 3DS detected; using direct Core 0 UDP path");
    }

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
    if (!__atomic_load_n(&g_soc_inited, __ATOMIC_ACQUIRE)) {
        if (out_ip_be)    *out_ip_be = 0;
        if (out_mask_be)  *out_mask_be = 0;
        if (out_bcast_be) *out_bcast_be = 0;
        return false;
    }

    uint32_t now_ms = nox_3ds_get_ticks_ms();
    uint32_t cached_ip = __atomic_load_n(&g_cached_ip_be, __ATOMIC_ACQUIRE);
    uint32_t cached_ms = __atomic_load_n(&g_cached_ip_ms, __ATOMIC_ACQUIRE);
    if (cached_ip != 0u && cached_ms != 0u && (uint32_t)(now_ms - cached_ms) < SOC_IP_CACHE_TTL_MS) {
        if (out_ip_be)    *out_ip_be    = cached_ip;
        if (out_mask_be)  *out_mask_be  = __atomic_load_n(&g_cached_mask_be, __ATOMIC_ACQUIRE);
        if (out_bcast_be) *out_bcast_be = __atomic_load_n(&g_cached_bcast_be, __ATOMIC_ACQUIRE);
        return true;
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

    if (ip != 0u) {
        __atomic_store_n(&g_cached_mask_be, mask, __ATOMIC_RELEASE);
        __atomic_store_n(&g_cached_bcast_be, bcast, __ATOMIC_RELEASE);
        __atomic_store_n(&g_cached_ip_be, ip, __ATOMIC_RELEASE);
        __atomic_store_n(&g_cached_ip_ms, now_ms ? now_ms : 1u, __ATOMIC_RELEASE);
    }

    if (out_ip_be)    *out_ip_be = ip;
    if (out_mask_be)  *out_mask_be = mask;
    if (out_bcast_be) *out_bcast_be = bcast;
    return ip != 0u;
}

bool nox_3ds_soc_is_ready(void)
{
    return __atomic_load_n(&g_soc_inited, __ATOMIC_ACQUIRE);
}

bool nox_3ds_soc_ensure_ready(void)
{
    if (nox_3ds_net_get_mode() != NOX_3DS_NET_MODE_WIFI)
        return false;
    if (__atomic_load_n(&g_soc_inited, __ATOMIC_ACQUIRE))
        return true;
    soc_lock();
    bool ok = soc_ensure_init_locked();
    soc_unlock();
    return ok;
}

bool nox_3ds_soc_resolve_ipv4(const char *hostname, uint32_t *out_ip_be)
{
    if (!hostname || !*hostname || !out_ip_be || !__atomic_load_n(&g_soc_inited, __ATOMIC_ACQUIRE))
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
    __atomic_store_n(&g_soc_suspended, suspended, __ATOMIC_RELEASE);
    if (__atomic_load_n(&g_pump_running, __ATOMIC_ACQUIRE))
        LightEvent_Signal(&g_udp_wake_event);
}

static void sockets_init(void)
{
    /* Deliberately no-op: socInit() is strictly lazy on Multiplayer menu entry. */
}

static void sockets_shutdown(void)
{
    if (!__atomic_load_n(&g_soc_inited, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&g_pump_running, __ATOMIC_ACQUIRE) &&
        !g_soc_buffer && !g_soc_mutex) {
        return;
    }

    /* 1. Stop and join Core 2 UDP pump OUTSIDE g_soc_mutex so no lock-order
     *    cycle is ever possible and Core 2 exits all soc:u calls cleanly. */
    bool pump_joined = true;
    if (__atomic_load_n(&g_pump_running, __ATOMIC_ACQUIRE) && g_pump_thread) {
        __atomic_store_n(&g_pump_stop, true, __ATOMIC_RELEASE);
        LightEvent_Signal(&g_udp_wake_event);

        Result jrc = threadJoin(g_pump_thread, 3000000000ULL);
        if (R_SUCCEEDED(jrc)) {
            threadFree(g_pump_thread);
            g_pump_thread = NULL;
            __atomic_store_n(&g_pump_running, false, __ATOMIC_RELEASE);
        } else {
            pump_joined = false;
            printf("[net3ds:soc] WARN: Core 2 UDP pump threadJoin timed out (0x%08lX); skipping socExit\n",
                   (unsigned long)jrc);
        }
    }

    /* Also verify lobby_3ds's worker exited cleanly (httpc_ready is cleared on
     * successful join in nox_3ds_lobby_shutdown()). Never revoke g_soc_buffer
     * userland page permissions via socExit()/free() if a worker is still alive! */
    bool lobby_still_active = __atomic_load_n(&nox_3ds_get_state()->httpc_ready, __ATOMIC_ACQUIRE);
    if (!pump_joined || lobby_still_active) {
        printf("[net3ds:soc] WARN: worker still active (pump_joined=%d, lobby_active=%d); preserving soc:u buffer\n",
               pump_joined ? 1 : 0, lobby_still_active ? 1 : 0);
        return;
    }

    if (g_soc_mutex)
        nox_3ds_mutex_lock(g_soc_mutex);

    for (int i = 0; i < MAX_SOC_SOCKS; ++i) {
        soc_slot_t *s = &g_soc_slots[i];
        uint8_t st = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
        if (st != SOC_SLOT_FREE && s->real_fd >= 0) {
            close(s->real_fd);
        }
        if (s->rings) {
            free(s->rings);
            s->rings = NULL;
        }
        if (s->rx_buf) {
            free(s->rx_buf);
            s->rx_buf = NULL;
        }
        s->real_fd = -1;
        s->rx_valid = false;
        s->rx_len = 0u;
        s->bound_port = 0u;
        __atomic_store_n(&s->state, SOC_SLOT_FREE, __ATOMIC_RELEASE);
    }

    if (__atomic_load_n(&g_soc_inited, __ATOMIC_ACQUIRE)) {
        socExit();
        __atomic_store_n(&g_soc_inited, false, __ATOMIC_RELEASE);
        __atomic_store_n(&nox_3ds_state_mut()->soc_ready, false, __ATOMIC_RELEASE);
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

/* =========================================================================
 * Socket Operations (Core 0 Entry Points)
 * ========================================================================= */
static int sockets_socket(int domain, int type, int protocol)
{
    soc_lock();
    if (!soc_ensure_init_locked()) {
        soc_unlock();
        return -1;
    }

    int slot_idx = -1;
    for (int attempt = 0; attempt < 4 && slot_idx < 0; ++attempt) {
        for (int i = 0; i < MAX_SOC_SOCKS; ++i) {
            if (__atomic_load_n(&g_soc_slots[i].state, __ATOMIC_ACQUIRE) == SOC_SLOT_FREE) {
                slot_idx = i;
                break;
            }
        }
        if (slot_idx < 0 && __atomic_load_n(&g_pump_running, __ATOMIC_ACQUIRE)) {
            soc_unlock();
            LightEvent_Signal(&g_udp_wake_event);
            svcSleepThread(500000LL); /* 0.5 ms to let Core 2 retire SOC_SLOT_CLOSING */
            soc_lock();
        } else {
            break;
        }
    }

    if (slot_idx < 0) {
        nox_3ds_net_set_last_error(NOX_WSAEMFILE);
        errno = EMFILE;
        soc_unlock();
        return -1;
    }

    bool use_pump = (type == SOCK_DGRAM) && __atomic_load_n(&g_pump_running, __ATOMIC_ACQUIRE);
    uint8_t *direct_rx_buf = NULL;
    if (type == SOCK_DGRAM && !use_pump) {
        direct_rx_buf = (uint8_t *)malloc(UDP_MAX_DGRAM);
        if (!direct_rx_buf) {
            nox_3ds_net_set_last_error(NOX_WSAENOBUFS);
            errno = ENOMEM;
            soc_unlock();
            return -1;
        }
    }

    int rfd = socket(domain, type, protocol);
    if (rfd < 0) {
        unsigned int wsa = wsa_from_errno(errno);
        free(direct_rx_buf);
        nox_3ds_net_set_last_error(wsa);
        printf("[net3ds:soc] socket(dom=%d, type=%d, proto=%d) FAILED errno=%d wsa=%u\n",
               domain, type, protocol, errno, wsa);
        soc_unlock();
        return -1;
    }

    /* Enforce strictly non-blocking mode immediately so dropped UDP packets
     * or TOCTOU races can NEVER hang the ARM11 30 Hz main loop or Core 2 pump. */
    bool nonblock_ok = false;
    int fl = fcntl(rfd, F_GETFL, 0);
    if (fl >= 0) {
        if (fcntl(rfd, F_SETFL, fl | O_NONBLOCK) == 0) {
            nonblock_ok = true;
        } else {
            nox_3ds_net_log("[net3ds:soc] WARN: fcntl(O_NONBLOCK) failed on rfd=%d errno=%d", rfd, errno);
        }
    } else {
        nox_3ds_net_log("[net3ds:soc] WARN: fcntl(F_GETFL) failed on rfd=%d errno=%d", rfd, errno);
    }

    if (type == SOCK_DGRAM) {
        int reuse = 1;
        (void)setsockopt(rfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        /* Request larger UDP socket buffers (best-effort; soc:u may clamp/ignore)
         * so multi-player state bursts don't silently overflow the RX queue.
         * Keep conservative enough to stay well within the 1 MB socInit() pool. */
        int rcvbuf = 64 * 1024;
        if (setsockopt(rfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
            rcvbuf = 32 * 1024;
            (void)setsockopt(rfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        }
        int sndbuf = 32 * 1024;
        (void)setsockopt(rfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        int eff_rcv = 0, eff_snd = 0;
        socklen_t opt_len = (socklen_t)sizeof(eff_rcv);
        (void)getsockopt(rfd, SOL_SOCKET, SO_RCVBUF, &eff_rcv, &opt_len);
        opt_len = (socklen_t)sizeof(eff_snd);
        (void)getsockopt(rfd, SOL_SOCKET, SO_SNDBUF, &eff_snd, &opt_len);
        nox_3ds_net_log("[net3ds:soc] UDP rfd=%d buffers SO_RCVBUF=%d SO_SNDBUF=%d (pump=%d)",
                        rfd, eff_rcv, eff_snd, use_pump ? 1 : 0);
    }

    soc_slot_t *s = &g_soc_slots[slot_idx];
    uint16_t gen = next_slot_gen(s->gen);
    memset(s, 0, sizeof(*s));
    s->gen         = gen;
    s->real_fd     = rfd;
    s->type        = type;
    s->nonblocking = nonblock_ok;
    s->rx_buf      = direct_rx_buf;

    uint8_t init_state = use_pump ? SOC_SLOT_SETUP : SOC_SLOT_DIRECT;
    __atomic_store_n(&s->state, init_state, __ATOMIC_RELEASE);

    int vfd = make_vfd(slot_idx, gen);
    nox_3ds_net_set_last_error(0);
    soc_unlock();
    printf("[net3ds:soc] socket(dom=%d, type=%d, proto=%d) -> vfd=%d (slot=%d gen=%u rfd=%d mode=%s)\n",
           domain, type, protocol, vfd, slot_idx, (unsigned)gen, rfd,
           use_pump ? "CORE2_SETUP" : "DIRECT");
    return vfd;
}

static int sockets_closesocket(int vfd)
{
    soc_lock();
    uint8_t st = SOC_SLOT_FREE;
    soc_slot_t *s = lookup_active_slot(vfd, &st);
    if (!s) {
        nox_3ds_net_set_last_error(NOX_WSAENOTSOCK);
        errno = EBADF;
        soc_unlock();
        return -1;
    }

    /* Immediately bump generation so any stale vfd reference fails lookup_active_slot(). */
    s->gen = next_slot_gen(s->gen);

    if ((st == SOC_SLOT_PUMPED || st == SOC_SLOT_SETUP) &&
        __atomic_load_n(&g_pump_running, __ATOMIC_ACQUIRE)) {
        /* Hand off close(real_fd) + ring teardown exclusively to Core 2! */
        __atomic_store_n(&s->state, SOC_SLOT_CLOSING, __ATOMIC_RELEASE);
        soc_unlock();
        LightEvent_Signal(&g_udp_wake_event);
        nox_3ds_net_set_last_error(0);
        return 0;
    }

    int rfd = s->real_fd;
    uint8_t *rx_buf = s->rx_buf;
    udp_spsc_rings_t *rings = s->rings;
    s->real_fd = -1;
    s->rx_buf = NULL;
    s->rings = NULL;
    s->rx_valid = false;
    s->rx_len = 0u;
    s->bound_port = 0u;
    __atomic_store_n(&s->state, SOC_SLOT_FREE, __ATOMIC_RELEASE);
    soc_unlock();

    free(rx_buf);
    free(rings);

    int r = close(rfd);
    if (r < 0) {
        nox_3ds_net_set_last_error(wsa_from_errno(errno));
        return -1;
    }
    nox_3ds_net_set_last_error(0);
    return 0;
}

static int sockets_bind(int vfd, const void *addr, unsigned int addrlen)
{
    if (!addr || addrlen < sizeof(struct sockaddr_in)) {
        nox_3ds_net_set_last_error(NOX_WSAEINVAL);
        errno = EINVAL;
        return -1;
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    memcpy(&sin, addr, sizeof(sin));
    sin.sin_family = AF_INET;
    memset(sin.sin_zero, 0, sizeof(sin.sin_zero));

    soc_lock();
    uint8_t st = SOC_SLOT_FREE;
    soc_slot_t *s = lookup_active_slot(vfd, &st);
    if (!s) {
        nox_3ds_net_set_last_error(NOX_WSAENOTSOCK);
        errno = EBADF;
        soc_unlock();
        return -1;
    }
    if (st == SOC_SLOT_PUMPED) {
        /* Already bound and handed off to Core 2; never touch real_fd on Core 0. */
        nox_3ds_net_set_last_error(NOX_WSAEINVAL);
        errno = EINVAL;
        soc_unlock();
        return -1;
    }
    int rfd = s->real_fd;
    int stype = s->type;
    s->rx_valid = false;
    s->rx_len = 0u;
    soc_unlock();

    /* If a caller just closed a pumped UDP socket on this exact port (e.g. mode-switch
     * discovery reopen in GAME5.c:12131), wait briefly for Core 2 to finish close(old_rfd)
     * so bind() never hits a spurious EADDRINUSE against our own closing descriptor. */
    uint16_t req_port = ntohs(sin.sin_port);
    if (req_port != 0u && __atomic_load_n(&g_pump_running, __ATOMIC_ACQUIRE)) {
        for (int wait_i = 0; wait_i < 25; ++wait_i) {
            bool closing_same_port = false;
            for (int i = 0; i < MAX_SOC_SOCKS; ++i) {
                if (__atomic_load_n(&g_soc_slots[i].state, __ATOMIC_ACQUIRE) == SOC_SLOT_CLOSING &&
                    g_soc_slots[i].bound_port == req_port) {
                    closing_same_port = true;
                    break;
                }
            }
            if (!closing_same_port)
                break;
            LightEvent_Signal(&g_udp_wake_event);
            svcSleepThread(200000LL); /* 0.2 ms */
        }
    }

    int r = -1;
    uint16_t bport = req_port;
    if (stype == SOCK_DGRAM && req_port == 0u) {
        int ephem = bind_ephemeral_udp_port(rfd);
        if (ephem > 0) {
            r = 0;
            bport = (uint16_t)ephem;
            sin.sin_port = htons(bport);
            nox_3ds_net_log("[net3ds:soc] bind vfd=%d rfd=%d explicit_ephem=%u -> rc=0",
                            vfd, rfd, (unsigned)bport);
        }
    } else {
        r = bind(rfd, (const struct sockaddr *)&sin, (socklen_t)sizeof(sin));
    }

    if (r < 0) {
        unsigned int wsa = wsa_from_errno(errno);
        nox_3ds_net_set_last_error(wsa);
        nox_3ds_net_log("[net3ds:soc] bind(vfd=%d, rfd=%d, port=%u) FAILED errno=%d wsa=%u",
                        vfd, rfd, (unsigned)ntohs(sin.sin_port), errno, wsa);
        return -1;
    }

    soc_lock();
    s = lookup_active_slot(vfd, &st);
    bool promoted = false;
    if (s) {
        s->bound_port = bport;
        if (st == SOC_SLOT_SETUP)
            promoted = promote_udp_slot_to_pumped_locked(s);
    }
    soc_unlock();

    nox_3ds_net_set_last_error(0);
    if (bport == 18590u || bport < 49152u) {
        nox_3ds_net_log("[net3ds:soc] bind(vfd=%d, rfd=%d, port=%u, core2_pump=%d) OK",
                        vfd, rfd, (unsigned)bport, promoted ? 1 : 0);
    }
    printf("[net3ds:soc] bind(vfd=%d, rfd=%d, port=%u, pump=%d) -> ok\n",
           vfd, rfd, (unsigned)bport, promoted ? 1 : 0);
    return 0;
}

static int sockets_setsockopt(int vfd, int level, int opt, const void *val, unsigned int len)
{
    soc_lock();
    uint8_t st = SOC_SLOT_FREE;
    soc_slot_t *s = lookup_active_slot(vfd, &st);
    if (!s) {
        nox_3ds_net_set_last_error(NOX_WSAENOTSOCK);
        errno = EBADF;
        soc_unlock();
        return -1;
    }

    int orig_level = level;
    if (level == 0xFFFF && opt == 0x20) {
        /* libctru defines SO_BROADCAST as 0x0000 (broadcast is always enabled on soc:u).
         * Short-circuit here with zero soc:u IPC (safe even on SOC_SLOT_PUMPED). */
        if (val && len >= sizeof(int)) {
            int on = 0;
            memcpy(&on, val, sizeof(int));
            s->broadcast = (on != 0);
        }
        soc_unlock();
        nox_3ds_net_set_last_error(0);
        return 0;
    }

    if (st == SOC_SLOT_PUMPED) {
        /* Core 2 owns real_fd exclusively; absorb any post-bind socket option cleanly. */
        if (level == SOL_SOCKET && opt == SO_BROADCAST && val && len >= sizeof(int)) {
            int on = 0;
            memcpy(&on, val, sizeof(int));
            s->broadcast = (on != 0);
        }
        soc_unlock();
        nox_3ds_net_set_last_error(0);
        return 0;
    }

    int rfd = s->real_fd;
    soc_unlock();

    int r = setsockopt(rfd, level, opt, val, (socklen_t)len);
    if (r < 0) {
        /* Gracefully absorb unsupported Windows/BSD options (matching src/compat.c:629)
         * so LAN/discovery socket setup never aborts on soc:u quirks. */
        if (orig_level == 0xFFFF || errno == ENOPROTOOPT || errno == EINVAL) {
            printf("[net3ds:soc] setsockopt(vfd=%d, lvl=%d, opt=%d) unsupported (errno=%d); ignoring\n",
                   vfd, level, opt, errno);
            nox_3ds_net_set_last_error(0);
            return 0;
        }
        nox_3ds_net_set_last_error(wsa_from_errno(errno));
        return -1;
    }

    if (level == SOL_SOCKET && opt == SO_BROADCAST && val && len >= sizeof(int)) {
        int on = 0;
        memcpy(&on, val, sizeof(int));
        soc_lock();
        s = lookup_active_slot(vfd, NULL);
        if (s)
            s->broadcast = (on != 0);
        soc_unlock();
    }

    nox_3ds_net_set_last_error(0);
    return 0;
}

static int sockets_ioctlsocket(int vfd, long cmd, unsigned long *argp)
{
    uint32_t ucmd = (uint32_t)cmd;
    if (!argp) {
        nox_3ds_net_set_last_error(NOX_WSAEINVAL);
        errno = EINVAL;
        return -1;
    }

    /* Lock-free Core 0 SPSC fast path for pumped New 3DS UDP sockets (~5 CPU cycles, 0 IPCs). */
    int fast_idx = -1;
    uint16_t fast_gen = 0u;
    if (decode_vfd(vfd, &fast_idx, &fast_gen)) {
        soc_slot_t *fs = &g_soc_slots[fast_idx];
        uint8_t fst = __atomic_load_n(&fs->state, __ATOMIC_ACQUIRE);
        if (fst == SOC_SLOT_PUMPED && fs->gen == fast_gen) {
            if (ucmd == NOX_FIONREAD) {
                if (__atomic_load_n(&g_soc_suspended, __ATOMIC_ACQUIRE) || is_rx_capped_core0(fs)) {
                    *argp = 0ul;
                    nox_3ds_net_set_last_error(0);
                    return 0;
                }
                udp_spsc_rings_t *rings = fs->rings;
                uint32_t r = rings->rx_read_idx;
                uint32_t w = __atomic_load_n(&rings->rx_write_idx, __ATOMIC_ACQUIRE);
                if (r != w) {
                    uint16_t n = rings->rx_pkts[r & UDP_RX_RING_MASK].len;
                    *argp = (unsigned long)(n > 0u ? n : 1u);
                } else {
                    *argp = 0ul;
                }
                nox_3ds_net_set_last_error(0);
                return 0;
            }
            if (ucmd == NOX_FIONBIO) {
                /* UDP sockets on ARM11 are permanently O_NONBLOCK; zero fcntl IPC needed. */
                nox_3ds_net_set_last_error(0);
                return 0;
            }
            nox_3ds_net_set_last_error(NOX_WSAEINVAL);
            errno = EINVAL;
            return -1;
        }
    }

    soc_lock();
    uint8_t st = SOC_SLOT_FREE;
    soc_slot_t *s = lookup_active_slot(vfd, &st);
    if (!s) {
        nox_3ds_net_set_last_error(NOX_WSAENOTSOCK);
        errno = EBADF;
        soc_unlock();
        return -1;
    }

    if (st == SOC_SLOT_SETUP) {
        /* Unbound pre-handoff N3DS UDP socket: always O_NONBLOCK and 0 RX bytes. */
        soc_unlock();
        if (ucmd == NOX_FIONBIO) {
            nox_3ds_net_set_last_error(0);
            return 0;
        }
        if (ucmd == NOX_FIONREAD) {
            *argp = 0ul;
            nox_3ds_net_set_last_error(0);
            return 0;
        }
        nox_3ds_net_set_last_error(NOX_WSAEINVAL);
        errno = EINVAL;
        return -1;
    }

    int rfd = s->real_fd;
    int stype = s->type;
    bool nb = s->nonblocking;
    bool capped = is_rx_capped_core0(s);
    soc_unlock();

    if (ucmd == NOX_FIONBIO) {
        /* Always keep UDP sockets non-blocking on ARM11 to prevent thread hangs. */
        int nonblock = (stype == SOCK_DGRAM) ? 1 : (*argp != 0ul ? 1 : 0);
        if (stype != SOCK_DGRAM) {
            int fl = fcntl(rfd, F_GETFL, 0);
            if (fl >= 0) {
                if (nonblock) fl |= O_NONBLOCK;
                else          fl &= ~O_NONBLOCK;
                (void)fcntl(rfd, F_SETFL, fl);
            }
        }
        soc_lock();
        s = lookup_active_slot(vfd, NULL);
        if (s)
            s->nonblocking = (nonblock != 0);
        soc_unlock();
        nox_3ds_net_set_last_error(0);
        return 0;
    }

    if (ucmd == NOX_FIONREAD) {
        if (__atomic_load_n(&g_soc_suspended, __ATOMIC_ACQUIRE) || capped) {
            *argp = 0ul;
            nox_3ds_net_set_last_error(0);
            return 0;
        }

        if (stype == SOCK_DGRAM) {
            /* Old 3DS direct fallback: prefetch one datagram directly into s->rx_buf
             * (zero intermediate copy) and drain transient ICMP unreachable errors. */
            soc_lock();
            s = lookup_active_slot(vfd, &st);
            if (!s || st != SOC_SLOT_DIRECT || !s->rx_buf) {
                soc_unlock();
                nox_3ds_net_set_last_error(NOX_WSAENOTSOCK);
                errno = EBADF;
                *argp = 0ul;
                return -1;
            }
            if (s->rx_valid) {
                uint16_t n = s->rx_len;
                soc_unlock();
                *argp = (unsigned long)(n > 0u ? n : 1u);
                nox_3ds_net_set_last_error(0);
                return 0;
            }

            ssize_t got = -1;
            int got_err = EWOULDBLOCK;
            int rx_flags = SOC_SAFE_RECV_FLAGS(0, nb);
            for (int tries = 0; tries < 16; ++tries) {
                socklen_t fl = (socklen_t)sizeof(s->rx_from);
                memset(&s->rx_from, 0, sizeof(s->rx_from));
                ssize_t r = recvfrom(rfd, s->rx_buf, UDP_MAX_DGRAM, rx_flags,
                                     (struct sockaddr *)&s->rx_from, &fl);
                if (r >= 0) {
                    got = r;
                    break;
                }
                int e = (errno > 0) ? errno : -errno;
                if (e == 0 && r < -1)
                    e = -(int)r;
                if (e == ECONNRESET || e == ECONNREFUSED || e == EHOSTUNREACH || e == ENETUNREACH)
                    continue;
                got_err = e;
                break;
            }
            if (got >= 0) {
                size_t store_n = ((size_t)got <= UDP_MAX_DGRAM) ? (size_t)got : UDP_MAX_DGRAM;
                s->rx_len = (uint16_t)store_n;
                s->rx_valid = true;
                soc_unlock();
                *argp = (unsigned long)(store_n > 0u ? store_n : 1u);
                nox_3ds_net_set_last_error(0);
                return 0;
            }
            soc_unlock();
            if (got_err != EWOULDBLOCK && got_err != EAGAIN && got_err != 0) {
                static uint32_t s_last_rxerr_ms = 0;
                uint32_t now_ms = nox_3ds_get_ticks_ms();
                if (s_last_rxerr_ms == 0u || (uint32_t)(now_ms - s_last_rxerr_ms) >= 1000u) {
                    s_last_rxerr_ms = now_ms ? now_ms : 1u;
                    nox_3ds_net_log("[net3ds:soc] FIONREAD UDP recvfrom(vfd=%d, rfd=%d, flags=0x%x) err=%d",
                                    vfd, rfd, rx_flags, got_err);
                }
            }
            *argp = 0ul;
            nox_3ds_net_set_last_error(0);
            return 0;
        }

        /* SOCK_STREAM: probe readability with hardware-true soc:u CTRPollFD masks. */
        struct pollfd pfd;
        pfd.fd = rfd;
        pfd.events = CTR_SOC_POLLRDNORM | CTR_SOC_POLLRDBAND;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, 0);
        if (pr < 0) {
            nox_3ds_net_set_last_error(wsa_from_errno(errno));
            *argp = 0ul;
            return -1;
        }
        if (pr == 0 || (pfd.revents & (CTR_SOC_POLLERR | CTR_SOC_POLLNVAL)) ||
            !(pfd.revents & (CTR_SOC_POLLRDNORM | CTR_SOC_POLLRDBAND))) {
            *argp = 0ul;
            nox_3ds_net_set_last_error(0);
            return 0;
        }

        char peek[1024];
        ssize_t n = recv(rfd, peek, sizeof(peek), SOC_SAFE_RECV_FLAGS(MSG_PEEK, nb));
        if (n > 0) {
            *argp = (unsigned long)n;
        } else if (n < 0 && errno == EMSGSIZE) {
            *argp = (unsigned long)sizeof(peek);
        } else {
            *argp = 0ul;
        }
        nox_3ds_net_set_last_error(0);
        return 0;
    }

    nox_3ds_net_set_last_error(NOX_WSAEINVAL);
    errno = EINVAL;
    return -1;
}

static int sockets_sendto(int vfd, const void *buf, unsigned int len, int flags,
                          const void *to, unsigned int tolen)
{
    if (!buf || !to || tolen < sizeof(struct sockaddr_in)) {
        nox_3ds_net_set_last_error(NOX_WSAEINVAL);
        errno = EINVAL;
        return -1;
    }
    if (len > UDP_MAX_DGRAM) {
        nox_3ds_net_set_last_error(NOX_WSAEMSGSIZE);
        errno = EMSGSIZE;
        return -1;
    }
    if (__atomic_load_n(&g_soc_suspended, __ATOMIC_ACQUIRE)) {
        nox_3ds_net_set_last_error(NOX_WSAENETRESET);
        errno = ENETRESET;
        return -1;
    }

    struct sockaddr_in dst_sin;
    memcpy(&dst_sin, to, sizeof(dst_sin));
    dst_sin.sin_family = AF_INET;
    memset(dst_sin.sin_zero, 0, sizeof(dst_sin.sin_zero));

    /* Lock-free Core 0 SPSC TX fast path for pumped New 3DS UDP sockets (~1 us, 0 soc:u IPCs). */
    int fast_idx = -1;
    uint16_t fast_gen = 0u;
    if (decode_vfd(vfd, &fast_idx, &fast_gen)) {
        soc_slot_t *fs = &g_soc_slots[fast_idx];
        uint8_t fst = __atomic_load_n(&fs->state, __ATOMIC_ACQUIRE);

        if (fst == SOC_SLOT_SETUP && fs->gen == fast_gen) {
            /* Auto-bind ephemeral UDP port if caller sends before bind(), then promote. */
            int ephem = bind_ephemeral_udp_port(fs->real_fd);
            soc_lock();
            if (lookup_active_slot(vfd, &fst) == fs && fst == SOC_SLOT_SETUP) {
                if (ephem > 0)
                    fs->bound_port = (uint16_t)ephem;
                (void)promote_udp_slot_to_pumped_locked(fs);
                fst = __atomic_load_n(&fs->state, __ATOMIC_ACQUIRE);
            }
            soc_unlock();
        }

        if (fst == SOC_SLOT_PUMPED && fs->gen == fast_gen) {
            udp_spsc_rings_t *rings = fs->rings;
            uint32_t w = rings->tx_write_idx;
            uint32_t r = __atomic_load_n(&rings->tx_read_idx, __ATOMIC_ACQUIRE);

            if ((uint32_t)(w - r) >= UDP_TX_RING_CAP) {
                /* Extreme >64-datagram burst: wake Core 2 and wait up to 1 ms for space
                 * without ever calling soc:u on Core 0 or corrupting ring state. */
                LightEvent_Signal(&g_udp_wake_event);
                for (int spin = 0; spin < 20; ++spin) {
                    svcSleepThread(50000LL); /* 50 us */
                    r = __atomic_load_n(&rings->tx_read_idx, __ATOMIC_ACQUIRE);
                    if ((uint32_t)(w - r) < UDP_TX_RING_CAP)
                        break;
                }
                if ((uint32_t)(w - r) >= UDP_TX_RING_CAP) {
                    nox_3ds_net_set_last_error(NOX_WSAEWOULDBLOCK);
                    errno = EWOULDBLOCK;
                    return -1;
                }
            }

            udp_tx_pkt_t *pkt = &rings->tx_pkts[w & UDP_TX_RING_MASK];
            pkt->dst_ip   = (uint32_t)dst_sin.sin_addr.s_addr;
            pkt->dst_port = (uint16_t)dst_sin.sin_port;
            pkt->len      = (uint16_t)len;
            pkt->flags    = SOC_SAFE_SEND_FLAGS(flags);
            if (len > 0u)
                memcpy(pkt->payload, buf, len);

            __atomic_store_n(&rings->tx_write_idx, w + 1u, __ATOMIC_RELEASE);
            LightEvent_Signal(&g_udp_wake_event);
            nox_3ds_net_set_last_error(0);
            return (int)len;
        }
    }

    /* Old 3DS direct UDP fallback / SOCK_STREAM path. */
    soc_lock();
    uint8_t st = SOC_SLOT_FREE;
    soc_slot_t *s = lookup_active_slot(vfd, &st);
    if (!s || st != SOC_SLOT_DIRECT) {
        nox_3ds_net_set_last_error(NOX_WSAENOTSOCK);
        errno = EBADF;
        soc_unlock();
        return -1;
    }
    int rfd = s->real_fd;
    int stype = s->type;
    soc_unlock();

    if (stype == SOCK_DGRAM && dst_sin.sin_addr.s_addr == htonl(INADDR_BROADCAST)) {
        uint32_t ip_be = 0, mask_be = 0, bcast_be = 0;
        ssize_t r_dir = -1;
        int err_dir = 0;
        if (nox_3ds_soc_get_ip_info(&ip_be, &mask_be, &bcast_be) &&
            bcast_be != 0u && bcast_be != htonl(INADDR_BROADCAST)) {
            struct sockaddr_in dir_sin = dst_sin;
            dir_sin.sin_addr.s_addr = bcast_be;
            r_dir = sendto(rfd, buf, (size_t)len, SOC_SAFE_SEND_FLAGS(flags),
                           (const struct sockaddr *)&dir_sin, (socklen_t)sizeof(dir_sin));
            err_dir = errno;
            nox_3ds_net_log("[net3ds:soc] Directed LAN bcast -> %u.%u.%u.%u:%u rc=%d errno=%d",
                            (unsigned)(bcast_be & 0xFFu), (unsigned)((bcast_be >> 8) & 0xFFu),
                            (unsigned)((bcast_be >> 16) & 0xFFu), (unsigned)((bcast_be >> 24) & 0xFFu),
                            (unsigned)ntohs(dst_sin.sin_port), (int)r_dir,
                            (r_dir < 0) ? err_dir : 0);
        }

        ssize_t r_lim = sendto(rfd, buf, (size_t)len, SOC_SAFE_SEND_FLAGS(flags),
                               (const struct sockaddr *)&dst_sin, (socklen_t)sizeof(dst_sin));
        if (r_dir >= 0 || r_lim >= 0) {
            nox_3ds_net_set_last_error(0);
            return (int)((r_dir >= 0) ? r_dir : r_lim);
        }
        nox_3ds_net_set_last_error(wsa_from_errno(errno ? errno : err_dir));
        return -1;
    }

    int tx_flags = SOC_SAFE_SEND_FLAGS(flags);
    ssize_t r = sendto(rfd, buf, (size_t)len, tx_flags,
                       (const struct sockaddr *)&dst_sin, (socklen_t)sizeof(dst_sin));
    if (r < 0) {
        int e = (errno > 0) ? errno : -errno;
        unsigned int wsa = wsa_from_errno(e);
        nox_3ds_net_set_last_error(wsa);
        static uint32_t s_last_txerr_ms = 0;
        uint32_t now_ms = nox_3ds_get_ticks_ms();
        if (s_last_txerr_ms == 0u || (uint32_t)(now_ms - s_last_txerr_ms) >= 1000u) {
            s_last_txerr_ms = now_ms ? now_ms : 1u;
            uint32_t dip = (uint32_t)dst_sin.sin_addr.s_addr;
            nox_3ds_net_log("[net3ds:soc] sendto(vfd=%d, rfd=%d, len=%u, flags=0x%x) -> %u.%u.%u.%u:%u FAILED errno=%d wsa=%u",
                            vfd, rfd, len, tx_flags,
                            (unsigned)(dip & 0xFFu), (unsigned)((dip >> 8) & 0xFFu),
                            (unsigned)((dip >> 16) & 0xFFu), (unsigned)((dip >> 24) & 0xFFu),
                            (unsigned)ntohs(dst_sin.sin_port), e, wsa);
        }
        return -1;
    }

    nox_3ds_net_set_last_error(0);
    return (int)r;
}

static int sockets_recvfrom(int vfd, void *buf, unsigned int len, int flags,
                            void *from, unsigned int *fromlen)
{
    if (!buf) {
        nox_3ds_net_set_last_error(NOX_WSAEINVAL);
        errno = EINVAL;
        return -1;
    }
    if (__atomic_load_n(&g_soc_suspended, __ATOMIC_ACQUIRE)) {
        nox_3ds_net_set_last_error(NOX_WSAEWOULDBLOCK);
        errno = EWOULDBLOCK;
        return -1;
    }

    /* Lock-free Core 0 SPSC RX fast path for pumped New 3DS UDP sockets (0 syscalls). */
    int fast_idx = -1;
    uint16_t fast_gen = 0u;
    if (decode_vfd(vfd, &fast_idx, &fast_gen)) {
        soc_slot_t *fs = &g_soc_slots[fast_idx];
        uint8_t fst = __atomic_load_n(&fs->state, __ATOMIC_ACQUIRE);
        if (fst == SOC_SLOT_PUMPED && fs->gen == fast_gen) {
            udp_spsc_rings_t *rings = fs->rings;
            uint32_t r = rings->rx_read_idx;
            uint32_t w = __atomic_load_n(&rings->rx_write_idx, __ATOMIC_ACQUIRE);
            if (r == w) {
                nox_3ds_net_set_last_error(NOX_WSAEWOULDBLOCK);
                errno = EWOULDBLOCK;
                return -1;
            }

            const udp_rx_pkt_t *pkt = &rings->rx_pkts[r & UDP_RX_RING_MASK];
            size_t copy_n = (len < (size_t)pkt->len) ? (size_t)len : (size_t)pkt->len;
            if (copy_n > 0u)
                memcpy(buf, pkt->payload, copy_n);

            if (from && fromlen && *fromlen >= sizeof(struct sockaddr_in)) {
                struct sockaddr_in src_sin;
                memset(&src_sin, 0, sizeof(src_sin));
                src_sin.sin_family = AF_INET;
                src_sin.sin_port = pkt->src_port;
                src_sin.sin_addr.s_addr = pkt->src_ip;
                memcpy(from, &src_sin, sizeof(src_sin));
                *fromlen = (unsigned int)sizeof(struct sockaddr_in);
            }

            if (!(flags & MSG_PEEK)) {
                __atomic_store_n(&rings->rx_read_idx, r + 1u, __ATOMIC_RELEASE);
                record_rx_packet_core0(fs);
            }

            nox_3ds_net_set_last_error(0);
            return (int)copy_n;
        }
        if (fst == SOC_SLOT_SETUP && fs->gen == fast_gen) {
            nox_3ds_net_set_last_error(NOX_WSAEWOULDBLOCK);
            errno = EWOULDBLOCK;
            return -1;
        }
    }

    /* Old 3DS direct UDP fallback / SOCK_STREAM path. */
    soc_lock();
    uint8_t st = SOC_SLOT_FREE;
    soc_slot_t *s = lookup_active_slot(vfd, &st);
    if (!s || st != SOC_SLOT_DIRECT) {
        nox_3ds_net_set_last_error(NOX_WSAENOTSOCK);
        errno = EBADF;
        soc_unlock();
        return -1;
    }
    int rfd = s->real_fd;
    int stype = s->type;
    bool nb = s->nonblocking;
    if (stype == SOCK_DGRAM && s->rx_valid && s->rx_buf) {
        size_t copy_n = (len < (size_t)s->rx_len) ? len : (size_t)s->rx_len;
        if (copy_n > 0u)
            memcpy(buf, s->rx_buf, copy_n);
        if (from && fromlen && *fromlen >= sizeof(struct sockaddr_in)) {
            struct sockaddr_in src_copy = s->rx_from;
            src_copy.sin_family = AF_INET;
            memset(src_copy.sin_zero, 0, sizeof(src_copy.sin_zero));
            memcpy(from, &src_copy, sizeof(src_copy));
            *fromlen = (unsigned int)sizeof(struct sockaddr_in);
        }
        if (!(flags & MSG_PEEK)) {
            s->rx_valid = false;
            s->rx_len = 0u;
            record_rx_packet_core0(s);
        }
        soc_unlock();
        nox_3ds_net_set_last_error(0);
        return (int)copy_n;
    }
    soc_unlock();

    struct sockaddr_in src_sin;
    socklen_t src_len = (socklen_t)sizeof(src_sin);
    ssize_t r = -1;
    int max_tries = (stype == SOCK_DGRAM) ? 16 : 1;
    int rx_flags = SOC_SAFE_RECV_FLAGS(flags, nb);
    for (int tries = 0; tries < max_tries; ++tries) {
        src_len = (socklen_t)sizeof(src_sin);
        memset(&src_sin, 0, sizeof(src_sin));
        r = recvfrom(rfd, buf, (size_t)len, rx_flags,
                     from ? (struct sockaddr *)&src_sin : NULL,
                     from ? &src_len : NULL);
        if (r >= 0)
            break;
        int e = (errno > 0) ? errno : -errno;
        if (e == 0 && r < -1)
            e = -(int)r;
        if (stype == SOCK_DGRAM &&
            (e == ECONNRESET || e == ECONNREFUSED || e == EHOSTUNREACH || e == ENETUNREACH)) {
            errno = EWOULDBLOCK;
            continue;
        }
        break;
    }
    if (r < 0) {
        int e = (errno > 0) ? errno : -errno;
        if (stype == SOCK_DGRAM &&
            (e == ECONNRESET || e == ECONNREFUSED || e == EHOSTUNREACH || e == ENETUNREACH)) {
            nox_3ds_net_set_last_error(NOX_WSAEWOULDBLOCK);
            errno = EWOULDBLOCK;
            return -1;
        }
        nox_3ds_net_set_last_error(wsa_from_errno(errno));
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
    s = lookup_active_slot(vfd, NULL);
    if (s)
        record_rx_packet_core0(s);
    soc_unlock();

    nox_3ds_net_set_last_error(0);
    return (int)r;
}

static int sockets_listen(int vfd, int backlog)
{
    soc_lock();
    uint8_t st = SOC_SLOT_FREE;
    soc_slot_t *s = lookup_active_slot(vfd, &st);
    if (!s || st != SOC_SLOT_DIRECT) {
        nox_3ds_net_set_last_error(NOX_WSAENOTSOCK);
        errno = EBADF;
        soc_unlock();
        return -1;
    }
    int rfd = s->real_fd;
    soc_unlock();

    int r = listen(rfd, backlog);
    if (r < 0) {
        nox_3ds_net_set_last_error(wsa_from_errno(errno));
        return -1;
    }
    nox_3ds_net_set_last_error(0);
    return 0;
}

static int sockets_accept(int vfd, void *addr, unsigned int *addrlen)
{
    if (__atomic_load_n(&g_soc_suspended, __ATOMIC_ACQUIRE)) {
        nox_3ds_net_set_last_error(NOX_WSAEWOULDBLOCK);
        errno = EWOULDBLOCK;
        return -1;
    }

    soc_lock();
    uint8_t st = SOC_SLOT_FREE;
    soc_slot_t *s = lookup_active_slot(vfd, &st);
    if (!s || st != SOC_SLOT_DIRECT) {
        nox_3ds_net_set_last_error(NOX_WSAENOTSOCK);
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
        nox_3ds_net_set_last_error(wsa_from_errno(errno));
        return -1;
    }

    int fl = fcntl(cfd, F_GETFL, 0);
    if (fl >= 0)
        (void)fcntl(cfd, F_SETFL, fl | O_NONBLOCK);

    soc_lock();
    int slot_idx = -1;
    for (int i = 0; i < MAX_SOC_SOCKS; ++i) {
        if (__atomic_load_n(&g_soc_slots[i].state, __ATOMIC_ACQUIRE) == SOC_SLOT_FREE) {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx < 0) {
        soc_unlock();
        close(cfd);
        nox_3ds_net_set_last_error(NOX_WSAEMFILE);
        errno = EMFILE;
        return -1;
    }
    soc_slot_t *cs = &g_soc_slots[slot_idx];
    uint16_t gen = next_slot_gen(cs->gen);
    memset(cs, 0, sizeof(*cs));
    cs->gen = gen;
    cs->real_fd = cfd;
    cs->type = SOCK_STREAM;
    cs->nonblocking = true;
    __atomic_store_n(&cs->state, SOC_SLOT_DIRECT, __ATOMIC_RELEASE);
    int child_vfd = make_vfd(slot_idx, gen);
    soc_unlock();

    if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        peer_sin.sin_family = AF_INET;
        memset(peer_sin.sin_zero, 0, sizeof(peer_sin.sin_zero));
        memcpy(addr, &peer_sin, sizeof(peer_sin));
        *addrlen = (unsigned int)sizeof(struct sockaddr_in);
    }

    nox_3ds_net_set_last_error(0);
    return child_vfd;
}

static int sockets_shutdown_sock(int vfd, int how)
{
    soc_lock();
    uint8_t st = SOC_SLOT_FREE;
    soc_slot_t *s = lookup_active_slot(vfd, &st);
    if (!s) {
        nox_3ds_net_set_last_error(NOX_WSAENOTSOCK);
        errno = EBADF;
        soc_unlock();
        return -1;
    }
    if (st == SOC_SLOT_PUMPED || st == SOC_SLOT_SETUP) {
        soc_unlock();
        nox_3ds_net_set_last_error(0);
        return 0;
    }
    int rfd = s->real_fd;
    soc_unlock();

    int r = shutdown(rfd, how);
    if (r < 0) {
        nox_3ds_net_set_last_error(wsa_from_errno(errno));
        return -1;
    }
    nox_3ds_net_set_last_error(0);
    return 0;
}

static int sockets_send(int vfd, const void *buf, unsigned int len, int flags)
{
    if (!buf) {
        nox_3ds_net_set_last_error(NOX_WSAEINVAL);
        errno = EINVAL;
        return -1;
    }
    if (__atomic_load_n(&g_soc_suspended, __ATOMIC_ACQUIRE)) {
        nox_3ds_net_set_last_error(NOX_WSAENETRESET);
        errno = ENETRESET;
        return -1;
    }

    soc_lock();
    uint8_t st = SOC_SLOT_FREE;
    soc_slot_t *s = lookup_active_slot(vfd, &st);
    if (!s || st != SOC_SLOT_DIRECT) {
        nox_3ds_net_set_last_error(NOX_WSAENOTSOCK);
        errno = EBADF;
        soc_unlock();
        return -1;
    }
    int rfd = s->real_fd;
    soc_unlock();

    ssize_t r = send(rfd, buf, (size_t)len, SOC_SAFE_SEND_FLAGS(flags));
    if (r < 0) {
        nox_3ds_net_set_last_error(wsa_from_errno(errno));
        return -1;
    }
    nox_3ds_net_set_last_error(0);
    return (int)r;
}

static int sockets_recv(int vfd, void *buf, unsigned int len, int flags)
{
    if (!buf) {
        nox_3ds_net_set_last_error(NOX_WSAEINVAL);
        errno = EINVAL;
        return -1;
    }
    if (__atomic_load_n(&g_soc_suspended, __ATOMIC_ACQUIRE)) {
        nox_3ds_net_set_last_error(NOX_WSAEWOULDBLOCK);
        errno = EWOULDBLOCK;
        return -1;
    }

    int fast_idx = -1;
    uint16_t fast_gen = 0u;
    if (decode_vfd(vfd, &fast_idx, &fast_gen)) {
        soc_slot_t *fs = &g_soc_slots[fast_idx];
        uint8_t fst = __atomic_load_n(&fs->state, __ATOMIC_ACQUIRE);
        if ((fst == SOC_SLOT_PUMPED || fst == SOC_SLOT_SETUP) && fs->gen == fast_gen) {
            return sockets_recvfrom(vfd, buf, len, flags, NULL, NULL);
        }
    }

    soc_lock();
    uint8_t st = SOC_SLOT_FREE;
    soc_slot_t *s = lookup_active_slot(vfd, &st);
    if (!s || st != SOC_SLOT_DIRECT) {
        nox_3ds_net_set_last_error(NOX_WSAENOTSOCK);
        errno = EBADF;
        soc_unlock();
        return -1;
    }
    int rfd = s->real_fd;
    bool nb = s->nonblocking;
    if (s->type == SOCK_DGRAM && s->rx_valid && s->rx_buf) {
        size_t copy_n = (len < (size_t)s->rx_len) ? len : (size_t)s->rx_len;
        if (copy_n > 0u)
            memcpy(buf, s->rx_buf, copy_n);
        if (!(flags & MSG_PEEK)) {
            s->rx_valid = false;
            s->rx_len = 0u;
        }
        soc_unlock();
        nox_3ds_net_set_last_error(0);
        return (int)copy_n;
    }
    soc_unlock();

    ssize_t r = recv(rfd, buf, (size_t)len, SOC_SAFE_RECV_FLAGS(flags, nb));
    if (r < 0) {
        nox_3ds_net_set_last_error(wsa_from_errno(errno));
        return -1;
    }
    nox_3ds_net_set_last_error(0);
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
        uint32_t local_ip = 0u;
        if (!nox_3ds_soc_get_ip_info(&local_ip, NULL, NULL) || local_ip == 0u)
            local_ip = (uint32_t)gethostid();
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
