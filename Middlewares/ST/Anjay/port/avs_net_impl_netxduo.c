/**
 ******************************************************************************
 * @file    avs_net_impl_netxduo.c
 * @brief   avs_commons socket implementation using native NetX Duo UDP API
 ******************************************************************************
 * @note
 *   Provides _avs_net_create_udp_socket() for Anjay via native NetX Duo
 *   nx_udp_socket_* API. No BSD socket layer required.
 *
 *   Based on the SIMCom reference implementation (xcc_com_sockets pattern):
 *   - sockfd_t is a pointer type (defined in netxduo-posix-compat.h)
 *   - get_system_socket() returns &sock->self (non-NULL, for event loop)
 *   - netxduo_poll() calls nx_udp_socket_receive() and buffers data
 *   - receive() reads from poll's buffer first, then from socket
 *
 *   Uses: nx_udp_socket_create/bind/send/receive/delete
 *         nx_dns_host_by_name_get for DNS resolution (native NetX)
 ******************************************************************************
 */

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <avsystem/commons/avs_socket_v_table.h>
#include <avsystem/commons/avs_net.h>
#include <avsystem/commons/avs_memory.h>
#include <avsystem/commons/avs_errno.h>
#include <avsystem/commons/avs_time.h>
#include <avsystem/commons/avs_utils.h>

#include <avs_commons_init.h>
#include <net/avs_net_impl.h>

/* netxduo-posix-compat.h is included via AVS_COMMONS_POSIX_COMPAT_HEADER
 * in avs_commons_config.h. It defines sockfd_t, struct pollfd, poll(), etc.
 * We include it explicitly here for the struct netxduo_sock_impl forward decl. */
#include "netxduo-posix-compat.h"

#include "nx_api.h"
#include "nxd_dns.h"

#include "elog.h"

/* Access global NetX objects from app_netxduo.c */
extern NX_DNS          dns_client;
extern volatile UINT   dns_client_initialized;
extern NX_PACKET_POOL  pool_0;
extern NX_IP          *app_netxduo_get_active_ip(void);

/* ============================================================================
 * Socket internal structure — native NetX Duo UDP
 *
 * Defined as both avs_net_socket_struct and netxduo_sock_impl so that:
 * - avs_commons sees it as avs_net_socket_t (via avs_net_socket_struct)
 * - netxduo-posix-compat.h's sockfd_t (= struct netxduo_sock_impl *) works
 * ============================================================================ */

struct avs_net_socket_struct {
    const avs_net_socket_v_table_t *operations;
    struct avs_net_socket_struct   *self;   /* points to self for get_system_socket() */

    NX_UDP_SOCKET       nx_udp_sock;
    int                 socket_created;
    int                 socket_bound;

    ULONG               remote_ip;           /* host byte order */
    UINT                remote_port;         /* host byte order */
    char                remote_host[16];     /* "255.255.255.255" */
    char                remote_port_str[6];  /* "65535" */

    avs_time_duration_t recv_timeout;        /* avs_time format */

    /* Poll buffer — data received by netxduo_poll() is stored here.
     * netxduo_receive() returns this data before calling nx_udp_socket_receive. */
    avs_error_t         poll_captured_recv_error;
    size_t              buffered_by_poll_len;
    uint8_t             buffered_by_poll[NETXDUO_POLL_BUF_SIZE];
};

/* Make netxduo_sock_impl an alias so sockfd_t (= struct netxduo_sock_impl *)
 * is compatible with avs_net_socket_t (= struct avs_net_socket_struct *).
 * Both structs are identical, just with two names. */
struct netxduo_sock_impl {
    const avs_net_socket_v_table_t *operations;
    struct avs_net_socket_struct   *self;
    NX_UDP_SOCKET       nx_udp_sock;
    int                 socket_created;
    int                 socket_bound;
    ULONG               remote_ip;
    UINT                remote_port;
    char                remote_host[16];
    char                remote_port_str[6];
    avs_time_duration_t recv_timeout;
    avs_error_t         poll_captured_recv_error;
    size_t              buffered_by_poll_len;
    uint8_t             buffered_by_poll[NETXDUO_POLL_BUF_SIZE];
};

/* ============================================================================
 * Forward declarations
 * ============================================================================ */

static const avs_net_socket_v_table_t netxduo_udp_vtable;

/* ============================================================================
 * errno mapping: NetX status → avs_error_t
 * ============================================================================ */

static avs_error_t avs_error_from_nx_status(UINT status) {
    switch (status) {
    case NX_SUCCESS:               return AVS_OK;
    case NX_NO_PACKET:             return avs_errno(AVS_ETIMEDOUT);
    case NX_NOT_CONNECTED:         return avs_errno(AVS_ENOTCONN);
    case NX_INVALID_PARAMETERS:    return avs_errno(AVS_EINVAL);
    case NX_INVALID_SOCKET:        return avs_errno(AVS_EINVAL);
    case NX_SOCKET_UNBOUND:        return avs_errno(AVS_ENOTCONN);
    case NX_NOT_ENABLED:           return avs_errno(AVS_ENODEV);
    case NX_ALREADY_BOUND:         return avs_errno(AVS_EADDRINUSE);
    default:                       return avs_errno(AVS_UNKNOWN_ERROR);
    }
}

/* ============================================================================
 * Helper: resolve hostname via NX_DNS (native NetX API)
 * ============================================================================ */

static avs_error_t resolve_host_port(const char *host, const char *port,
                                      ULONG *out_ip, UINT *out_port) {
    if (!host || !port) {
        return avs_errno(AVS_EINVAL);
    }

    *out_port = (UINT) atoi(port);

    /* Try as dotted-decimal IP first */
    UINT a, b, c, d;
    if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
        a <= 255 && b <= 255 && c <= 255 && d <= 255) {
        *out_ip = IP_ADDRESS(a, b, c, d);
        return AVS_OK;
    }

    /* DNS resolution via native NX_DNS */
    if (!dns_client_initialized) {
        elog_e("NETX", "DNS client not initialized");
        return avs_errno(AVS_EADDRNOTAVAIL);
    }

    ULONG resolved;
    UINT status = nx_dns_host_by_name_get(&dns_client, (UCHAR *)host,
                                           &resolved, 5 * NX_IP_PERIODIC_RATE);
    if (status != NX_SUCCESS) {
        elog_e("NETX", "DNS resolve(%s) failed: 0x%02X", host, status);
        return avs_errno(AVS_EADDRNOTAVAIL);
    }

    *out_ip = resolved;
    return AVS_OK;
}

/* ============================================================================
 * Helper: ensure native NetX UDP socket is created and bound
 * ============================================================================ */

static avs_error_t ensure_socket_ready(avs_net_socket_t *sock) {
    if (sock->socket_created) {
        return AVS_OK;
    }

    NX_IP *ip = app_netxduo_get_active_ip();
    if (!ip) {
        elog_e("NETX", "No active IP instance");
        return avs_errno(AVS_ENODEV);
    }

    /* Create UDP socket with max queue depth of 5 packets */
    UINT status = nx_udp_socket_create(ip, &sock->nx_udp_sock,
                                        "Anjay UDP",
                                        NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                        128, 5);
    if (status != NX_SUCCESS) {
        elog_e("NETX", "nx_udp_socket_create() failed: 0x%02X", status);
        return avs_error_from_nx_status(status);
    }
    sock->socket_created = 1;

    /* Bind to any ephemeral port */
    status = nx_udp_socket_bind(&sock->nx_udp_sock, NX_ANY_PORT,
                                 NX_NO_WAIT);
    if (status != NX_SUCCESS) {
        elog_e("NETX", "nx_udp_socket_bind() failed: 0x%02X", status);
        nx_udp_socket_delete(&sock->nx_udp_sock);
        sock->socket_created = 0;
        return avs_error_from_nx_status(status);
    }
    sock->socket_bound = 1;

    UINT local_port;
    nx_udp_socket_port_get(&sock->nx_udp_sock, &local_port);
    elog_i("NETX", "UDP socket created, bound to port %u", local_port);

    return AVS_OK;
}

/* ============================================================================
 * Socket create
 * ============================================================================ */

avs_error_t _avs_net_create_udp_socket(avs_net_socket_t **socket,
                                        const void *socket_configuration) {
    (void) socket_configuration;

    if (!socket) {
        return avs_errno(AVS_EINVAL);
    }

    if (*socket) {
        avs_net_socket_cleanup(socket);
    }

    avs_net_socket_t *sock = avs_calloc(1, sizeof(avs_net_socket_t));
    if (!sock) {
        return avs_errno(AVS_ENOMEM);
    }

    sock->operations        = &netxduo_udp_vtable;
    sock->self              = sock;    /* self-reference for get_system_socket() */
    sock->socket_created    = 0;
    sock->socket_bound      = 0;
    sock->recv_timeout      = avs_time_duration_from_scalar(30, AVS_TIME_S);

    *socket = sock;
    return AVS_OK;
}

avs_error_t _avs_net_create_tcp_socket(avs_net_socket_t **socket,
                                        const void *socket_configuration) {
    (void) socket;
    (void) socket_configuration;
    elog_w("NETX", "TCP socket not implemented yet");
    return avs_errno(AVS_ENOTSUP);
}

/* ============================================================================
 * connect — resolve remote and store address for send
 * ============================================================================ */

static avs_error_t netxduo_connect(avs_net_socket_t *socket,
                                    const char *host,
                                    const char *port) {
    if (!socket) {
        return avs_errno(AVS_EINVAL);
    }

    ULONG ip;
    UINT  port_num;
    avs_error_t err = resolve_host_port(host, port, &ip, &port_num);
    if (avs_is_err(err)) {
        return err;
    }

    err = ensure_socket_ready(socket);
    if (avs_is_err(err)) {
        return err;
    }

    socket->remote_ip   = ip;
    socket->remote_port = port_num;
    strncpy(socket->remote_host, host, sizeof(socket->remote_host) - 1);
    strncpy(socket->remote_port_str, port, sizeof(socket->remote_port_str) - 1);

    elog_i("NETX", "UDP connected to %s:%s = %lu.%lu.%lu.%lu:%u",
           host, port,
           (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
           (ip >> 8) & 0xFF, ip & 0xFF, port_num);
    return AVS_OK;
}

/* ============================================================================
 * send — allocate packet, copy data, send via nx_udp_socket_send
 * ============================================================================ */

static avs_error_t netxduo_send(avs_net_socket_t *socket,
                                 const void *buffer,
                                 size_t buffer_length) {
    if (!socket || !socket->socket_created) {
        return avs_errno(AVS_EBADF);
    }

    NX_PACKET *pkt = NULL;
    UINT status = nx_packet_allocate(&pool_0, &pkt,
                                      NX_UDP_PACKET, NX_NO_WAIT);
    if (status != NX_SUCCESS) {
        elog_e("NETX", "nx_packet_allocate() failed: 0x%02X", status);
        return avs_error_from_nx_status(status);
    }

    status = nx_packet_data_append(pkt, (void *)buffer, (ULONG)buffer_length,
                                    &pool_0, NX_NO_WAIT);
    if (status != NX_SUCCESS) {
        elog_e("NETX", "nx_packet_data_append() failed: 0x%02X", status);
        nx_packet_release(pkt);
        return avs_error_from_nx_status(status);
    }

    /* nx_udp_socket_send() takes IP address in host byte order */
    status = nx_udp_socket_send(&socket->nx_udp_sock, pkt,
                                 socket->remote_ip,
                                 (UINT)socket->remote_port);
    if (status != NX_SUCCESS) {
        elog_e("NETX", "nx_udp_socket_send() failed: 0x%02X", status);
        nx_packet_release(pkt);
        return avs_error_from_nx_status(status);
    }

    return AVS_OK;
}

/* ============================================================================
 * sendto — send to specified destination via nx_udp_socket_send
 * ============================================================================ */

static avs_error_t netxduo_send_to(avs_net_socket_t *sock,
                                    const void *buffer,
                                    size_t buffer_length,
                                    const char *host,
                                    const char *port) {
    if (!sock) {
        return avs_errno(AVS_EINVAL);
    }

    ULONG ip;
    UINT  port_num;
    avs_error_t err = resolve_host_port(host, port, &ip, &port_num);
    if (avs_is_err(err)) {
        return err;
    }

    err = ensure_socket_ready(sock);
    if (avs_is_err(err)) {
        return err;
    }

    NX_PACKET *pkt = NULL;
    UINT status = nx_packet_allocate(&pool_0, &pkt,
                                      NX_UDP_PACKET, NX_NO_WAIT);
    if (status != NX_SUCCESS) {
        return avs_error_from_nx_status(status);
    }

    status = nx_packet_data_append(pkt, (void *)buffer, (ULONG)buffer_length,
                                    &pool_0, NX_NO_WAIT);
    if (status != NX_SUCCESS) {
        nx_packet_release(pkt);
        return avs_error_from_nx_status(status);
    }

    /* nx_udp_socket_send() takes IP in host byte order */
    status = nx_udp_socket_send(&sock->nx_udp_sock, pkt, ip, (UINT)port_num);
    if (status != NX_SUCCESS) {
        nx_packet_release(pkt);
        return avs_error_from_nx_status(status);
    }

    return AVS_OK;
}

/* ============================================================================
 * receive — return buffered data from poll, or call nx_udp_socket_receive
 *
 * Following the reference pattern: poll() does the actual receiving and
 * buffers data. receive() returns that buffered data first, then falls
 * through to nx_udp_socket_receive() if no buffered data.
 * ============================================================================ */

static avs_error_t netxduo_receive(avs_net_socket_t *socket,
                                    size_t *out_bytes_received,
                                    void *buffer,
                                    size_t buffer_length) {
    if (!socket || !socket->socket_created) {
        return avs_errno(AVS_EBADF);
    }

    *out_bytes_received = 0;

    /* Return captured error from poll() if any */
    if (avs_is_err(socket->poll_captured_recv_error)) {
        avs_error_t err = socket->poll_captured_recv_error;
        socket->poll_captured_recv_error = AVS_OK;
        return err;
    }

    if (buffer_length == 0) {
        return AVS_OK;
    }

    /* Return data buffered by poll() if available */
    if (socket->buffered_by_poll_len > 0) {
        size_t to_copy = socket->buffered_by_poll_len;
        if (to_copy > buffer_length) {
            to_copy = buffer_length;
        }
        memcpy(buffer, socket->buffered_by_poll, to_copy);
        socket->buffered_by_poll_len = 0;
        *out_bytes_received = to_copy;
        return AVS_OK;
    }

    /* No buffered data — call nx_udp_socket_receive() with stored timeout */
    int64_t timeout_ms = 0;
    if (avs_time_duration_to_scalar(&timeout_ms, AVS_TIME_MS,
                                     socket->recv_timeout)) {
        timeout_ms = -1; /* treat invalid as infinite */
    } else if (timeout_ms < 0) {
        timeout_ms = 0;
    }

    ULONG timeout_ticks;
    if (timeout_ms < 0) {
        timeout_ticks = NX_WAIT_FOREVER;
    } else {
        timeout_ticks = (ULONG)(timeout_ms * NX_IP_PERIODIC_RATE / 1000);
    }

    NX_PACKET *pkt = NULL;
    UINT status = nx_udp_socket_receive(&socket->nx_udp_sock, &pkt,
                                         timeout_ticks);

    if (status != NX_SUCCESS) {
        if (status == NX_NO_PACKET) {
            return avs_errno(AVS_ETIMEDOUT);
        }
        return avs_error_from_nx_status(status);
    }

    ULONG actual_len = 0;
    nx_packet_data_retrieve(pkt, buffer, &actual_len);
    nx_packet_release(pkt);

    if (actual_len > buffer_length) {
        actual_len = (ULONG)buffer_length;
    }

    *out_bytes_received = (size_t) actual_len;
    return AVS_OK;
}

/* ============================================================================
 * receivefrom — receive and extract source address
 * ============================================================================ */

static avs_error_t netxduo_receive_from(avs_net_socket_t *socket,
                                         size_t *out_bytes_received,
                                         void *buffer,
                                         size_t buffer_length,
                                         char *host,
                                         size_t host_size,
                                         char *port,
                                         size_t port_size) {
    /* For simplicity, delegate to receive() and skip source extraction.
     * Anjay's CoAP doesn't use receivefrom on the primary socket. */
    avs_error_t err = netxduo_receive(socket, out_bytes_received,
                                       buffer, buffer_length);
    if (avs_is_ok(err) && host && host_size > 0) {
        strncpy(host, socket->remote_host, host_size - 1);
        host[host_size - 1] = '\0';
        if (port && port_size > 0) {
            strncpy(port, socket->remote_port_str, port_size - 1);
            port[port_size - 1] = '\0';
        }
    }
    return err;
}

/* ============================================================================
 * bind — create and bind native NetX UDP socket to specific port
 * ============================================================================ */

static avs_error_t netxduo_bind(avs_net_socket_t *sock,
                                 const char *address,
                                 const char *port) {
    if (!sock) {
        return avs_errno(AVS_EINVAL);
    }

    if (sock->socket_bound) {
        nx_udp_socket_unbind(&sock->nx_udp_sock);
        sock->socket_bound = 0;
    }
    if (sock->socket_created) {
        nx_udp_socket_delete(&sock->nx_udp_sock);
        sock->socket_created = 0;
    }

    NX_IP *ip = app_netxduo_get_active_ip();
    if (!ip) {
        return avs_errno(AVS_ENODEV);
    }

    UINT status = nx_udp_socket_create(ip, &sock->nx_udp_sock,
                                        "Anjay UDP",
                                        NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                        128, 5);
    if (status != NX_SUCCESS) {
        return avs_error_from_nx_status(status);
    }
    sock->socket_created = 1;

    UINT bind_port = (port && *port) ? (UINT)atoi(port) : NX_ANY_PORT;
    status = nx_udp_socket_bind(&sock->nx_udp_sock, bind_port, NX_NO_WAIT);
    if (status != NX_SUCCESS) {
        nx_udp_socket_delete(&sock->nx_udp_sock);
        sock->socket_created = 0;
        return avs_error_from_nx_status(status);
    }
    sock->socket_bound = 1;

    return AVS_OK;
}

/* ============================================================================
 * close / cleanup
 * ============================================================================ */

static avs_error_t netxduo_close(avs_net_socket_t *socket) {
    if (!socket) {
        return avs_errno(AVS_EINVAL);
    }

    if (socket->socket_bound) {
        nx_udp_socket_unbind(&socket->nx_udp_sock);
        socket->socket_bound = 0;
    }
    if (socket->socket_created) {
        nx_udp_socket_delete(&socket->nx_udp_sock);
        socket->socket_created = 0;
    }
    return AVS_OK;
}

static avs_error_t netxduo_cleanup(avs_net_socket_t **socket) {
    if (!socket || !*socket) {
        return AVS_OK;
    }
    netxduo_close(*socket);
    avs_free(*socket);
    *socket = NULL;
    return AVS_OK;
}

/* ============================================================================
 * get_system_socket — returns &sock->self so event loop can use poll()
 *
 * The event loop does: fd = *(const sockfd_t *) avs_net_socket_get_system(s);
 * sockfd_t is struct netxduo_sock_impl*, so it dereferences &sock->self
 * to get sock, then passes sock (as sockfd_t) to poll().
 * ============================================================================ */

static const void *netxduo_get_system_socket(avs_net_socket_t *socket) {
    if (!socket || !socket->socket_created) {
        return NULL;
    }
    return &socket->self;
}

/* ============================================================================
 * get_local_port / get_remote_host / get_remote_port
 * ============================================================================ */

static avs_error_t netxduo_get_local_port(avs_net_socket_t *socket,
                                           char *out_buffer,
                                           size_t out_buffer_size) {
    if (!socket || !socket->socket_created) {
        return avs_errno(AVS_EBADF);
    }
    UINT port = 0;
    UINT status = nx_udp_socket_port_get(&socket->nx_udp_sock, &port);
    if (status != NX_SUCCESS) {
        return avs_error_from_nx_status(status);
    }
    snprintf(out_buffer, out_buffer_size, "%u", port);
    return AVS_OK;
}

static avs_error_t netxduo_get_remote_host(avs_net_socket_t *socket,
                                             char *out_buffer,
                                             size_t out_buffer_size) {
    if (!socket || !socket->remote_host[0]) {
        return avs_errno(AVS_EBADF);
    }
    snprintf(out_buffer, out_buffer_size, "%s", socket->remote_host);
    return AVS_OK;
}

static avs_error_t netxduo_get_remote_port(avs_net_socket_t *socket,
                                             char *out_buffer,
                                             size_t out_buffer_size) {
    if (!socket || !socket->remote_port_str[0]) {
        return avs_errno(AVS_EBADF);
    }
    snprintf(out_buffer, out_buffer_size, "%s", socket->remote_port_str);
    return AVS_OK;
}

/* ============================================================================
 * set_opt / get_opt
 * ============================================================================ */

static avs_error_t netxduo_set_opt(avs_net_socket_t *socket,
                                    avs_net_socket_opt_key_t option_key,
                                    avs_net_socket_opt_value_t option_value) {
    if (!socket) {
        return avs_errno(AVS_EINVAL);
    }
    switch (option_key) {
    case AVS_NET_SOCKET_OPT_RECV_TIMEOUT:
        socket->recv_timeout = option_value.recv_timeout;
        return AVS_OK;
    default:
        return avs_errno(AVS_ENOTSUP);
    }
}

static avs_error_t netxduo_get_opt(avs_net_socket_t *socket,
                                    avs_net_socket_opt_key_t option_key,
                                    avs_net_socket_opt_value_t *out_value) {
    if (!socket || !out_value) {
        return avs_errno(AVS_EINVAL);
    }
    switch (option_key) {
    case AVS_NET_SOCKET_OPT_STATE:
        out_value->state = socket->socket_created
                ? AVS_NET_SOCKET_STATE_CONNECTED
                : AVS_NET_SOCKET_STATE_CLOSED;
        return AVS_OK;
    case AVS_NET_SOCKET_OPT_ADDR_FAMILY:
        out_value->addr_family = AVS_NET_AF_INET4;
        return AVS_OK;
    case AVS_NET_SOCKET_OPT_RECV_TIMEOUT:
        out_value->recv_timeout = socket->recv_timeout;
        return AVS_OK;
    case AVS_NET_SOCKET_OPT_INNER_MTU:
        /* PPP MRU 1480 - IP header 20 - UDP header 8 = 1452 */
        out_value->mtu = 1452;
        return AVS_OK;
    case AVS_NET_SOCKET_OPT_BYTES_SENT:
        out_value->bytes_sent = 0;  /* not tracked */
        return AVS_OK;
    case AVS_NET_SOCKET_OPT_BYTES_RECEIVED:
        out_value->bytes_received = 0;  /* not tracked */
        return AVS_OK;
    default:
        return avs_errno(AVS_ENOTSUP);
    }
}

/* ============================================================================
 * VTable
 * ============================================================================ */

static const avs_net_socket_v_table_t netxduo_udp_vtable = {
    .connect             = netxduo_connect,
    .send                = netxduo_send,
    .send_to             = netxduo_send_to,
    .receive             = netxduo_receive,
    .receive_from        = netxduo_receive_from,
    .bind                = netxduo_bind,
    .close               = netxduo_close,
    .cleanup             = netxduo_cleanup,
    .shutdown            = netxduo_close,
    .get_remote_host     = netxduo_get_remote_host,
    .get_remote_hostname = netxduo_get_remote_host,
    .get_remote_port     = netxduo_get_remote_port,
    .get_local_host      = NULL,
    .get_local_port      = netxduo_get_local_port,
    .get_opt             = netxduo_get_opt,
    .set_opt             = netxduo_set_opt,
    .get_system_socket   = netxduo_get_system_socket,
    .get_interface_name  = NULL,
    .accept              = NULL,
    .decorate            = NULL,
};

/* ============================================================================
 * netxduo_poll — custom poll() implementation for anjay_event_loop
 *
 * Called from handle_sockets() in anjay_event_loop.c via the poll() macro.
 * Calls nx_udp_socket_receive() internally and buffers data in the socket.
 * ============================================================================ */

int netxduo_poll(struct pollfd *fds, nfds_t nfds, int timeout_ms) {
    if (nfds == 0) {
        return 0;
    }

    /* We only support single-socket polling (typical for LwM2M) */
    struct pollfd *fd = &fds[0];
    struct netxduo_sock_impl *sock = fd->fd;

    if (!sock || sock == INVALID_SOCKET) {
        fd->revents = POLLERR;
        return 1;
    }

    fd->revents = 0;

    /* If poll() previously captured a recv error, report it */
    if (avs_is_err(sock->poll_captured_recv_error)) {
        fd->revents = POLLERR;
        return 1;
    }

    /* If there's already buffered data from a previous poll, report ready */
    if (sock->buffered_by_poll_len > 0) {
        fd->revents = fd->events & POLLIN;
        return fd->revents ? 1 : 0;
    }

    /* Convert timeout to NetX ticks */
    ULONG timeout_ticks;
    if (timeout_ms < 0) {
        timeout_ticks = NX_WAIT_FOREVER;
    } else {
        timeout_ticks = (ULONG)((uint32_t)timeout_ms * NX_IP_PERIODIC_RATE
                                 / 1000);
        if (timeout_ticks == 0) {
            timeout_ticks = 1;  /* minimum 1 tick for non-blocking check */
        }
    }

    /* Receive data from the NetX socket */
    NX_PACKET *pkt = NULL;
    UINT status = nx_udp_socket_receive(&sock->nx_udp_sock, &pkt,
                                         timeout_ticks);

    if (status == NX_NO_PACKET) {
        /* Timeout — no data available */
        return 0;
    }

    if (status != NX_SUCCESS) {
        /* Error — capture for netxduo_receive() to report */
        sock->poll_captured_recv_error = avs_error_from_nx_status(status);
        fd->revents = POLLERR;
        return 1;
    }

    /* Data received — buffer it for netxduo_receive() */
    ULONG actual_len = 0;
    nx_packet_data_retrieve(pkt, sock->buffered_by_poll, &actual_len);
    nx_packet_release(pkt);

    if (actual_len > NETXDUO_POLL_BUF_SIZE) {
        actual_len = NETXDUO_POLL_BUF_SIZE;
    }
    sock->buffered_by_poll_len = (size_t) actual_len;

    fd->revents = fd->events & POLLIN;
    return fd->revents ? 1 : 0;
}
