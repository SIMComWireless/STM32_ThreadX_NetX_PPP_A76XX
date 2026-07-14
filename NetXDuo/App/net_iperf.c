/**
  ******************************************************************************
  * @file    net_iperf.c
  * @brief   TCP/UDP iperf TX throughput tests — native NetX Duo API
  ******************************************************************************
  * @note
  *   TCP iperf: connect → send 1460B packets for IPERF_TEST_DURATION
  *   UDP iperf: bind → send 1470B packets (iperf-compatible header) for duration
  *
  *   Execution order: PPP link-up → TCP iperf → UDP iperf → NTP sync
  *   Both threads wait for PPP_EVT_LINK_DOWN after completion to loop back.
  ******************************************************************************
  */

#include "net_iperf.h"

#if IPERF_ENABLE

#include "net_dns.h"
#include "app_netxduo.h"
#include "elog.h"

#include <string.h>

#define TAG_TCP "TCP"
#define TAG_UDP "UDP"

/* ============================================================================
 * Portable byte-swap for wire protocol headers (iperf payload).
 * NetX Duo APIs use host byte order for IP addresses, but iperf
 * payload fields (id, timestamp) are big-endian on the wire.
 * ARM Cortex-M4 is little-endian, so we need actual swapping.
 * ============================================================================ */

#ifndef htonl
static inline uint32_t _netx_htonl(uint32_t v) {
    return ((v & 0xFF000000u) >> 24) |
           ((v & 0x00FF0000u) >>  8) |
           ((v & 0x0000FF00u) <<  8) |
           ((v & 0x000000FFu) << 24);
}
#define htonl(v) _netx_htonl(v)
#define ntohl(v) _netx_htonl(v)
#endif

/* ============================================================================
 * iperf configuration — change these to match your iperf server
 * ============================================================================ */

#ifndef IPERF_TCP_SERVER_HOST
#define IPERF_TCP_SERVER_HOST   "47.109.101.196"
#endif
#ifndef IPERF_TCP_SERVER_PORT
#define IPERF_TCP_SERVER_PORT   9000
#endif
#ifndef IPERF_UDP_SERVER_HOST
#define IPERF_UDP_SERVER_HOST   "47.109.101.196"
#endif
#ifndef IPERF_UDP_SERVER_PORT
#define IPERF_UDP_SERVER_PORT   9003
#endif

/** iperf TX test duration in ticks (10s at 1000Hz = 10000) */
#define IPERF_TEST_DURATION     (10 * TX_TIMER_TICKS_PER_SECOND)

/** UDP iperf packet payload size (bytes) */
#define IPERF_UDP_PACKET_SIZE   1400  /* Must fit within PPP MRU (1480) after IP+UDP headers (28 bytes) */

/* ============================================================================
 * TCP iperf TX thread
 * ============================================================================ */

void tcp_iperf_thread_entry(ULONG param)
{
    (void) param;
    ULONG actual_flags;

    while (1) {
        /* Wait for PPP link-up */
        elog_d(TAG_TCP, "[iperf] Waiting for PPP link-up...");
        tx_event_flags_get(&ppp_events, PPP_EVT_LINK_UP, TX_OR_CLEAR,
                           &actual_flags, TX_WAIT_FOREVER);

        if (ppp_destroying) goto tcp_wait_down;

        /* Resolve server address */
        ULONG server_ip;
        UINT dns_ok = NX_FALSE;
        for (int r = 0; r < 5; r++) {
            if (resolve_host(IPERF_TCP_SERVER_HOST, &server_ip) == NX_SUCCESS) {
                dns_ok = NX_TRUE;
                break;
            }
            elog_w(TAG_TCP, "[iperf] DNS retry %d", r + 1);
            tx_thread_sleep(3000);
        }
        if (!dns_ok) {
            elog_e(TAG_TCP, "[iperf] DNS failed for %s", IPERF_TCP_SERVER_HOST);
            goto tcp_wait_down;
        }

        elog_i(TAG_TCP, "[iperf] TCP TX -> %lu.%lu.%lu.%lu:%d",
               (server_ip >> 24) & 0xFF, (server_ip >> 16) & 0xFF,
               (server_ip >> 8) & 0xFF, server_ip & 0xFF,
               IPERF_TCP_SERVER_PORT);

        /* Create TCP socket */
        NX_TCP_SOCKET tcp_socket;
        UINT status = nx_tcp_socket_create(&ip_0, &tcp_socket, "TCP iperf",
                                            NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                            64, 8192, NX_NULL, NX_NULL);
        if (status != NX_SUCCESS) {
            elog_e(TAG_TCP, "[iperf] nx_tcp_socket_create failed: 0x%02X", status);
            goto tcp_wait_down;
        }

        /* Bind to any port */
        status = nx_tcp_client_socket_bind(&tcp_socket, NX_ANY_PORT, 5 * NX_IP_PERIODIC_RATE);
        if (status != NX_SUCCESS) {
            elog_e(TAG_TCP, "[iperf] bind failed: 0x%02X", status);
            nx_tcp_socket_delete(&tcp_socket);
            goto tcp_wait_down;
        }

        /* Connect — nx_tcp_client_socket_connect handles SYN/ACK handshake */
        status = nx_tcp_client_socket_connect(&tcp_socket, server_ip,
                                               IPERF_TCP_SERVER_PORT,
                                               10 * NX_IP_PERIODIC_RATE);
        if (status != NX_SUCCESS) {
            elog_e(TAG_TCP, "[iperf] connect failed: 0x%02X", status);
            nx_tcp_client_socket_unbind(&tcp_socket);
            nx_tcp_socket_delete(&tcp_socket);
            goto tcp_wait_down;
        }

        elog_i(TAG_TCP, "[iperf] Connected! Starting %ds TCP TX...",
               (int) (IPERF_TEST_DURATION / TX_TIMER_TICKS_PER_SECOND));

        /* Determine send size */
        int send_size = 1460;

        /* Allocate zero-filled send buffer */
        CHAR *send_buf = NULL;
        if (tx_byte_allocate(byte_pool_ptr, (VOID **)&send_buf,
                             send_size, TX_NO_WAIT) != TX_SUCCESS) {
            elog_e(TAG_TCP, "[iperf] buffer alloc failed");
            nx_tcp_client_socket_unbind(&tcp_socket);
            nx_tcp_socket_delete(&tcp_socket);
            goto tcp_wait_down;
        }
        memset(send_buf, 0, send_size);

        /* Timed transmit loop */
        ULONG packets_txed  = 0;
        ULONG bytes_txed    = 0;
        ULONG error_counter = 0;
        ULONG start_time    = tx_time_get();
        ULONG expire_time   = start_time + IPERF_TEST_DURATION;

        while (tx_time_get() < expire_time && !ppp_destroying) {
            NX_PACKET *pkt = NX_NULL;
            status = nx_packet_allocate(&pool_0, &pkt, NX_TCP_PACKET,
                                         10 * NX_IP_PERIODIC_RATE);
            if (status != NX_SUCCESS) {
                error_counter++;
                break;
            }

            status = nx_packet_data_append(pkt, send_buf, send_size,
                                            &pool_0, 10 * NX_IP_PERIODIC_RATE);
            if (status != NX_SUCCESS) {
                nx_packet_release(pkt);
                error_counter++;
                break;
            }

            status = nx_tcp_socket_send(&tcp_socket, pkt, 10 * NX_IP_PERIODIC_RATE);
            if (status != NX_SUCCESS) {
                nx_packet_release(pkt);
                error_counter++;
                break;
            }

            packets_txed++;
            bytes_txed += send_size;
        }

        /* Results */
        ULONG run_time = tx_time_get() - start_time;
        if (run_time == 0) run_time = 1;
        ULONG bytes_per_sec       = bytes_txed / run_time * NX_IP_PERIODIC_RATE
                                  + (bytes_txed % run_time) * NX_IP_PERIODIC_RATE / run_time;
        ULONG throughput_kbps     = bytes_per_sec * 8 / 1000;
        ULONG throughput_mbps_x10 = throughput_kbps / 100;

        elog_i(TAG_TCP, "========================================");
        elog_i(TAG_TCP, "[iperf] TCP TX Results:");
        elog_i(TAG_TCP, "  Server    : %s:%d", IPERF_TCP_SERVER_HOST, IPERF_TCP_SERVER_PORT);
        elog_i(TAG_TCP, "  Duration  : %lu.%lus", run_time / NX_IP_PERIODIC_RATE,
               (run_time % NX_IP_PERIODIC_RATE) * 10 / NX_IP_PERIODIC_RATE);
        elog_i(TAG_TCP, "  Pkt Size  : %d bytes", send_size);
        elog_i(TAG_TCP, "  Packets TX: %lu", packets_txed);
        elog_i(TAG_TCP, "  Bytes TX  : %lu (%lu KB)", bytes_txed, bytes_txed / 1024);
        elog_i(TAG_TCP, "  Throughput: %lu.%lu Mbps", throughput_mbps_x10 / 10,
               throughput_mbps_x10 % 10);
        elog_i(TAG_TCP, "  Errors    : %lu", error_counter);
        elog_i(TAG_TCP, "========================================");

        /* Disconnect and cleanup */
        nx_tcp_socket_disconnect(&tcp_socket, 5 * NX_IP_PERIODIC_RATE);
        nx_tcp_client_socket_unbind(&tcp_socket);
        nx_tcp_socket_delete(&tcp_socket);

tcp_wait_down:
        tx_event_flags_set(&ppp_events, PPP_EVT_TCP_IPERF_DONE, TX_OR);
        tx_event_flags_get(&ppp_events, PPP_EVT_LINK_DOWN, TX_OR_CLEAR,
                           &actual_flags, TX_WAIT_FOREVER);
    }
}

/* ============================================================================
 * UDP iperf TX thread
 * ============================================================================ */

void udp_iperf_thread_entry(ULONG param)
{
    (void) param;
    ULONG actual_flags;

    while (1) {
        /* Wait for TCP iperf to finish */
        elog_d(TAG_UDP, "[iperf] Waiting for TCP iperf...");
        tx_event_flags_get(&ppp_events, PPP_EVT_TCP_IPERF_DONE, TX_AND,
                           &actual_flags, TX_WAIT_FOREVER);

        if (ppp_destroying) goto udp_wait_down;

        /* Resolve server address */
        ULONG server_ip;
        UINT dns_ok = NX_FALSE;
        for (int r = 0; r < 5; r++) {
            if (resolve_host(IPERF_UDP_SERVER_HOST, &server_ip) == NX_SUCCESS) {
                dns_ok = NX_TRUE;
                break;
            }
            elog_w(TAG_UDP, "[iperf] DNS retry %d", r + 1);
            tx_thread_sleep(3000);
        }
        if (!dns_ok) {
            elog_e(TAG_UDP, "[iperf] DNS failed for %s", IPERF_UDP_SERVER_HOST);
            goto udp_wait_down;
        }

        elog_i(TAG_UDP, "[iperf] UDP TX -> %lu.%lu.%lu.%lu:%d",
               (server_ip >> 24) & 0xFF, (server_ip >> 16) & 0xFF,
               (server_ip >> 8) & 0xFF, server_ip & 0xFF,
               IPERF_UDP_SERVER_PORT);

        /* Create UDP socket */
        NX_UDP_SOCKET udp_socket;
        UINT status = nx_udp_socket_create(&ip_0, &udp_socket, "UDP iperf",
                                            NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                            64, NX_NULL);
        if (status != NX_SUCCESS) {
            elog_e(TAG_UDP, "[iperf] nx_udp_socket_create failed: 0x%02X", status);
            goto udp_wait_down;
        }

        /* Bind to any port */
        status = nx_udp_socket_bind(&udp_socket, NX_ANY_PORT, 5 * NX_IP_PERIODIC_RATE);
        if (status != NX_SUCCESS) {
            elog_e(TAG_UDP, "[iperf] bind failed: 0x%02X", status);
            nx_udp_socket_delete(&udp_socket);
            goto udp_wait_down;
        }

        /* Disable UDP checksum — some PPP links have checksum issues */
        nx_udp_socket_checksum_disable(&udp_socket);

        /* Allocate zero-filled padding buffer */
        CHAR *pad_buf = NULL;
        if (tx_byte_allocate(byte_pool_ptr, (VOID **)&pad_buf,
                             IPERF_UDP_PACKET_SIZE, TX_NO_WAIT) != TX_SUCCESS) {
            elog_e(TAG_UDP, "[iperf] pad buffer alloc failed");
            nx_udp_socket_unbind(&udp_socket);
            nx_udp_socket_delete(&udp_socket);
            goto udp_wait_down;
        }
        memset(pad_buf, 0, IPERF_UDP_PACKET_SIZE);

        elog_i(TAG_UDP, "[iperf] Socket ready. Starting %ds UDP TX (pkt_size=%d)...",
               (int) (IPERF_TEST_DURATION / TX_TIMER_TICKS_PER_SECOND),
               IPERF_UDP_PACKET_SIZE);

        /* Timed transmit loop */
        ULONG packets_txed  = 0;
        ULONG bytes_txed    = 0;
        ULONG error_counter = 0;
        int   udp_id        = 0;
        ULONG start_time    = tx_time_get();
        ULONG expire_time   = start_time + IPERF_TEST_DURATION;

        while (tx_time_get() < expire_time && !ppp_destroying) {
            /* Allocate packet */
            NX_PACKET *pkt = NX_NULL;
            status = nx_packet_allocate(&pool_0, &pkt, NX_UDP_PACKET,
                                         10 * NX_IP_PERIODIC_RATE);
            if (status != NX_SUCCESS) {
                error_counter++;
                break;
            }

            /* Build iperf-compatible payload: id(4) + tv_sec(4) + tv_usec(4) + padding */
            ULONG header[3];
            ULONG ticks = tx_time_get();
            header[0] = htonl((uint32_t) udp_id);
            header[1] = htonl((uint32_t) (ticks / NX_IP_PERIODIC_RATE));
            header[2] = htonl((uint32_t) ((ticks % NX_IP_PERIODIC_RATE) * 1000000
                                            / NX_IP_PERIODIC_RATE));

            /* Append header */
            status = nx_packet_data_append(pkt, header, 12,
                                            &pool_0, 10 * NX_IP_PERIODIC_RATE);
            if (status != NX_SUCCESS) {
                nx_packet_release(pkt);
                error_counter++;
                break;
            }

            /* Append padding to reach IPERF_UDP_PACKET_SIZE */
            ULONG padding_len = IPERF_UDP_PACKET_SIZE - 12;
            status = nx_packet_data_append(pkt, pad_buf, padding_len,
                                            &pool_0, 10 * NX_IP_PERIODIC_RATE);
            if (status != NX_SUCCESS) {
                nx_packet_release(pkt);
                error_counter++;
                break;
            }

            /* Send to destination */
            status = nx_udp_socket_send(&udp_socket, pkt,
                                         server_ip, IPERF_UDP_SERVER_PORT);
            if (status != NX_SUCCESS) {
                elog_e(TAG_UDP, "[iperf] nx_udp_socket_send failed: 0x%02X (pkt #%d)",
                       status, udp_id);
                nx_packet_release(pkt);
                error_counter++;
            } else if (udp_id < 3) {
                elog_i(TAG_UDP, "[iperf] UDP pkt #%d sent (%d bytes)",
                       udp_id, IPERF_UDP_PACKET_SIZE);
            }

            udp_id = (udp_id + 1) & 0x7FFFFFFF;
            packets_txed++;
            bytes_txed += IPERF_UDP_PACKET_SIZE;
        }

        /* Results */
        ULONG run_time = tx_time_get() - start_time;
        if (run_time == 0) run_time = 1;
        ULONG bytes_per_sec       = bytes_txed / run_time * NX_IP_PERIODIC_RATE
                                  + (bytes_txed % run_time) * NX_IP_PERIODIC_RATE / run_time;
        ULONG throughput_kbps     = bytes_per_sec * 8 / 1000;
        ULONG throughput_mbps_x10 = throughput_kbps / 100;

        elog_i(TAG_UDP, "========================================");
        elog_i(TAG_UDP, "[iperf] UDP TX Results:");
        elog_i(TAG_UDP, "  Server    : %s:%d", IPERF_UDP_SERVER_HOST, IPERF_UDP_SERVER_PORT);
        elog_i(TAG_UDP, "  Duration  : %lu.%lus", run_time / NX_IP_PERIODIC_RATE,
               (run_time % NX_IP_PERIODIC_RATE) * 10 / NX_IP_PERIODIC_RATE);
        elog_i(TAG_UDP, "  Pkt Size  : %d bytes", IPERF_UDP_PACKET_SIZE);
        elog_i(TAG_UDP, "  Packets TX: %lu", packets_txed);
        elog_i(TAG_UDP, "  Bytes TX  : %lu (%lu KB)", bytes_txed, bytes_txed / 1024);
        elog_i(TAG_UDP, "  Throughput: %lu.%lu Mbps", throughput_mbps_x10 / 10,
               throughput_mbps_x10 % 10);
        elog_i(TAG_UDP, "  Errors    : %lu", error_counter);
        elog_i(TAG_UDP, "========================================");

        /* Cleanup */
        nx_udp_socket_unbind(&udp_socket);
        nx_udp_socket_delete(&udp_socket);

udp_wait_down:
        tx_event_flags_set(&ppp_events, PPP_EVT_UDP_IPERF_DONE, TX_OR);
        tx_event_flags_get(&ppp_events, PPP_EVT_LINK_DOWN, TX_OR_CLEAR,
                           &actual_flags, TX_WAIT_FOREVER);
    }
}

#endif /* IPERF_ENABLE */
