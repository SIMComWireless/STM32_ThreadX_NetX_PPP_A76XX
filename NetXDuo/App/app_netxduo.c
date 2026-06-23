/**
  ******************************************************************************
  * @file    app_netxduo.c
  * @brief   NetXDuo application — PPP over A7683E + DNS + SNTP time sync
  ******************************************************************************
  * @note
  *   Flow:
  *   1. MX_NetXDuo_Init() — creates IP instance, packet pool, PPP instance
  *   2. PPP thread — initializes modem, dials PPP, feeds bytes to NetX
  *   3. On PPP link-up: DNS resolves NTP server, SNTP syncs time
  *   4. RTC is set from NTP time
  ******************************************************************************
  */

/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_netxduo.c
  * @author  MCD Application Team
  * @brief   NetXDuo applicative file
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "app_netxduo.h"
#include "nx_api.h"
#include "nx_ppp.h"
#include "nxd_dns.h"
#include "nxd_sntp_client.h"
#include "elog.h"
#include "a7683e.h"
#include "bsp_serial.h"
#include "main.h"
#include "rtc.h"
#include "ux_network_driver.h"
#include "nxd_dhcp_client.h"
#include <string.h>
#include <stdio.h>

#define TAG "NET"

/* USER CODE BEGIN Includes */
/* USER CODE END Includes */

/* Private defines -----------------------------------------------------------*/

#define TAG_NTP     "NTP"
#define TAG_PPP     "PPP"
#define TAG_DNS     "DNS"
#define TAG_TCP     "TCP"
#define TAG_UDP     "UDP"

/** NTP server hostname — configurable */
#ifndef NTP_SERVER_HOST
#define NTP_SERVER_HOST         "ntp.aliyun.com"
#endif

/** NTP epoch offset (seconds from 1900-01-01 to 1970-01-01) */
#define NTP_EPOCH_OFFSET        2208988800UL

/** Days in each month (non-leap year) */
static const uint8_t days_in_month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

/**
 * @brief  Check if a year is a leap year
 */
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

/** PPP serial read thread priority — highest app priority,
 *  must keep up with UART3 DMA to avoid byte loss */
#define PPP_READ_THREAD_PRIO    5
#define PPP_READ_THREAD_STACK   1024

/** NTP sync thread priority — background, runs once after PPP up */
#define NTP_THREAD_PRIO         31
#define NTP_THREAD_STACK        1024*3

/** Packet pool configuration */
#define PACKET_POOL_SIZE        4096*5
#define PACKET_PAYLOAD_SIZE     1536    /* Slightly larger than PPP MTU (1500) */

/** PPP thread stack size */
#define PPP_THREAD_STACK_SIZE   1024

/** TCP/UDP client configuration — change these to match your server */
#ifndef TCP_SERVER_HOST
#define TCP_SERVER_HOST         "47.109.101.196"
#endif
#ifndef TCP_SERVER_PORT
#define TCP_SERVER_PORT         9000
#endif
#ifndef UDP_SERVER_HOST
#define UDP_SERVER_HOST         "47.109.101.196"
#endif
#ifndef UDP_SERVER_PORT
#define UDP_SERVER_PORT         9003
#endif
#ifndef CLIENT_SEND_PERIOD
#define CLIENT_SEND_PERIOD      5000    /* Send period in ticks (5s at 1000Hz) */
#endif

/** TCP/UDP client thread priorities — unique per thread */
#define TCP_CLIENT_THREAD_PRIO  32
#define TCP_CLIENT_THREAD_STACK 1024*3
#define UDP_CLIENT_THREAD_PRIO  33
#define UDP_CLIENT_THREAD_STACK 1024*3

/** TCP/UDP receive buffer size */
#define CLIENT_RX_BUF_SIZE      256

/** IP instance internal memory size */
#define IP_MEMORY_SIZE          2048

/** ARP cache memory size */
#define ARP_CACHE_SIZE          1024

/* Private variables ---------------------------------------------------------*/

/* NetX objects */
static NX_PACKET_POOL    pool_0;
static NX_IP             ip_0;
NX_PPP                   ppp_0;
static NX_SNTP_CLIENT    sntp_client;


/* Memory pointers — allocated from NX byte pool */
static VOID *pool_memory_ptr;
static VOID *ip_memory_ptr;
static VOID *arp_cache_ptr;
static VOID *ppp_stack_ptr;
static VOID *ppp_read_stack_ptr;
static VOID *ntp_stack_ptr;
static VOID *tcp_client_stack_ptr;
static VOID *udp_client_stack_ptr;

/* PPP link state */
static TX_EVENT_FLAGS_GROUP ppp_events;
#define PPP_EVT_LINK_UP     0x01
#define PPP_EVT_LINK_DOWN   0x02

/* Thread handles */
static TX_THREAD ppp_read_thread;
static TX_THREAD ntp_thread;
static TX_THREAD tcp_client_thread;
static TX_THREAD udp_client_thread;

/* PPP creation flag — deferred to thread context */
static uint8_t ppp_created = 0;

/* Flag: set when app_netxduo_destroy_ppp() begins teardown.
 * TCP/UDP threads check this before calling nx_* cleanup APIs
 * to avoid operating on a deleted IP instance. */
static volatile uint8_t ppp_destroying = 0;

/* Modem reconnect event — USB disconnect signals this to unblock modem thread */
static TX_EVENT_FLAGS_GROUP modem_events;
static uint8_t modem_events_created = 0;

/* Global DNS client — created once, reused across all resolve_host() calls.
 * Protected by dns_mutex to prevent concurrent create/delete from
 * NTP, TCP, and UDP threads calling resolve_host() simultaneously. */
static NX_DNS  dns_client;
static uint8_t dns_client_created = 0;
static TX_MUTEX dns_mutex;

/* PPP serial I/O buffer */
#define PPP_RX_BATCH_SIZE   1024
static uint8_t ppp_rx_buf[PPP_RX_BATCH_SIZE];

/* Serial port for PPP data (set by app_netxduo_set_serial).
 * In direct mode: bsp_serial_uart3
 * In CMUX mode:   cmux_serial_get(CMUX_PPP_DLCI) */
static bsp_serial_t *ppp_serial = NULL;

/* Debug: track which init step failed (inspect in debugger) */
volatile UINT netx_init_status = 0;
volatile UINT netx_init_step = 0;

/* ---- CDC-ECM network interface ---- */

#define TAG_ECM     "ECM"

/** ECM IP instance internal memory (does not need to come from pool) */
#define ECM_IP_MEMORY_SIZE      2048
#define ECM_ARP_CACHE_SIZE      1024
#define ECM_PACKET_POOL_SIZE    (1536 * 8 + 128 * 8)  /* 8 × (1536 payload + ~128 NX_PACKET overhead) */
#define ECM_PACKET_PAYLOAD      1536
#define ECM_DHCP_THREAD_STACK   1024
#define ECM_DHCP_THREAD_PRIO    30

static NX_IP            ecm_ip;
static NX_PACKET_POOL   ecm_pool;
static NX_DHCP          ecm_dhcp;
static TX_THREAD        ecm_dhcp_thread;
static uint8_t          ecm_ip_created = 0;
static uint8_t          ecm_ip_logged  = 0;

/* Statically allocated memory for ECM — avoids pool fragmentation */
static ULONG            ecm_ip_memory[ECM_IP_MEMORY_SIZE / sizeof(ULONG)];
static ULONG            ecm_arp_cache[ECM_ARP_CACHE_SIZE / sizeof(ULONG)];
static ULONG            ecm_dhcp_stack[ECM_DHCP_THREAD_STACK / sizeof(ULONG)];
static ULONG            ecm_pool_memory[ECM_PACKET_POOL_SIZE / sizeof(ULONG)];

/* Active IP/pool accessors — return the PPP IP instance and its packet pool.
 * Used by NTP, TCP, UDP threads to get the active network context. */
NX_IP *app_netxduo_get_active_ip(void)   { return &ip_0; }
NX_PACKET_POOL *app_netxduo_get_active_pool(void) { return &pool_0; }

/* USER CODE BEGIN PV */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/

static void ppp_read_thread_entry(ULONG param);
static void ntp_thread_entry(ULONG param);
static void tcp_client_thread_entry(ULONG param);
static void udp_client_thread_entry(ULONG param);
static UINT resolve_host(const char *host, ULONG *ip_address);
static void ppp_link_up_callback(NX_PPP *ppp_ptr);
static void ppp_link_down_callback(NX_PPP *ppp_ptr);
static void dns_client_cleanup(void);
static UINT ppp_generate_login(CHAR *name, CHAR *password);
static void ppp_byte_send(UCHAR byte);
static UINT dns_client_setup(void);

/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/**
  * @brief  Set the serial port used for PPP data transport
  * @param  serial  Pointer to a bsp_serial_t instance
  */
void app_netxduo_set_serial(bsp_serial_t *serial)
{
    ppp_serial = serial;
}

/**
  * @brief  PPP byte send callback — called by NetX PPP to output one byte
  * @note   Buffers bytes and flushes as a single DMA transfer per PPP frame.
  *         PPP frames are delimited by 0x7E (flag byte). On each 0x7E outside
  *         an escape context, the buffer is flushed. This reduces DMA setup from
  *         ~20 times/frame to 1 time/frame, significantly improving throughput.
  *         Uses ppp_serial which is set to either UART3 (direct) or
  *         CMUX DLCI 2 (virtual serial) by app_netxduo_set_serial().
  */
/* PPP byte send buffer state — must be reset on reconnection */
static uint8_t  ppp_tx_buf[4096];
static uint16_t ppp_tx_pos = 0;
static uint8_t  ppp_tx_overflow_logged = 0;

/**
 * @brief  Reset PPP TX buffer state — call before starting a new PPP session.
 */
static void ppp_byte_send_reset(void)
{
    ppp_tx_pos = 0;
    ppp_tx_overflow_logged = 0;
}

static void ppp_byte_send(UCHAR byte)
{
    /*
     * Buffer an entire PPP frame (leading 0x7E … trailing 0x7E) and flush
     * it in a single uart3_write() call — one DMA transfer per frame.
     *
     * Worst-case size: MRU(1500) + addr/ctrl(2) + proto(2) + FCS(2) = 1506
     * raw bytes, each potentially escaped (×2) = 3012, plus two 0x7E flags.
     * 4096 bytes covers this with margin.
     */

    if (!ppp_serial) return;

    /* 0x7E = PPP flag byte — marks frame start / end */
    if (byte == 0x7E)
    {
        if (ppp_tx_pos == 0)
        {
            /* Start of new frame — begin buffering */
            ppp_tx_buf[ppp_tx_pos++] = 0x7E;
        }
        else if (ppp_tx_buf[0] == 0x7E)
        {
            /* End of frame — flush everything in one write */
            if (ppp_tx_pos < sizeof(ppp_tx_buf))
                ppp_tx_buf[ppp_tx_pos++] = 0x7E;
            ppp_serial->write(ppp_tx_buf, ppp_tx_pos);

        #ifdef NX_PPP_DEBUG_LOG_ENABLE
            elog_hexdump("PPP TX", 16, ppp_tx_buf, ppp_tx_pos);
        #endif /* NX_PPP_DEBUG_LOG_ENABLE */
            ppp_tx_pos = 0;
        }
        else
        {
            /* Stale non-PPP data — discard and start fresh frame */
            ppp_tx_pos = 0;
            ppp_tx_buf[ppp_tx_pos++] = 0x7E;
        }
        return;
    }

    /* Buffer frame payload (space permitting) */
    if (ppp_tx_pos < sizeof(ppp_tx_buf))
    {
        ppp_tx_buf[ppp_tx_pos++] = byte;
    }
    else
    {
        /* Frame exceeds 4096 bytes — should never happen with standard PPP MTU */
        if (!ppp_tx_overflow_logged) {
            ppp_tx_overflow_logged = 1;
            elog_e(TAG_PPP, "PPP TX buffer overflow! Frame > %u bytes", (unsigned)sizeof(ppp_tx_buf));
        }
    }
}

/**
  * @brief  Print negotiated IP addresses (IPv4 / IPv6 / dual-stack)
  * @note   Reads addresses from the IP instance after PPP link-up.
  *         IPv6 support is conditional on FEATURE_NX_IPV6.
  */
static void print_ip_addresses(void)
{
    UINT status;
    NX_IP *active_ip = &ip_0;

    /* ---- IPv4 ---- */
    ULONG ipv4_addr = 0, ipv4_mask = 0;
    status = nx_ip_address_get(active_ip, &ipv4_addr, &ipv4_mask);
    if (status == NX_SUCCESS && ipv4_addr != 0)
    {
        elog_d(TAG_PPP, "IPv4: %lu.%lu.%lu.%lu  Mask: %lu.%lu.%lu.%lu",
              (ipv4_addr >> 24) & 0xFF, (ipv4_addr >> 16) & 0xFF,
              (ipv4_addr >> 8)  & 0xFF,  ipv4_addr & 0xFF,
              (ipv4_mask >> 24) & 0xFF, (ipv4_mask >> 16) & 0xFF,
              (ipv4_mask >> 8)  & 0xFF,  ipv4_mask & 0xFF);

        ULONG gw = 0;
        nx_ip_gateway_address_get(active_ip, &gw);
        elog_d(TAG_PPP, "Gateway: %lu.%lu.%lu.%lu",
              (gw >> 24) & 0xFF, (gw >> 16) & 0xFF,
              (gw >> 8)  & 0xFF,  gw & 0xFF);

        /* Interface status */
        ULONG if_link_up = 0;
        nx_ip_interface_status_check(active_ip, 0, NX_IP_LINK_ENABLED,
                                     &if_link_up, NX_NO_WAIT);
        elog_d(TAG_PPP, "Interface: %s, ARP: %s, MTU: %lu",
              if_link_up ? "UP" : "DOWN",
              active_ip->nx_ip_interface[0].nx_interface_address_mapping_needed ? "yes" : "no",
              (unsigned long)active_ip->nx_ip_interface[0].nx_interface_ip_mtu_size);
    }
    else
    {
        elog_w(TAG_PPP, "IPv4: not assigned");
    }
		
    /* ---- IPv6 ---- */
#ifdef FEATURE_NX_IPV6
    {
        UINT has_ipv6 = 0;
        for (UINT i = 0; i < NX_MAX_IPV6_ADDRESSES; i++)
        {
            NXD_ADDRESS nxd_addr;
            ULONG prefix_len = 0;
            UINT  if_idx = 0;

            status = nxd_ipv6_address_get(active_ip, i, &nxd_addr, &prefix_len, &if_idx);
            if (status == NX_SUCCESS && nxd_addr.nxd_ip_address.v6[0] != 0)
            {
                elog_d(TAG_PPP, "IPv6[%u]: %04lX:%04lX:%04lX:%04lX:%04lX:%04lX:%04lX:%04lX /%lu (if=%u)",
                      i,
                      (nxd_addr.nxd_ip_address.v6[0] >> 16) & 0xFFFF,
                       nxd_addr.nxd_ip_address.v6[0] & 0xFFFF,
                      (nxd_addr.nxd_ip_address.v6[1] >> 16) & 0xFFFF,
                       nxd_addr.nxd_ip_address.v6[1] & 0xFFFF,
                      (nxd_addr.nxd_ip_address.v6[2] >> 16) & 0xFFFF,
                       nxd_addr.nxd_ip_address.v6[2] & 0xFFFF,
                      (nxd_addr.nxd_ip_address.v6[3] >> 16) & 0xFFFF,
                       nxd_addr.nxd_ip_address.v6[3] & 0xFFFF,
                      prefix_len, if_idx);
                has_ipv6 = 1;
            }
        }
        if (!has_ipv6)
        {
            elog_w(TAG_PPP, "IPv6: not assigned");
        }
    }
#else
    elog_d(TAG_PPP, "IPv6: disabled (NX_DISABLE_IPV6)");
#endif /* FEATURE_NX_IPV6 */
}

/**
  * @brief  NetX PPP link-up callback
  */
static void ppp_link_up_callback(NX_PPP *ppp_ptr)
{
    elog_d(TAG_PPP, "PPP link UP");
    /* Print negotiated IP addresses */
    print_ip_addresses();
    tx_event_flags_set(&ppp_events, PPP_EVT_LINK_UP, TX_OR);
}

/**
  * @brief  NetX PPP link-down callback
  */
static void ppp_link_down_callback(NX_PPP *ppp_ptr)
{
    elog_w(TAG_PPP, "PPP link DOWN");
    dns_client_cleanup();
    tx_event_flags_set(&ppp_events, PPP_EVT_LINK_DOWN, TX_OR);
}

/**
  * @brief  PPP PAP login generator — returns empty credentials (no auth)
  */
static UINT ppp_generate_login(CHAR *name, CHAR *password)
{
    name[0] = '\0';
    password[0] = '\0';
    return NX_SUCCESS;
}

/**
  * @brief  PPP serial read thread
  * @note   Reads PPP bytes from ppp_serial (UART3 or CMUX DLCI 2)
  *         and feeds them to the NetX PPP parser.
  */
static void ppp_read_thread_entry(ULONG param)
{
    (void)param;

    /* Wait for serial port to be configured (race: thread starts before
     * app_netxduo_set_serial() is called) */
    while (!ppp_serial) {
        tx_thread_sleep(10);
    }

    elog_d(TAG_PPP, "PPP read thread started (serial: %s)", ppp_serial->name);

    /* NOTE: Do NOT flush the ring buffer here!
     * After ATD*99***1# returns CONNECT, the modem immediately starts sending
     * PPP LCP frames. Any stale bytes in the buffer are likely legitimate PPP
     * data that must be fed to nx_ppp_byte_receive(). Flushing would lose
     * the initial LCP Configure-Request, causing PPP negotiation to hang. */

    /* Drain any bytes captured during AT response parsing (e.g. the modem's
     * first LCP Configure-Request that arrived immediately after "CONNECT\r\n").
     * Without this, those bytes would be lost and PPP negotiation would stall
     * with LCP stuck in REQ_SENT state. */
    {
        uint16_t n_ov = a7683e_drain_overflow(ppp_rx_buf, PPP_RX_BATCH_SIZE);
        for (uint16_t i = 0; i < n_ov; i++)
        {
            nx_ppp_byte_receive(&ppp_0, ppp_rx_buf[i]);
        }
        if (n_ov > 0) {
            elog_d(TAG_PPP, "Drained %u overflow bytes", n_ov);
        }
    }

    while (1)
    {
        /* Block until at least one byte is available, then read up to PPP_RX_BATCH_SIZE */
        uint16_t n = ppp_serial->read(ppp_rx_buf, PPP_RX_BATCH_SIZE, TX_WAIT_FOREVER);

        /* Feed bytes to NetX PPP parser. The PPP Thread's do-while loop
         * (in nx_ppp.c) processes ALL pending packets per event, so feeding
         * multiple frames in one batch is safe. */
        for (uint16_t i = 0; i < n; i++)
        {
            nx_ppp_byte_receive(&ppp_0, ppp_rx_buf[i]);
        }
        #ifdef NX_PPP_DEBUG_LOG_ENABLE
            elog_hexdump("PPP RX", 16, ppp_rx_buf, n);
        #endif
    }
}

/**
  * @brief  NTP sync thread
  * @note   Waits for PPP link-up, then does DNS resolution + SNTP time sync
  */
static void ntp_thread_entry(ULONG param)
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

        /* Wait for PPP routing and carrier DNS to settle */
        tx_thread_sleep(5000);

        /* ---- Step 3: Resolve NTP server hostname (with retries) ---- */
        {
            UINT dns_retry;
            for (dns_retry = 0; dns_retry < 10; dns_retry++)
            {
                elog_d(TAG_DNS, "Resolving %s (attempt %u) ...", NTP_SERVER_HOST, dns_retry + 1);
                status = resolve_host(NTP_SERVER_HOST, &ntp_ip_address);
                if (status == NX_SUCCESS) break;
                elog_w(TAG_DNS, "DNS attempt %u failed: 0x%02X", dns_retry + 1, status);
                tx_thread_sleep(3000);
            }
        }
        if (status != NX_SUCCESS)
        {
            elog_w(TAG_DNS, "DNS resolution failed after 10 retries, using fallback NTP IP");
            ntp_ip_address = IP_ADDRESS(203, 107, 6, 88);
            elog_d(TAG_NTP, "NTP server: 203.107.6.88 (fallback)");
        }
        else
        {
            elog_d(TAG_NTP, "NTP server: %s = %lu.%lu.%lu.%lu",
                  NTP_SERVER_HOST,
                  (ntp_ip_address >> 24) & 0xFF,
                  (ntp_ip_address >> 16) & 0xFF,
                  (ntp_ip_address >> 8) & 0xFF,
                  ntp_ip_address & 0xFF);
        }

        /* ---- Step 5-6: SNTP time sync (with retries) ---- */
        {
            UINT sntp_retry;
            UINT sntp_ok = NX_FALSE;

            for (sntp_retry = 0; sntp_retry < 3; sntp_retry++)
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

                status = nx_sntp_client_initialize_unicast(&sntp_client, ntp_ip_address);
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
                      (ntp_ip_address >> 24) & 0xFF,
                      (ntp_ip_address >> 16) & 0xFF,
                      (ntp_ip_address >> 8) & 0xFF,
                      ntp_ip_address & 0xFF);

                status = nx_sntp_client_request_unicast_time(&sntp_client,
                                                              30 * NX_IP_PERIODIC_RATE);
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
                        uint32_t h = ((unix_time / 3600) + 8) % 24; /* UTC+8 */

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

            if (!sntp_ok)
            {
                elog_e(TAG_NTP, "NTP sync failed after %u attempts", sntp_retry);
            }
        }

        elog_d(TAG_NTP, "Time sync complete");

        /* Wait for next PPP link-down/up cycle */
        tx_event_flags_get(&ppp_events, PPP_EVT_LINK_DOWN, TX_OR_CLEAR,
                           &actual_flags, TX_WAIT_FOREVER);
    }
}

/**
  * @brief  Application NetXDuo Initialization.
  * @param  memory_ptr: pointer to the NX byte pool
  * @retval NX_SUCCESS on success
  */
UINT MX_NetXDuo_Init(VOID *memory_ptr)
{
    UINT status;
    TX_BYTE_POOL *byte_pool = (TX_BYTE_POOL *)memory_ptr;

    /* Initialize NetX system internals — MUST be called before any nx_* API */
    nx_system_initialize();

    /* USER CODE BEGIN MX_NetXDuo_MEM_POOL */
    /* USER CODE END MX_NetXDuo_MEM_POOL */

    /* ---- Allocate all memory from NX byte pool ---- */

    /* Packet pool memory: PACKET_POOL_SIZE bytes */
    netx_init_step = 1;
    status = tx_byte_allocate(byte_pool, &pool_memory_ptr,
                               PACKET_POOL_SIZE, TX_NO_WAIT);
    if (status != TX_SUCCESS) { netx_init_status = status; return status; }

    /* IP instance memory: IP_MEMORY_SIZE bytes */
    netx_init_step = 2;
    status = tx_byte_allocate(byte_pool, &ip_memory_ptr,
                               IP_MEMORY_SIZE, TX_NO_WAIT);
    if (status != TX_SUCCESS) { netx_init_status = status; return status; }

    /* PPP thread stack */
    netx_init_step = 3;
    status = tx_byte_allocate(byte_pool, &ppp_stack_ptr,
                               PPP_THREAD_STACK_SIZE, TX_NO_WAIT);
    if (status != TX_SUCCESS) { netx_init_status = status; return status; }

    /* PPP read thread stack */
    netx_init_step = 4;
    status = tx_byte_allocate(byte_pool, &ppp_read_stack_ptr,
                               PPP_READ_THREAD_STACK, TX_NO_WAIT);
    if (status != TX_SUCCESS) { netx_init_status = status; return status; }

    /* NTP sync thread stack */
    netx_init_step = 5;
    status = tx_byte_allocate(byte_pool, &ntp_stack_ptr,
                               NTP_THREAD_STACK, TX_NO_WAIT);
    if (status != TX_SUCCESS) { netx_init_status = status; return status; }

    /* TCP client thread stack */
    netx_init_step = 51;
    status = tx_byte_allocate(byte_pool, &tcp_client_stack_ptr,
                               TCP_CLIENT_THREAD_STACK, TX_NO_WAIT);
    if (status != TX_SUCCESS) { netx_init_status = status; return status; }

    /* UDP client thread stack */
    netx_init_step = 52;
    status = tx_byte_allocate(byte_pool, &udp_client_stack_ptr,
                               UDP_CLIENT_THREAD_STACK, TX_NO_WAIT);
    if (status != TX_SUCCESS) { netx_init_status = status; return status; }

    /* ARP cache memory */
    netx_init_step = 6;
    status = tx_byte_allocate(byte_pool, &arp_cache_ptr,
                               ARP_CACHE_SIZE, TX_NO_WAIT);
    if (status != TX_SUCCESS) { netx_init_status = status; return status; }

    /* ---- Create packet pool ---- */
    netx_init_step = 7;
    status = nx_packet_pool_create(&pool_0, "NetX Main Packet Pool",
                                    PACKET_PAYLOAD_SIZE, pool_memory_ptr, PACKET_POOL_SIZE);
    if (status != NX_SUCCESS) { netx_init_status = status; return status; }

    /* ---- Create event flags ---- */
    netx_init_step = 8;
    tx_event_flags_create(&ppp_events, "PPP Events");

    /* ---- Create DNS mutex ---- */
    tx_mutex_create(&dns_mutex, "DNS Mutex", TX_INHERIT);

    /* ---- Create PPP read thread ---- */
    netx_init_step = 9;
    status = tx_thread_create(&ppp_read_thread, "PPP Read",
                               ppp_read_thread_entry, 0,
                               ppp_read_stack_ptr, PPP_READ_THREAD_STACK,
                               PPP_READ_THREAD_PRIO, PPP_READ_THREAD_PRIO,
                               TX_NO_TIME_SLICE, TX_DONT_START);
    if (status != TX_SUCCESS) { netx_init_status = status; return status; }

    /* ---- Create NTP sync thread ---- */
    netx_init_step = 10;
    status = tx_thread_create(&ntp_thread, "NTP Sync",
                               ntp_thread_entry, 0,
                               ntp_stack_ptr, NTP_THREAD_STACK,
                               NTP_THREAD_PRIO, NTP_THREAD_PRIO,
                               TX_NO_TIME_SLICE, TX_AUTO_START);
    if (status != TX_SUCCESS) { netx_init_status = status; return status; }

    /* ---- Create TCP client thread ---- */
    netx_init_step = 11;
    status = tx_thread_create(&tcp_client_thread, "TCP Client",
                               tcp_client_thread_entry, 0,
                               tcp_client_stack_ptr, TCP_CLIENT_THREAD_STACK,
                               TCP_CLIENT_THREAD_PRIO, TCP_CLIENT_THREAD_PRIO,
                               TX_NO_TIME_SLICE, TX_AUTO_START);
    if (status != TX_SUCCESS) { netx_init_status = status; return status; }

    /* ---- Create UDP client thread ---- */
    netx_init_step = 12;
    status = tx_thread_create(&udp_client_thread, "UDP Client",
                               udp_client_thread_entry, 0,
                               udp_client_stack_ptr, UDP_CLIENT_THREAD_STACK,
                               UDP_CLIENT_THREAD_PRIO, UDP_CLIENT_THREAD_PRIO,
                               TX_NO_TIME_SLICE, TX_AUTO_START);
    if (status != TX_SUCCESS) { netx_init_status = status; return status; }

    /* NOTE: PPP and IP instance creation deferred to app_netxduo_create_ppp()
     * because nx_ppp_create requires a thread context, and IP must be created
     * after PPP so the PPP driver is ready. */

    /* USER CODE BEGIN MX_NetXDuo_Init */
    /* USER CODE END MX_NetXDuo_Init */

    return NX_SUCCESS;


}

/**
  * @brief  Create and configure PPP instance — must be called from a thread
  * @note   Separated from MX_NetXDuo_Init because nx_ppp_create requires
  *         a thread context (NX_THREADS_ONLY_CALLER_CHECKING).
  * @return NX_SUCCESS on success
  */
UINT app_netxduo_create_ppp(void)
{
    UINT status;

    if (ppp_created) return NX_SUCCESS;

    /* Reset PPP TX buffer state for new session */
    ppp_byte_send_reset();

    /* ---- Step 1: Create PPP instance (must be in thread context) ----
     * NOTE: nx_ppp_create initializes the PPP structure and associates it
     * with the IP instance. nx_ppp_driver (used in nx_ip_create) expects
     * the PPP instance to already exist. So PPP must be created first. */
    elog_d(TAG, "Creating PPP instance...");

    status = nx_ppp_create(&ppp_0, "PPP", &ip_0,
                            ppp_stack_ptr, PPP_THREAD_STACK_SIZE, 15,
                            &pool_0,
                            NX_NULL,
                            ppp_byte_send);
    if (status != NX_SUCCESS)
    {
        elog_e(TAG, "PPP create failed: 0x%02X", status);
        return status;
    }
    elog_d(TAG, "PPP instance created (id=0x%08lX)", (unsigned long)ppp_0.nx_ppp_id);

    /* ---- Step 2: Create IP instance (PPP driver is now ready) ---- */
    elog_d(TAG, "Creating IP instance...");

    status = nx_ip_create(&ip_0, "NetX IP Instance", IP_ADDRESS(0, 0, 0, 0),
                           IP_ADDRESS(0, 0, 0, 0), &pool_0,
                           nx_ppp_driver,
                           ip_memory_ptr, IP_MEMORY_SIZE, 1);
    if (status != NX_SUCCESS)
    {
        elog_e(TAG, "IP create failed: 0x%02X", status);
        return status;
    }

    /* Allow ip fragmentation */
    status = nx_ip_fragment_enable(&ip_0);

    /* Enable ARP (required by NetX even for PPP) */
    status = nx_arp_enable(&ip_0, arp_cache_ptr, ARP_CACHE_SIZE);
    if (status != NX_SUCCESS)
    {
        elog_e(TAG, "ARP enable failed: 0x%02X", status);
        return status;
    }

    /* Enable ICMP (for ping) */
    status = nx_icmp_enable(&ip_0);
    if (status != NX_SUCCESS)
    {
        elog_e(TAG, "ICMP enable failed: 0x%02X", status);
        return status;
    }

    /* Enable UDP (required for SNTP) */
    status = nx_udp_enable(&ip_0);
    if (status != NX_SUCCESS)
    {
        elog_e(TAG, "UDP enable failed: 0x%02X", status);
        return status;
    }

    /* Enable TCP (optional, useful for other services) */
    status = nx_tcp_enable(&ip_0);
    if (status != NX_SUCCESS)
    {
        elog_e(TAG, "TCP enable failed: 0x%02X", status);
        return status;
    }

    /* ---- Step 3: Configure PPP ---- */
    nx_ppp_link_up_notify(&ppp_0, ppp_link_up_callback);
    nx_ppp_link_down_notify(&ppp_0, ppp_link_down_callback);

    /* Let PPP negotiate IP addresses (0.0.0.0 = auto) */
    nx_ppp_ip_address_assign(&ppp_0, 0, 0);

    /* ---- Step 4: Start PPP read thread ----
     * After first creation (ppp_created==0): thread is in TX_DONT_START
     *   state → resume it.
     * After reconnect (ppp_created was reset, thread was deleted): recreate. */
    if (!ppp_created) {
        /* First time — thread was created with TX_DONT_START, resume it */
        status = tx_thread_resume(&ppp_read_thread);
    } else {
        /* Reconnect — thread was deleted, recreate it */
        status = tx_thread_create(&ppp_read_thread, "PPP Read",
                                   ppp_read_thread_entry, 0,
                                   ppp_read_stack_ptr, PPP_READ_THREAD_STACK,
                                   PPP_READ_THREAD_PRIO, PPP_READ_THREAD_PRIO,
                                   TX_NO_TIME_SLICE, TX_AUTO_START);
    }
    if (status != TX_SUCCESS)
    {
        elog_e(TAG, "Failed to start PPP read thread: 0x%02X", status);
        return status;
    }
    elog_d(TAG, "PPP read thread started");

    ppp_created = 1;
    elog_d(TAG, "PPP + IP instance ready");
    return NX_SUCCESS;
}

/**
  * @brief  Start PPP negotiation
  * @return NX_SUCCESS on success
  */
UINT app_netxduo_start_ppp(void)
{
    UINT status = nx_ppp_start(&ppp_0);
    if (status == NX_SUCCESS)
    {
        elog_d(TAG, "PPP negotiation started — waiting for link-up...");
    }
    else
    {
        elog_e(TAG, "nx_ppp_start failed: 0x%02X", status);
    }
    return status;
}

/**
  * @brief  Destroy PPP instance and release all associated resources.
  *         Call on USB disconnect to clean up before reconnection.
  *
  *  Teardown order:
  *   1. Set destroying flag (TCP/UDP/NTP skip network API calls)
  *   2. Terminate PPP read thread (stops feeding bytes to PPP)
  *   3. Stop PPP (triggers link-down callback → TCP/UDP/NTP wake up)
  *   4. Delete DNS client
  *   5. Delete PPP instance
  *   6. Delete IP instance
  *   7. Reset flags
  */
void app_netxduo_destroy_ppp(void)
{
    UINT status;
    elog_i(TAG, "=== Destroying PPP instance ===");

    /* 1. Signal teardown in progress — TCP/UDP threads check this
     *    before calling nx_tcp_socket_disconnect etc. */
    ppp_destroying = 1;

    /* 2. Terminate and delete the PPP read thread FIRST.
     *    It's blocked on ppp_serial->read() which won't return after USB removal.
     *    Must happen before nx_ppp_stop() to avoid feeding bytes to a
     *    transitioning PPP instance. */
    if (ppp_read_thread.tx_thread_id == 0x54485244UL) {
        elog_d(TAG, "Terminating PPP read thread...");
        tx_thread_terminate(&ppp_read_thread);
        tx_thread_delete(&ppp_read_thread);
        elog_d(TAG, "PPP read thread terminated");
    }

    /* 3. Stop PPP — triggers ppp_link_down_callback which sets PPP_EVT_LINK_DOWN,
     *    causing TCP/UDP/NTP threads to clean up and wait for next link-up */
    if (ppp_0.nx_ppp_id == NX_PPP_ID) {
        elog_d(TAG, "Stopping PPP...");
        status = nx_ppp_stop(&ppp_0);
        elog_d(TAG, "PPP stop: 0x%02X", status);
    }

    /* 4. Delete DNS client if active */
    elog_d(TAG, "Cleaning up DNS client...");
    dns_client_cleanup();

    /* 5. Delete PPP instance */
    if (ppp_0.nx_ppp_id == NX_PPP_ID) {
        status = nx_ppp_delete(&ppp_0);
        elog_d(TAG, "PPP delete: 0x%02X", status);
    }

    /* 6. Delete IP instance */
    if (ip_0.nx_ip_id == NX_IP_ID) {
        status = nx_ip_delete(&ip_0);
        elog_d(TAG, "IP delete: 0x%02X", status);
    }

    /* 7. Reset flags so app_netxduo_create_ppp() can run again */
    ppp_created = 0;
    ppp_destroying = 0;

    elog_i(TAG, "=== PPP instance destroyed — ready for reconnect ===");
}

/**
  * @brief  Signal that USB modem has been disconnected.
  *         Called from USBX host event callback.
  *         Idempotent — safe to call multiple times for the same disconnect.
  */
void app_netxduo_set_reconnect_event(void)
{
    /* Lazy-init the event flags (first call) */
    if (!modem_events_created) {
        tx_event_flags_create(&modem_events, "Modem Events");
        modem_events_created = 1;
    }
    tx_event_flags_set(&modem_events, MODEM_EVT_USB_DISCONNECT, TX_OR);
    elog_w(TAG, ">> USB disconnect event signaled");
}

/**
  * @brief  Wait for USB disconnect event (blocking).
  *         Call from modem thread after PPP is running.
  */
void app_netxduo_wait_disconnect(void)
{
    ULONG actual_flags;
    if (!modem_events_created) {
        tx_event_flags_create(&modem_events, "Modem Events");
        modem_events_created = 1;
    }
    tx_event_flags_get(&modem_events, MODEM_EVT_USB_DISCONNECT, TX_OR_CLEAR,
                       &actual_flags, TX_WAIT_FOREVER);
}

/* USER CODE BEGIN 1 */

/**
  * @brief  Initialize or re-initialize the global DNS client.
  *         Creates the DNS client (once), configures timeout/retries,
  *         and adds DNS servers from PPP IPCP or fallback.
  *         Safe to call multiple times — only initializes once.
  *         Mutex-protected against concurrent callers.
  * @return NX_SUCCESS on success
  */
static UINT dns_client_setup(void)
{
    UINT status;

    /* Mutex prevents concurrent create from NTP/TCP/UDP threads */
    tx_mutex_get(&dns_mutex, TX_WAIT_FOREVER);

    if (dns_client_created) {
        tx_mutex_put(&dns_mutex);
        return NX_SUCCESS;
    }

    NX_IP *active_ip = app_netxduo_get_active_ip();
    if (active_ip == NULL) {
        tx_mutex_put(&dns_mutex);
        return NX_NOT_ENABLED;
    }

    /* Check PPP instance validity */
    if (ppp_0.nx_ppp_id != NX_PPP_ID) {
        elog_e(TAG_DNS, "PPP instance not initialized");
        tx_mutex_put(&dns_mutex);
        return NX_PTR_ERROR;
    }

    /* Check PPP state — IPCP must be completed for DNS addresses to be available */
    if (ppp_0.nx_ppp_state != NX_PPP_ESTABLISHED) {
        elog_w(TAG_DNS, "PPP not established (state=%d), deferring DNS init",
               ppp_0.nx_ppp_state);
        tx_mutex_put(&dns_mutex);
        return NX_NOT_ENABLED;
    }

    /* Create DNS client */
    status = nx_dns_create(&dns_client, active_ip, (UCHAR *)"DNS Client");
    if (status != NX_SUCCESS) {
        elog_e(TAG_DNS, "DNS client create failed: 0x%02X", status);
        tx_mutex_put(&dns_mutex);
        return status;
    }

    /* Configure retries (timeout is per-query, set in nx_dns_host_by_name_get call).
     * NOTE: nx_dns_retries is set directly because NetX DNS has no setter API.
     * With 2 DNS servers and 15s timeout: worst case = 4 × 2 × 15s = 120s. */
    dns_client.nx_dns_retries = 4;  /* 4 retries per DNS server */

    /* Add DNS servers from PPP IPCP negotiation (carrier-assigned) */
    ULONG dns1 = 0, dns2 = 0;
    UINT dns1_rc = nx_ppp_dns_address_get(&ppp_0, &dns1);
    UINT dns2_rc = nx_ppp_secondary_dns_address_get(&ppp_0, &dns2);

    if (dns1_rc == NX_SUCCESS) nx_dns_server_add(&dns_client, dns1);
    if (dns2_rc == NX_SUCCESS) nx_dns_server_add(&dns_client, dns2);

    /* Fallback DNS if carrier didn't provide any */
    if (dns1_rc != NX_SUCCESS && dns2_rc != NX_SUCCESS) {
        nx_dns_server_add(&dns_client, IP_ADDRESS(8, 8, 8, 8));
        nx_dns_server_add(&dns_client, IP_ADDRESS(114, 114, 114, 114));
        elog_w(TAG_DNS, "No carrier DNS (rc=%u/%u), using fallback 8.8.8.8 / 114.114.114.114",
               dns1_rc, dns2_rc);
    } else {
        elog_i(TAG_DNS, "DNS servers: %lu.%lu.%lu.%lu, %lu.%lu.%lu.%lu",
              (dns1 >> 24) & 0xFF, (dns1 >> 16) & 0xFF,
              (dns1 >> 8)  & 0xFF,  dns1 & 0xFF,
              (dns2 >> 24) & 0xFF, (dns2 >> 16) & 0xFF,
              (dns2 >> 8)  & 0xFF,  dns2 & 0xFF);
    }

    /* Log IP routing info for debugging */
    ULONG ip_addr = 0, mask = 0, gw = 0;
    nx_ip_address_get(active_ip, &ip_addr, &mask);
    nx_ip_gateway_address_get(active_ip, &gw);
    UINT link_up = 0;
    if (ppp_0.nx_ppp_interface_ptr != NX_NULL)
        link_up = ppp_0.nx_ppp_interface_ptr->nx_interface_link_up;
    elog_d(TAG_DNS, "IP: %lu.%lu.%lu.%lu  GW: %lu.%lu.%lu.%lu  link_up: %d  ppp_state: %d",
          (ip_addr >> 24) & 0xFF, (ip_addr >> 16) & 0xFF,
          (ip_addr >> 8)  & 0xFF,  ip_addr & 0xFF,
          (gw >> 24) & 0xFF, (gw >> 16) & 0xFF,
          (gw >> 8)  & 0xFF,  gw & 0xFF,
          link_up,
          ppp_0.nx_ppp_state);

    dns_client_created = 1;
    elog_i(TAG_DNS, "DNS client initialized");
    tx_mutex_put(&dns_mutex);
    return NX_SUCCESS;
}

/**
  * @brief  Destroy the global DNS client (call on PPP link-down).
  *         Mutex-protected against concurrent resolve_host() callers.
  */
static void dns_client_cleanup(void)
{
    tx_mutex_get(&dns_mutex, TX_WAIT_FOREVER);
    if (dns_client_created) {
        nx_dns_delete(&dns_client);
        dns_client_created = 0;
        elog_d(TAG_DNS, "DNS client destroyed");
    }
    tx_mutex_put(&dns_mutex);
}

/**
  * @brief  Resolve hostname or parse IP address string.
  *         If host is a dotted-decimal IP (e.g. "39.105.97.3"), parse directly.
  *         Otherwise, do DNS lookup via the global DNS client.
  * @param  host        Hostname or IP string
  * @param  ip_address  Output: resolved IP address in network byte order
  * @return NX_SUCCESS on success
  */
static UINT resolve_host(const char *host, ULONG *ip_address)
{
    /* Try to parse as dotted-decimal IP first */
    const char *p = host;
    ULONG addr = 0;
    int   octets = 0;

    while (*p && octets < 4)
    {
        int val = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9')
        {
            val = val * 10 + (*p - '0');
            p++;
            digits++;
        }
        if (digits == 0 || val > 255) break;
        addr = (addr << 8) | val;
        octets++;
        if (*p == '.') p++;
        else if (*p == '\0' && octets == 4) /* ok */;
        else break;
    }

    if (octets == 4 && *p == '\0')
    {
        /* Successfully parsed as IP address */
        *ip_address = addr;
        return NX_SUCCESS;
    }

    /* Ensure DNS client is initialized */
    if (!dns_client_created) {
        UINT rc = dns_client_setup();
        if (rc != NX_SUCCESS) return rc;
    }

    /* Hold mutex during DNS query to prevent dns_client_cleanup() from
     * deleting the client mid-query (PPP link-down race). */
    tx_mutex_get(&dns_mutex, TX_WAIT_FOREVER);

    if (!dns_client_created) {
        tx_mutex_put(&dns_mutex);
        return NX_NOT_ENABLED;
    }

    UINT status = nx_dns_host_by_name_get(&dns_client, (UCHAR *)host,
                                           ip_address, 15 * NX_IP_PERIODIC_RATE);
    if (status != NX_SUCCESS) {
        elog_e(TAG_DNS, "DNS query failed: 0x%02X (host=%s) ppp_state=%d",
               status, host, ppp_0.nx_ppp_state);
    }

    tx_mutex_put(&dns_mutex);
    return status;
}

/**
  * @brief  TCP client thread — connects to server, periodically sends data and prints replies
  * @note   Restarts on PPP link-down/up cycle (re-resolves, re-creates socket)
  */
static void tcp_client_thread_entry(ULONG param)
{
    (void)param;
    ULONG actual_flags;
    uint8_t rx_buf[CLIENT_RX_BUF_SIZE];

    while (1)
    {
        /* Wait for PPP link-up */
        elog_d(TAG_TCP, "Waiting for PPP link-up...");
        tx_event_flags_get(&ppp_events, PPP_EVT_LINK_UP, TX_OR_CLEAR,
                           &actual_flags, TX_WAIT_FOREVER);
        tx_thread_sleep(1000);  /* Let routing settle */

        NX_TCP_SOCKET socket;
        NX_PACKET *packet_ptr;
        UINT status;
        ULONG server_ip;

        /* Resolve server address (retry up to 5 times with backoff) */
        {
            UINT dns_retries;
            for (dns_retries = 0; dns_retries < 5; dns_retries++) {
                status = resolve_host(TCP_SERVER_HOST, &server_ip);
                if (status == NX_SUCCESS) break;
                elog_w(TAG_TCP, "Resolve attempt %u failed: 0x%02X", dns_retries + 1, status);
                tx_thread_sleep(2000 * (dns_retries + 1));
            }
        }
        if (status != NX_SUCCESS)
        {
            elog_e(TAG_TCP, "Resolve '%s' failed after 5 attempts: 0x%02X", TCP_SERVER_HOST, status);
            goto tcp_wait_link_down;
        }

        elog_d(TAG_TCP, "Server: %s = %lu.%lu.%lu.%lu:%d", TCP_SERVER_HOST,
              (server_ip >> 24) & 0xFF, (server_ip >> 16) & 0xFF,
              (server_ip >> 8) & 0xFF, server_ip & 0xFF, TCP_SERVER_PORT);

        while (1)
        {
            /* Check for PPP link-down (non-blocking) */
            status = tx_event_flags_get(&ppp_events, PPP_EVT_LINK_DOWN, TX_OR_CLEAR,
                                        &actual_flags, NX_NO_WAIT);
            if (status == TX_SUCCESS)
            {
                elog_w(TAG_TCP, "PPP link-down, cleaning up TCP");
                goto tcp_cleanup_socket;
            }

            /* Create TCP socket */
            {
                NX_IP *tcp_ip = app_netxduo_get_active_ip();
                NX_PACKET_POOL *tcp_pool = app_netxduo_get_active_pool();
                if (tcp_ip == NULL || tcp_pool == NULL) { tx_thread_sleep(CLIENT_SEND_PERIOD); continue; }
                status = nx_tcp_socket_create(tcp_ip, &socket, "TCP Client",
                                              NX_IP_NORMAL, NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE, 2048,
                                              NX_NULL, NX_NULL);
                if (status != NX_SUCCESS)
                {
                    elog_e(TAG_TCP, "Socket create failed: 0x%02X", status);
                    tx_thread_sleep(CLIENT_SEND_PERIOD);
                    continue;
                }
            }

            status = nx_tcp_client_socket_bind(&socket, NX_ANY_PORT, NX_IP_PERIODIC_RATE);
            if (status != NX_SUCCESS)
            {
                elog_e(TAG_TCP, "Bind failed: 0x%02X", status);
                nx_tcp_socket_delete(&socket);
                tx_thread_sleep(CLIENT_SEND_PERIOD);
                continue;
            }

            status = nx_tcp_client_socket_connect(&socket, server_ip, TCP_SERVER_PORT,
                                                  5 * NX_IP_PERIODIC_RATE);
            if (status != NX_SUCCESS)
            {
                elog_e(TAG_TCP, "Connect failed: 0x%02X", status);
                nx_tcp_client_socket_unbind(&socket);
                nx_tcp_socket_delete(&socket);
                tx_thread_sleep(CLIENT_SEND_PERIOD);
                continue;
            }

            elog_d(TAG_TCP, "Connected!");

            while (1)
            {
                /* Check for PPP link-down (non-blocking) */
                status = tx_event_flags_get(&ppp_events, PPP_EVT_LINK_DOWN, TX_OR_CLEAR,
                                            &actual_flags, NX_NO_WAIT);
                if (status == TX_SUCCESS)
                {
                    elog_w(TAG_TCP, "PPP link-down during session");
                    goto tcp_cleanup_socket;
                }

                /* Send data */
                NX_IP *tcp_ip = app_netxduo_get_active_ip();
                NX_PACKET_POOL *tcp_pool = app_netxduo_get_active_pool();
                if (tcp_ip == NULL || tcp_pool == NULL) break;

                const char *msg = "Hello from STM32 TCP\r\n";
                status = nx_packet_allocate(tcp_pool, &packet_ptr, NX_TCP_PACKET, NX_WAIT_FOREVER);
                if (status != NX_SUCCESS)
                {
                    elog_e(TAG_TCP, "Packet alloc failed: 0x%02X", status);
                    break;
                }
                status = nx_packet_data_append(packet_ptr, (void *)msg, strlen(msg),
                                               tcp_pool, NX_WAIT_FOREVER);
                if (status != NX_SUCCESS)
                {
                    elog_e(TAG_TCP, "Packet append failed: 0x%02X", status);
                    nx_packet_release(packet_ptr);
                    break;
                }
                status = nx_tcp_socket_send(&socket, packet_ptr, NX_IP_PERIODIC_RATE);
                if (status != NX_SUCCESS)
                {
                    elog_e(TAG_TCP, "Send failed: 0x%02X", status);
                    nx_packet_release(packet_ptr);
                    break;
                }
                elog_d(TAG_TCP, "TX: %s", msg);

                /* Receive response */
                status = nx_tcp_socket_receive(&socket, &packet_ptr, 2 * NX_IP_PERIODIC_RATE);
                if (status == NX_SUCCESS)
                {
                    ULONG len = packet_ptr->nx_packet_length;
                    if (len > CLIENT_RX_BUF_SIZE - 1) len = CLIENT_RX_BUF_SIZE - 1;
                    memcpy(rx_buf, packet_ptr->nx_packet_prepend_ptr, len);
                    rx_buf[len] = '\0';
                    elog_d(TAG_TCP, "RX [%lu]: %s", len, rx_buf);
                    nx_packet_release(packet_ptr);
                }
                else if (status == NX_NO_PACKET)
                {
                    /* Timeout — no data received, just continue */
                }
                else
                {
                    elog_w(TAG_TCP, "Receive error: 0x%02X", status);
                    break;
                }

                tx_thread_sleep(CLIENT_SEND_PERIOD);
            }

tcp_cleanup_socket:
            /* Disconnect and cleanup — skip if PPP is being destroyed
             * (IP instance may already be deleted by destroy_ppp) */
            if (!ppp_destroying) {
                nx_tcp_socket_disconnect(&socket, NX_IP_PERIODIC_RATE);
                nx_tcp_client_socket_unbind(&socket);
                nx_tcp_socket_delete(&socket);
            } else {
                elog_w(TAG_TCP, "PPP destroying — skipping socket cleanup");
            }

            /* If link-down triggered cleanup, wait for next link-up cycle */
            if (actual_flags & PPP_EVT_LINK_DOWN) goto tcp_wait_link_down;

            elog_w(TAG_TCP, "Disconnected, reconnecting...");
            tx_thread_sleep(1000);
        }

tcp_wait_link_down:
        /* Wait for link-down event if not already received */
        tx_event_flags_get(&ppp_events, PPP_EVT_LINK_DOWN, TX_OR_CLEAR,
                           &actual_flags, TX_WAIT_FOREVER);
    }
}

/**
  * @brief  UDP client thread — periodically sends data to server and prints replies
  * @note   Restarts on PPP link-down/up cycle (re-resolves, re-creates socket)
  */
static void udp_client_thread_entry(ULONG param)
{
    (void)param;
    ULONG actual_flags;
    uint8_t rx_buf[CLIENT_RX_BUF_SIZE];

    while (1)
    {
        /* Wait for PPP link-up */
        elog_d(TAG_UDP, "Waiting for PPP link-up...");
        tx_event_flags_get(&ppp_events, PPP_EVT_LINK_UP, TX_OR_CLEAR,
                           &actual_flags, TX_WAIT_FOREVER);
        tx_thread_sleep(1500);  /* Let routing settle, stagger after TCP */

        NX_UDP_SOCKET socket;
        NX_PACKET *packet_ptr;
        UINT status;
        ULONG server_ip;

        /* Resolve server address (retry up to 5 times with backoff) */
        {
            UINT dns_retries;
            for (dns_retries = 0; dns_retries < 5; dns_retries++) {
                status = resolve_host(UDP_SERVER_HOST, &server_ip);
                if (status == NX_SUCCESS) break;
                elog_w(TAG_UDP, "Resolve attempt %u failed: 0x%02X", dns_retries + 1, status);
                tx_thread_sleep(2000 * (dns_retries + 1));
            }
        }
        if (status != NX_SUCCESS)
        {
            elog_e(TAG_UDP, "Resolve '%s' failed after 5 attempts: 0x%02X", UDP_SERVER_HOST, status);
            goto udp_wait_link_down;
        }

        elog_d(TAG_UDP, "Server: %s = %lu.%lu.%lu.%lu:%d", UDP_SERVER_HOST,
              (server_ip >> 24) & 0xFF, (server_ip >> 16) & 0xFF,
              (server_ip >> 8) & 0xFF, server_ip & 0xFF, UDP_SERVER_PORT);

        /* Create UDP socket */
        {
            NX_IP *udp_ip = app_netxduo_get_active_ip();
            NX_PACKET_POOL *udp_pool = app_netxduo_get_active_pool();
            if (udp_ip == NULL || udp_pool == NULL) { goto udp_wait_link_down; }
            status = nx_udp_socket_create(udp_ip, &socket, "UDP Client",
                                          NX_IP_NORMAL, NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE, 512);
            if (status != NX_SUCCESS)
            {
                elog_e(TAG_UDP, "Socket create failed: 0x%02X", status);
                goto udp_wait_link_down;
            }
        }

        status = nx_udp_socket_bind(&socket, NX_ANY_PORT, NX_IP_PERIODIC_RATE);
        if (status != NX_SUCCESS)
        {
            elog_e(TAG_UDP, "Bind failed: 0x%02X", status);
            nx_udp_socket_delete(&socket);
            goto udp_wait_link_down;
        }

        while (1)
        {
            /* Check for PPP link-down (non-blocking) */
            status = tx_event_flags_get(&ppp_events, PPP_EVT_LINK_DOWN, TX_OR_CLEAR,
                                        &actual_flags, NX_NO_WAIT);
            if (status == TX_SUCCESS)
            {
                elog_w(TAG_UDP, "PPP link-down, cleaning up UDP");
                break;
            }

            /* Send data */
            NX_IP *udp_ip = app_netxduo_get_active_ip();
            NX_PACKET_POOL *udp_pool = app_netxduo_get_active_pool();
            if (udp_ip == NULL || udp_pool == NULL) break;

            const char *msg = "Hello from STM32 UDP\r\n";
            status = nx_packet_allocate(udp_pool, &packet_ptr, NX_UDP_PACKET, NX_WAIT_FOREVER);
            if (status != NX_SUCCESS)
            {
                elog_e(TAG_UDP, "Packet alloc failed: 0x%02X", status);
                tx_thread_sleep(CLIENT_SEND_PERIOD);
                continue;
            }
            status = nx_packet_data_append(packet_ptr, (void *)msg, strlen(msg),
                                           udp_pool, NX_WAIT_FOREVER);
            if (status != NX_SUCCESS)
            {
                elog_e(TAG_UDP, "Packet append failed: 0x%02X", status);
                nx_packet_release(packet_ptr);
                tx_thread_sleep(CLIENT_SEND_PERIOD);
                continue;
            }
            status = nx_udp_socket_send(&socket, packet_ptr, server_ip, UDP_SERVER_PORT);
            if (status != NX_SUCCESS)
            {
                elog_e(TAG_UDP, "Send failed: 0x%02X", status);
                nx_packet_release(packet_ptr);
            }
            else
            {
                elog_d(TAG_UDP, "TX: %s", msg);
            }

            /* Receive response */
            status = nx_udp_socket_receive(&socket, &packet_ptr, 2 * NX_IP_PERIODIC_RATE);
            if (status == NX_SUCCESS)
            {
                ULONG len = packet_ptr->nx_packet_length;
                if (len > CLIENT_RX_BUF_SIZE - 1) len = CLIENT_RX_BUF_SIZE - 1;
                memcpy(rx_buf, packet_ptr->nx_packet_prepend_ptr, len);
                rx_buf[len] = '\0';
                elog_d(TAG_UDP, "RX [%lu]: %s", len, rx_buf);
                nx_packet_release(packet_ptr);
            }
            else if (status == NX_NO_PACKET)
            {
                /* Timeout — no response */
            }
            else
            {
                elog_w(TAG_UDP, "Receive error: 0x%02X", status);
            }

            tx_thread_sleep(CLIENT_SEND_PERIOD);
        }

        /* Cleanup socket before next cycle — skip if PPP is being destroyed */
        if (!ppp_destroying) {
            nx_udp_socket_unbind(&socket);
            nx_udp_socket_delete(&socket);
        } else {
            elog_w(TAG_UDP, "PPP destroying — skipping socket cleanup");
        }

udp_wait_link_down:
        /* Wait for link-down event if not already received */
        tx_event_flags_get(&ppp_events, PPP_EVT_LINK_DOWN, TX_OR_CLEAR,
                           &actual_flags, TX_WAIT_FOREVER);
    }
}

/**
  * @brief  DHCP thread — waits for ECM link up, starts DHCP, logs IP
  */
static void ecm_dhcp_thread_entry(ULONG param)
{
    UINT status;
    ULONG ip_address, network_mask, gateway;
    UINT retries = 0;

    (void)param;

    /* Wait for the actual CDC-ECM USB link to come up.
     * NOTE: nx_ip_status_check(NX_IP_LINK_ENABLED) returns immediately because
     * NX_LINK_ENABLE is called during nx_ip_create() and sets
     * nx_interface_link_up = TRUE before the actual USB link is established.
     * Instead, we poll the USB network driver's link status, which is set to
     * NX_TRUE only after _ux_network_driver_link_up() is called by the CDC-ECM
     * thread when it receives the NETWORK_CONNECTION interrupt notification. */
    elog_i(TAG_ECM, "Waiting for ECM link...");

    retries = 0;
    while (retries < 600)  /* up to 600 × 100ms = 60s */
    {
        /* Get the USB network device through the interface's additional_link_info.
         * This is set by _ux_network_driver_entry(NX_LINK_INTERFACE_ATTACH). */
        USB_NETWORK_DEVICE_TYPE *usb_dev =
            (USB_NETWORK_DEVICE_TYPE *)ecm_ip.nx_ip_interface[0].nx_interface_additional_link_info;

        if (usb_dev != NX_NULL && usb_dev->ux_network_device_usb_link_up == NX_TRUE)
        {
            elog_i(TAG_ECM, "ECM link is up (after %u.%us)", retries / 10, retries % 10);
            break;
        }

        tx_thread_sleep(NX_IP_PERIODIC_RATE / 10);  /* 100ms */
        retries++;
    }

    if (retries >= 600)
    {
        elog_e(TAG_ECM, "ECM link did not come up within 60s");
        return;
    }

    /* Small settle delay — let the CDC-ECM thread finish its initialization */
    tx_thread_sleep(NX_IP_PERIODIC_RATE / 10);  /* 100ms */

    /* Start DHCP client */
    status = nx_dhcp_start(&ecm_dhcp);
    if (status != NX_SUCCESS)
    {
        elog_e(TAG_ECM, "DHCP start failed: 0x%02X", status);
        return;
    }
    elog_i(TAG_ECM, "DHCP started, waiting for IP address...");

    /* Poll for an address — DHCP state changes from SELECTING → REQUESTING → BOUND */
    retries = 0;
    while (retries < 120)  /* up to 120 × 500ms = 60s */
    {
        tx_thread_sleep(NX_IP_PERIODIC_RATE / 2);  /* 500ms */

        nx_ip_address_get(&ecm_ip, &ip_address, &network_mask);
        if (ip_address != 0)
        {
            ULONG dns_server;
            /* Got an IP! */
            elog_i(TAG_ECM, "ECM IP: %lu.%lu.%lu.%lu",
                   (ip_address >> 24) & 0xFF, (ip_address >> 16) & 0xFF,
                   (ip_address >> 8) & 0xFF, ip_address & 0xFF);
            elog_i(TAG_ECM, "ECM Mask: %lu.%lu.%lu.%lu",
                   (network_mask >> 24) & 0xFF, (network_mask >> 16) & 0xFF,
                   (network_mask >> 8) & 0xFF, network_mask & 0xFF);

            /* Try to get gateway */
            nx_ip_gateway_address_get(&ecm_ip, &gateway);
            if (gateway != 0)
            {
                elog_i(TAG_ECM, "ECM GW: %lu.%lu.%lu.%lu",
                       (gateway >> 24) & 0xFF, (gateway >> 16) & 0xFF,
                       (gateway >> 8) & 0xFF, gateway & 0xFF);
            }

            /* Try to get DNS server from DHCP */
            {
                UINT dns_size = sizeof(dns_server);
                if (nx_dhcp_interface_user_option_retrieve(&ecm_dhcp, 0,
                        NX_DHCP_OPTION_DNS_SVR, (UCHAR *)&dns_server, &dns_size) == NX_SUCCESS)
                {
                    elog_i(TAG_ECM, "ECM DNS: %lu.%lu.%lu.%lu",
                           (dns_server >> 24) & 0xFF, (dns_server >> 16) & 0xFF,
                           (dns_server >> 8) & 0xFF, dns_server & 0xFF);
                }
            }

            ecm_ip_logged = 1;
            return;
        }
        retries++;
    }

    elog_w(TAG_ECM, "DHCP: no IP address after 60s");
}

/**
  * @brief  Initialize CDC-ECM IP instance and start DHCP
  * @note   Called from usbx_app_thread_entry after USB device connect.
  *         Idempotent — only runs once. Can be called before or after
  *         CDC-ECM link-up; the DHCP thread will wait for link.
  * @return NX_SUCCESS on success
  */
UINT app_netxduo_ecm_init(void)
{
    UINT status;

    if (ecm_ip_created) return NX_SUCCESS;

    elog_i(TAG_ECM, "Creating ECM IP instance...");

    /* Create dedicated packet pool for ECM */
    status = nx_packet_pool_create(&ecm_pool, "ECM Packet Pool",
                                    ECM_PACKET_PAYLOAD,
                                    (VOID *)ecm_pool_memory,
                                    ECM_PACKET_POOL_SIZE);
    if (status != NX_SUCCESS)
    {
        elog_e(TAG_ECM, "ECM pool create failed: 0x%02X", status);
        return status;
    }

    /* Set low watermark to avoid starvation */
    nx_packet_pool_low_watermark_set(&ecm_pool, 4);

    /* Create IP instance with the USBX network driver */
    status = nx_ip_create(&ecm_ip, "ECM IP Instance",
                          IP_ADDRESS(0, 0, 0, 0),
                          IP_ADDRESS(255, 255, 255, 0),
                          &ecm_pool,
                          _ux_network_driver_entry,
                          (VOID *)ecm_ip_memory, ECM_IP_MEMORY_SIZE, 1);
    if (status != NX_SUCCESS)
    {
        elog_e(TAG_ECM, "ECM IP create failed: 0x%02X", status);
        return status;
    }

    /* Enable ARP */
    status = nx_arp_enable(&ecm_ip, (VOID *)ecm_arp_cache, ECM_ARP_CACHE_SIZE);
    if (status != NX_SUCCESS)
    {
        elog_e(TAG_ECM, "ECM ARP enable failed: 0x%02X", status);
        return status;
    }

    /* Enable UDP */
    status = nx_udp_enable(&ecm_ip);
    if (status != NX_SUCCESS)
    {
        elog_e(TAG_ECM, "ECM UDP enable failed: 0x%02X", status);
        return status;
    }

    /* Enable ICMP */
    nx_icmp_enable(&ecm_ip);

    /* Enable IGMP */
    nx_igmp_enable(&ecm_ip);

    /* Enable IP fragment */
    nx_ip_fragment_enable(&ecm_ip);

    /* Enable TCP */
    nx_tcp_enable(&ecm_ip);

    /* Create DHCP thread */
    status = tx_thread_create(&ecm_dhcp_thread, "ECM DHCP",
                               ecm_dhcp_thread_entry, 0,
                               ecm_dhcp_stack, ECM_DHCP_THREAD_STACK,
                               ECM_DHCP_THREAD_PRIO, ECM_DHCP_THREAD_PRIO,
                               TX_NO_TIME_SLICE, TX_AUTO_START);
    if (status != TX_SUCCESS)
    {
        elog_e(TAG_ECM, "DHCP thread create failed: 0x%02X", status);
        /* Non-fatal — ECM IP is created, just no auto-IP */
    }

    ecm_ip_created = 1;
    elog_i(TAG_ECM, "ECM IP instance created");
    return NX_SUCCESS;
}

/**
  * @brief  Destroy CDC-ECM IP instance and release resources.
  *         Call on USB disconnect to prevent resource leaks on reconnect.
  */
void app_netxduo_ecm_destroy(void)
{
    if (!ecm_ip_created) return;

    elog_i(TAG_ECM, "Destroying ECM instance...");

    nx_dhcp_stop(&ecm_dhcp);
    nx_dhcp_delete(&ecm_dhcp);
    elog_d(TAG_ECM, "DHCP stopped");

    nx_ip_delete(&ecm_ip);
    elog_d(TAG_ECM, "IP deleted");

    nx_packet_pool_delete(&ecm_pool);
    elog_d(TAG_ECM, "Packet pool deleted");

    ecm_ip_created = 0;
    ecm_ip_logged  = 0;

    elog_i(TAG_ECM, "ECM instance destroyed");
}

/* USER CODE END 1 */
