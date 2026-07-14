/**
  ******************************************************************************
  * @file    net_iperf.h
  * @brief   TCP/UDP iperf throughput test threads
  ******************************************************************************
  */

#ifndef NET_IPERF_H
#define NET_IPERF_H

#include "nx_api.h"

/** Enable iperf throughput tests (set to 0 for production builds) */
#ifndef IPERF_ENABLE
#define IPERF_ENABLE            0
#endif

/** iperf event flags — consumed by NTP thread to wait for completion */
#define PPP_EVT_TCP_IPERF_DONE  0x04
#define PPP_EVT_UDP_IPERF_DONE  0x08
#define PPP_EVT_IPERF_ALL_DONE  (PPP_EVT_TCP_IPERF_DONE | PPP_EVT_UDP_IPERF_DONE)

#if IPERF_ENABLE

/**
  * @brief  TCP iperf TX thread entry — call via tx_thread_create
  */
void tcp_iperf_thread_entry(ULONG param);

/**
  * @brief  UDP iperf TX thread entry — call via tx_thread_create
  */
void udp_iperf_thread_entry(ULONG param);

#endif /* IPERF_ENABLE */

#endif /* NET_IPERF_H */
