/**
  ******************************************************************************
  * @file    net_ntp.c
  * @brief   NTP time synchronization via SNTP + RTC set
  ******************************************************************************
  * @note
  *   Waits for PPP link-up (+ iperf completion if IPERF_ENABLE), then:
  *   1. Resolves NTP_SERVER_HOST via DNS (3 retries)
  *   2. Falls back to hardcoded IP 203.107.6.88 if DNS fails
  *   3. Runs SNTP unicast request (3 retries per server)
  *   4. Converts NTP epoch (1900) to Unix epoch (1970)
  *   5. Sets MCU RTC via HAL_RTC_SetTime/SetDate
  ******************************************************************************
  */

#include "net_ntp.h"
#include "net_dns.h"
#include "net_iperf.h"
#include "app_netxduo.h"
#include "nxd_sntp_client.h"
#include "elog.h"
#include "rtc.h"

#define TAG_NTP "NTP"
#define TAG_DNS "DNS"

/* ============================================================================
 * Helpers: Unix timestamp → year/month/day
 * ============================================================================ */

/** Days in each month (non-leap year) */
static const uint8_t days_in_month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static int is_leap_year(uint32_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/**
  * @brief  Convert Unix timestamp to year, month, day (UTC)
  * @param  unix_time  Seconds since 1970-01-01 00:00:00 UTC
  * @param  year       Output: full year (e.g. 2026)
  * @param  month      Output: 1-12
  * @param  day        Output: 1-31
  */
static void unix_to_ymd(uint32_t unix_time, uint32_t *year, uint32_t *month, uint32_t *day)
{
    uint32_t days = unix_time / 86400;

    /* Find year */
    uint32_t y = 1970;
    while (1) {
        uint32_t days_this_year = is_leap_year(y) ? 366 : 365;
        if (days < days_this_year) break;
        days -= days_this_year;
        y++;
    }

    /* Find month */
    uint32_t m = 1;
    while (m <= 12) {
        uint32_t dim = days_in_month[m - 1];
        if (m == 2 && is_leap_year(y)) dim = 29;
        if (days < dim) break;
        days -= dim;
        m++;
    }

    *year  = y;
    *month = m;
    *day   = days + 1;  /* days is 0-based within month */
}

/* ============================================================================
 * NTP sync thread
 * ============================================================================ */

static NX_SNTP_CLIENT sntp_client;

void ntp_thread_entry(ULONG param)
{
    (void)param;
    ULONG actual_flags;
    UINT status;
    ULONG ntp_ip_address;

    while (1)
    {
        /* Wait for PPP link-up event */
        elog_d(TAG_NTP, "Waiting for PPP link...");
        tx_event_flags_get(&ppp_events, PPP_EVT_LINK_UP, TX_OR_CLEAR,
                           &actual_flags, TX_WAIT_FOREVER);

        elog_d(TAG_NTP, "PPP link is up, starting time sync...");

#if IPERF_ENABLE
        /* Wait for BOTH TCP and UDP iperf tests to complete (AND, not OR) */
        elog_d(TAG_NTP, "Waiting for iperf tests to finish...");
        tx_event_flags_get(&ppp_events, PPP_EVT_IPERF_ALL_DONE, TX_AND_CLEAR,
                           &actual_flags, TX_WAIT_FOREVER);
        elog_d(TAG_NTP, "iperf tests done");
#endif

        /* ---- Step 3: Resolve NTP server hostname (with retries) ---- */
        UINT dns_ok = NX_FALSE;
        {
            UINT dns_retry;
            for (dns_retry = 0; dns_retry < 3; dns_retry++)
            {
                elog_d(TAG_DNS, "Resolving %s (attempt %u) ...", NTP_SERVER_HOST, dns_retry + 1);
                status = resolve_host(NTP_SERVER_HOST, &ntp_ip_address);
                if (status == NX_SUCCESS)
                {
                    dns_ok = NX_TRUE;
                    break;
                }
                elog_w(TAG_DNS, "DNS attempt %u failed: 0x%02X", dns_retry + 1, status);
                tx_thread_sleep(3000);
            }
        }

        /* ---- Step 5-6: SNTP time sync ---- */
        /*
         * Strategy:
         *   1. If DNS resolved, try NTP up to 3 times with the resolved IP.
         *   2. If DNS failed, OR all 3 NTP attempts with DNS IP failed,
         *      fall back to hardcoded IP and try 3 more times.
         */
        {
            UINT sntp_ok = NX_FALSE;

            /* Build list of IPs to try: [DNS-resolved IP, fallback IP] */
            ULONG ntp_ips[2];
            UINT  num_ips = 0;
            const char *ip_labels[2];

            if (dns_ok)
            {
                ntp_ips[num_ips]    = ntp_ip_address;
                ip_labels[num_ips]  = NTP_SERVER_HOST;
                num_ips++;
            }

            /* Always add fallback IP (used when DNS fails, or as 2nd attempt) */
            ntp_ips[num_ips]    = IP_ADDRESS(203, 107, 6, 88);
            ip_labels[num_ips]  = "203.107.6.88 (fallback)";
            num_ips++;

            /* If DNS succeeded, we try DNS IP first; if that fails 3 times,
               we also try fallback. If DNS failed, we only try fallback. */
            for (UINT ip_idx = 0; ip_idx < num_ips && !sntp_ok; ip_idx++)
            {
                ULONG cur_ip = ntp_ips[ip_idx];

                elog_i(TAG_NTP, "NTP server: %s = %lu.%lu.%lu.%lu",
                      ip_labels[ip_idx],
                      (cur_ip >> 24) & 0xFF,
                      (cur_ip >> 16) & 0xFF,
                      (cur_ip >> 8) & 0xFF,
                      cur_ip & 0xFF);

                for (UINT sntp_retry = 0; sntp_retry < 3; sntp_retry++)
                {
                    NX_IP *active_ip = app_netxduo_get_active_ip();
                    NX_PACKET_POOL *active_pool = app_netxduo_get_active_pool();
                    if (active_ip == NULL || active_pool == NULL) { break; }

                    status = nx_sntp_client_create(&sntp_client, active_ip, 0, active_pool,
                                                    NX_NULL, NX_NULL, NX_NULL);
                    if (status != NX_SUCCESS)
                    {
                        elog_e(TAG_NTP, "SNTP client create failed: 0x%02X", status);
                        break;
                    }

                    status = nx_sntp_client_initialize_unicast(&sntp_client, cur_ip);
                    if (status != NX_SUCCESS)
                    {
                        elog_e(TAG_NTP, "SNTP unicast init failed: 0x%02X", status);
                        nx_sntp_client_delete(&sntp_client);
                        break;
                    }

                    status = nx_sntp_client_run_unicast(&sntp_client);
                    if (status != NX_SUCCESS)
                    {
                        elog_e(TAG_NTP, "SNTP run failed: 0x%02X", status);
                        nx_sntp_client_delete(&sntp_client);
                        break;
                    }

                    elog_d(TAG_NTP, "SNTP request attempt %u to %lu.%lu.%lu.%lu ...",
                          sntp_retry + 1,
                          (cur_ip >> 24) & 0xFF,
                          (cur_ip >> 16) & 0xFF,
                          (cur_ip >> 8) & 0xFF,
                          cur_ip & 0xFF);

                    status = nx_sntp_client_request_unicast_time(&sntp_client, 30);
                    nx_sntp_client_stop(&sntp_client);

                    if (status == NX_SUCCESS)
                    {
                        ULONG seconds, fraction;
                        status = nx_sntp_client_get_local_time(&sntp_client, &seconds, &fraction, NX_NULL);
                        if (status == NX_SUCCESS)
                        {
                            /* Convert NTP epoch (1900) to Unix epoch (1970) */
                            uint32_t unix_time = seconds - NTP_EPOCH_OFFSET;

                            elog_d(TAG_NTP, "NTP time received!");
                            elog_d(TAG_NTP, "Unix timestamp: %lu", (unsigned long)unix_time);

                            /* ---- Step 7: Set RTC ---- */
                            uint32_t s = unix_time % 60;
                            uint32_t m = (unix_time / 60) % 60;
                            uint32_t h = ((unix_time / 3600) + NTP_TZ_OFFSET_HOURS) % 24;

                            elog_d(TAG_NTP, "Time (UTC+8): %02lu:%02lu:%02lu", h, m, s);

                            RTC_TimeTypeDef sTime = {0};
                            RTC_DateTypeDef sDate = {0};

                            sTime.Hours   = (uint8_t)h;
                            sTime.Minutes = (uint8_t)m;
                            sTime.Seconds = (uint8_t)s;
                            if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK)
                            {
                                elog_e(TAG_NTP, "RTC SetTime failed");
                            }

                            uint32_t rtc_year, rtc_month, rtc_day;
                            unix_to_ymd(unix_time, &rtc_year, &rtc_month, &rtc_day);

                            uint32_t days = unix_time / 86400;
                            sDate.WeekDay = (uint8_t)((days + 4) % 7);
                            sDate.Year    = (uint8_t)(rtc_year - 2000);
                            sDate.Month   = (uint8_t)rtc_month;
                            sDate.Date    = (uint8_t)rtc_day;
                            if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK)
                            {
                                elog_e(TAG_NTP, "RTC SetDate failed");
                            }

                            elog_d(TAG_NTP, "Date (UTC+8): %04lu-%02lu-%02lu", rtc_year, rtc_month, rtc_day);
                            elog_d(TAG_NTP, "RTC synchronized successfully!");
                            sntp_ok = NX_TRUE;
                        }
                        else
                        {
                            elog_e(TAG_NTP, "Get local time failed: 0x%02X", status);
                        }
                    }
                    else
                    {
                        elog_w(TAG_NTP, "SNTP request attempt %u failed: 0x%02X", sntp_retry + 1, status);
                    }

                    nx_sntp_client_delete(&sntp_client);

                    if (sntp_ok) break;
                    tx_thread_sleep(3000);  /* Wait before retry */
                }
            }

            if (!sntp_ok)
            {
                elog_e(TAG_NTP, "NTP sync failed after trying all servers");
            }
        }

        elog_d(TAG_NTP, "Time sync complete");

        /* Signal NTP done — Anjay can start now */
        tx_event_flags_set(&ppp_events, PPP_EVT_NTP_DONE, TX_OR);

        /* Wait for next PPP link-down/up cycle */
        tx_event_flags_get(&ppp_events, PPP_EVT_LINK_DOWN, TX_OR_CLEAR,
                           &actual_flags, TX_WAIT_FOREVER);
    }
}
