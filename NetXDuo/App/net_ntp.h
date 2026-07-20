/**
  ******************************************************************************
  * @file    net_ntp.h
  * @brief   NTP time synchronization via SNTP + RTC set
  ******************************************************************************
  */

#ifndef NET_NTP_H
#define NET_NTP_H

#include "nx_api.h"

/** NTP server hostname — configurable */
#ifndef NTP_SERVER_HOST
#define NTP_SERVER_HOST         "ntp.aliyun.com"
#endif

/** NTP epoch offset (seconds from 1900-01-01 to 1970-01-01) */
#define NTP_EPOCH_OFFSET        2208988800UL

/** UTC timezone offset in hours — change for your region (e.g. +8 for China) */
#ifndef NTP_TZ_OFFSET_HOURS
#define NTP_TZ_OFFSET_HOURS     8
#endif

/** Periodic NTP re-sync interval in seconds (default: 120 = 2 minutes) */
#ifndef NTP_SYNC_INTERVAL_SEC
#define NTP_SYNC_INTERVAL_SEC   120
#endif

/** NTP thread priority and stack size */
#define NTP_THREAD_PRIO         31
#define NTP_THREAD_STACK        (1024 * 3)

/**
  * @brief  NTP sync thread entry — call via tx_thread_create.
  *         Waits for PPP link-up (+ iperf completion if enabled), then
  *         resolves NTP server, runs SNTP, and sets the MCU RTC.
  */
void ntp_thread_entry(ULONG param);

#endif /* NET_NTP_H */
