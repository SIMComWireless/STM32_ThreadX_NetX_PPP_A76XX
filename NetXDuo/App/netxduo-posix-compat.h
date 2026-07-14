/**
 ******************************************************************************
 * @file    netxduo-posix-compat.h
 * @brief   AVS_COMMONS_POSIX_COMPAT_HEADER for native NetX Duo
 ******************************************************************************
 * @note
 *   Provides sockfd_t, struct pollfd, poll() for Anjay's event loop.
 *   Based on the SIMCom reference implementation (xcc_com_sockets pattern).
 *
 *   sockfd_t is a POINTER to the socket impl struct (not an int fd).
 *   poll() calls nx_udp_socket_receive() internally and buffers data.
 ******************************************************************************
 */

#ifndef NETXDUO_POSIX_COMPAT_H
#define NETXDUO_POSIX_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#include "nx_api.h"

/* Forward declaration — actual struct defined in avs_net_impl_netxduo.c */
struct netxduo_sock_impl;

/* sockfd_t is a pointer to the socket implementation struct.
 * This is the key trick: avs_net_socket_get_system() returns &sock->self,
 * which is a struct netxduo_sock_impl**. The event loop dereferences it
 * to get struct netxduo_sock_impl*, which is our sockfd_t. */
typedef struct netxduo_sock_impl *sockfd_t;

#ifndef INVALID_SOCKET
#    define INVALID_SOCKET ((sockfd_t) NULL)
#endif

/* poll events */
#define POLLIN  1
#define POLLERR 8

typedef unsigned int nfds_t;

/* struct pollfd — matches the layout expected by anjay_event_loop.c */
struct pollfd {
    sockfd_t fd;
    short    events;
    short    revents;
};

/* Receive buffer size for poll() — must fit one UDP datagram */
#define NETXDUO_POLL_BUF_SIZE 1500

/* NetX poll function — called from anjay_event_loop.c via poll().
 * Calls nx_udp_socket_receive() internally and buffers the result. */
int netxduo_poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);

/* Provide poll() as a macro so anjay_event_loop.c's poll() call works */
#define poll(fds, nfds, timeout) netxduo_poll(fds, nfds, timeout)

#endif /* NETXDUO_POSIX_COMPAT_H */
