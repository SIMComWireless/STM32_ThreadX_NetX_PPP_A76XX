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
#include "bsp_serial.h"
#include "main.h"
#include "rtc.h"
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

/** PPP serial read thread priority */
#define PPP_READ_THREAD_PRIO    14
#define PPP_READ_THREAD_STACK   1024

/** NTP sync thread priority */
#define NTP_THREAD_PRIO         16
#define NTP_THREAD_STACK        1024*2

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

/** TCP/UDP client thread priorities */
#define TCP_CLIENT_THREAD_PRIO  18
#define TCP_CLIENT_THREAD_STACK 1024*3
#define UDP_CLIENT_THREAD_PRIO  19
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
static UINT ppp_generate_login(CHAR *name, CHAR *password);
static void nx_driver_stm32_ppp(NX_IP_DRIVER *driver_req_ptr);
static void ppp_byte_send(UCHAR byte);

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
  *         Skips the opening 0x7E, buffers frame data, flushes on closing 0x7E.
  */
static void ppp_byte_send(UCHAR byte)
{
    static uint8_t buf[256];
    static uint16_t pos = 0;
    static uint8_t in_escape = 0;

    if (!ppp_serial) return;

    /* 0x7D = PPP control-escape prefix, next byte is XOR 0x20 */
    if (byte == 0x7D)
    {
        in_escape = 1;
        if (pos < sizeof(buf)) buf[pos++] = byte;
        return;
    }
    if (in_escape)
    {
        in_escape = 0;
        if (pos < sizeof(buf)) buf[pos++] = byte;
        return;
    }

    /* 0x7E = PPP frame flag (start/end) */
    if (byte == 0x7E)
    {
        if (pos == 0)
        {
            /* Opening 0x7E — start of frame, skip it */
            return;
        }
        /* Closing 0x7E — append and flush the complete frame */
        if (pos < sizeof(buf)) buf[pos++] = byte;
        ppp_serial->write(buf, pos);
        pos = 0;
    }
    else
    {
        if (pos < sizeof(buf)) buf[pos++] = byte;
        /* Safety: flush if buffer full (shouldn't happen for normal PPP frames) */
        if (pos >= sizeof(buf))
        {
            ppp_serial->write(buf, pos);
            pos = 0;
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

    /* ---- IPv4 ---- */
    ULONG ipv4_addr = 0, ipv4_mask = 0;
    status = nx_ip_address_get(&ip_0, &ipv4_addr, &ipv4_mask);
    if (status == NX_SUCCESS && ipv4_addr != 0)
    {
        LOG_I(TAG_PPP, "IPv4: %lu.%lu.%lu.%lu  Mask: %lu.%lu.%lu.%lu",
              (ipv4_addr >> 24) & 0xFF, (ipv4_addr >> 16) & 0xFF,
              (ipv4_addr >> 8)  & 0xFF,  ipv4_addr & 0xFF,
              (ipv4_mask >> 24) & 0xFF, (ipv4_mask >> 16) & 0xFF,
              (ipv4_mask >> 8)  & 0xFF,  ipv4_mask & 0xFF);

        ULONG gw = 0;
        nx_ip_gateway_address_get(&ip_0, &gw);
        LOG_I(TAG_PPP, "Gateway: %lu.%lu.%lu.%lu",
              (gw >> 24) & 0xFF, (gw >> 16) & 0xFF,
              (gw >> 8)  & 0xFF,  gw & 0xFF);

        /* Interface status */
        ULONG if_link_up = 0;
        nx_ip_interface_status_check(&ip_0, 0, NX_IP_LINK_ENABLED,
                                     &if_link_up, NX_NO_WAIT);
        LOG_I(TAG_PPP, "Interface: %s, ARP: %s, MTU: %lu",
              if_link_up ? "UP" : "DOWN",
              ip_0.nx_ip_interface[0].nx_interface_address_mapping_needed ? "yes" : "no",
              (unsigned long)ip_0.nx_ip_interface[0].nx_interface_ip_mtu_size);
    }
    else
    {
        LOG_W(TAG_PPP, "IPv4: not assigned");
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

            status = nxd_ipv6_address_get(&ip_0, i, &nxd_addr, &prefix_len, &if_idx);
            if (status == NX_SUCCESS && nxd_addr.nxd_ip_address.v6[0] != 0)
            {
                LOG_I(TAG_PPP, "IPv6[%u]: %04lX:%04lX:%04lX:%04lX:%04lX:%04lX:%04lX:%04lX /%lu (if=%u)",
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
            LOG_W(TAG_PPP, "IPv6: not assigned");
        }
    }
#else
    LOG_D(TAG_PPP, "IPv6: disabled (NX_DISABLE_IPV6)");
#endif /* FEATURE_NX_IPV6 */
}

/**
  * @brief  NetX PPP link-up callback
  */
static void ppp_link_up_callback(NX_PPP *ppp_ptr)
{
    LOG_I(TAG_PPP, "PPP link UP");
    /* Print negotiated IP addresses */
    print_ip_addresses();
    tx_event_flags_set(&ppp_events, PPP_EVT_LINK_UP, TX_OR);
}

/**
  * @brief  NetX PPP link-down callback
  */
static void ppp_link_down_callback(NX_PPP *ppp_ptr)
{
    LOG_W(TAG_PPP, "PPP link DOWN");
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
  * @brief  PPP network driver stub — PPP handles I/O via ppp_byte_send callback
  * @note   NetX requires a driver function for nx_ip_create, but the PPP addon
  *         manages its own serial I/O through the byte-send callback.
  */
static void nx_driver_stm32_ppp(NX_IP_DRIVER *driver_req_ptr)
{
    switch (driver_req_ptr->nx_ip_driver_command)
    {
    case NX_LINK_INTERFACE_ATTACH:
    case NX_LINK_INITIALIZE:
    case NX_LINK_ENABLE:
    case NX_LINK_DISABLE:
        driver_req_ptr->nx_ip_driver_status = NX_SUCCESS;
        break;

    default:
        driver_req_ptr->nx_ip_driver_status = NX_UNHANDLED_COMMAND;
        break;
    }
}

/**
  * @brief  PPP serial read thread
  * @note   Reads PPP bytes from ppp_serial (UART3 or CMUX DLCI 2)
  *         and feeds them to the NetX PPP parser.
  */
static void ppp_read_thread_entry(ULONG param)
{
    (void)param;

    if (!ppp_serial) {
        LOG_E(TAG_PPP, "PPP read thread: no serial port configured!");
        return;
    }

    LOG_I(TAG_PPP, "PPP read thread started (serial: %s)", ppp_serial->name);

    /* NOTE: Do NOT flush the ring buffer here!
     * After ATD*99***1# returns CONNECT, the modem immediately starts sending
     * PPP LCP frames. Any stale bytes in the buffer are likely legitimate PPP
     * data that must be fed to nx_ppp_byte_receive(). Flushing would lose
     * the initial LCP Configure-Request, causing PPP negotiation to hang. */

    while (1)
    {
        /* Block until at least one byte is available, then read up to PPP_RX_BATCH_SIZE */
        uint16_t n = ppp_serial->read(ppp_rx_buf, PPP_RX_BATCH_SIZE, TX_WAIT_FOREVER);
        for (uint16_t i = 0; i < n; i++)
        {
            nx_ppp_byte_receive(&ppp_0, ppp_rx_buf[i]);
        }
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
        LOG_I(TAG_NTP, "Waiting for PPP link...");
        tx_event_flags_get(&ppp_events, PPP_EVT_LINK_UP, TX_OR_CLEAR,
                           &actual_flags, TX_WAIT_FOREVER);

        LOG_I(TAG_NTP, "PPP link is up, starting time sync...");

        /* ---- Step 3: Resolve NTP server hostname ---- */
        LOG_I(TAG_DNS, "Resolving %s ...", NTP_SERVER_HOST);
        status = resolve_host(NTP_SERVER_HOST, &ntp_ip_address);
        if (status != NX_SUCCESS)
        {
            LOG_W(TAG_DNS, "DNS resolution failed: 0x%02X, using fallback IP", status);
            ntp_ip_address = IP_ADDRESS(203, 107, 6, 88);  /* ntp.aliyun.com fallback */
            LOG_I(TAG_NTP, "NTP server: 203.107.6.88 (fallback IP)");
        }
        else
        {
            LOG_I(TAG_NTP, "NTP server: %s = %lu.%lu.%lu.%lu",
                  NTP_SERVER_HOST,
                  (ntp_ip_address >> 24) & 0xFF,
                  (ntp_ip_address >> 16) & 0xFF,
                  (ntp_ip_address >> 8) & 0xFF,
                  ntp_ip_address & 0xFF);
        }

        /* ---- Step 5: Create SNTP client ---- */
        status = nx_sntp_client_create(&sntp_client, &ip_0, 0, &pool_0,
                                        NX_NULL, NX_NULL, NX_NULL);
        if (status != NX_SUCCESS)
        {
            LOG_E(TAG_NTP, "SNTP client create failed: 0x%02X", status);
            continue;
        }

        /* Initialize unicast SNTP mode */
        status = nx_sntp_client_initialize_unicast(&sntp_client, ntp_ip_address);
        if (status != NX_SUCCESS)
        {
            LOG_E(TAG_NTP, "SNTP unicast init failed: 0x%02X", status);
            nx_sntp_client_delete(&sntp_client);
            continue;
        }

        /* Run the SNTP client */
        status = nx_sntp_client_run_unicast(&sntp_client);
        if (status != NX_SUCCESS)
        {
            LOG_E(TAG_NTP, "SNTP run failed: 0x%02X", status);
            nx_sntp_client_delete(&sntp_client);
            continue;
        }

        /* ---- Step 6: Request time from NTP server ---- */
        LOG_I(TAG_NTP, "Requesting time from %lu.%lu.%lu.%lu ...",
              (ntp_ip_address >> 24) & 0xFF,
              (ntp_ip_address >> 16) & 0xFF,
              (ntp_ip_address >> 8) & 0xFF,
              ntp_ip_address & 0xFF);
        status = nx_sntp_client_request_unicast_time(&sntp_client,
                                                      10 * NX_IP_PERIODIC_RATE);
        if (status == NX_SUCCESS)
        {
            ULONG seconds, fraction;
            status = nx_sntp_client_get_local_time(&sntp_client, &seconds, &fraction, NX_NULL);
            if (status == NX_SUCCESS)
            {
                /* Convert NTP epoch (1900) to Unix epoch (1970) */
                uint32_t unix_time = seconds - NTP_EPOCH_OFFSET;

                LOG_I(TAG_NTP, "NTP time received!");
                LOG_I(TAG_NTP, "Unix timestamp: %lu", (unsigned long)unix_time);

                /* ---- Step 7: Set RTC ---- */
                /* Calculate human-readable time */
                uint32_t s = unix_time % 60;
                uint32_t m = (unix_time / 60) % 60;
                uint32_t h = ((unix_time / 3600) + 8) % 24; /* UTC+8 for China */

                LOG_I(TAG_NTP, "Time (UTC+8): %02lu:%02lu:%02lu", h, m, s);

                /* Set the RTC */
                RTC_TimeTypeDef sTime = {0};
                RTC_DateTypeDef sDate = {0};

                sTime.Hours   = (uint8_t)h;
                sTime.Minutes = (uint8_t)m;
                sTime.Seconds = (uint8_t)s;
                if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK)
                {
                    LOG_E(TAG_NTP, "RTC SetTime failed");
                }

                /* Calculate date from Unix timestamp */
                uint32_t rtc_year, rtc_month, rtc_day;
                unix_to_ymd(unix_time, &rtc_year, &rtc_month, &rtc_day);

                /* Day of week (Jan 1 1970 was Thursday=4) */
                uint32_t days = unix_time / 86400;
                sDate.WeekDay = (uint8_t)((days + 4) % 7);
                sDate.Year    = (uint8_t)(rtc_year - 2000);
                sDate.Month   = (uint8_t)rtc_month;
                sDate.Date    = (uint8_t)rtc_day;
                if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK)
                {
                    LOG_E(TAG_NTP, "RTC SetDate failed");
                }

                LOG_I(TAG_NTP, "Date (UTC+8): %04lu-%02lu-%02lu", rtc_year, rtc_month, rtc_day);

                LOG_I(TAG_NTP, "RTC synchronized successfully!");
            }
            else
            {
                LOG_E(TAG_NTP, "Get local time failed: 0x%02X", status);
            }
        }
        else
        {
            LOG_E(TAG_NTP, "NTP request failed: 0x%02X", status);
        }

        /* Cleanup */
        nx_sntp_client_stop(&sntp_client);
        nx_sntp_client_delete(&sntp_client);

        LOG_I(TAG_NTP, "Time sync complete");

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

    /* ---- Step 1: Create PPP instance (must be in thread context) ----
     * NOTE: nx_ppp_create initializes the PPP structure and associates it
     * with the IP instance. nx_ppp_driver (used in nx_ip_create) expects
     * the PPP instance to already exist. So PPP must be created first. */
    LOG_I(TAG, "Creating PPP instance...");

    status = nx_ppp_create(&ppp_0, "PPP", &ip_0,
                            ppp_stack_ptr, PPP_THREAD_STACK_SIZE, 15,
                            &pool_0,
                            NX_NULL,
                            ppp_byte_send);
    if (status != NX_SUCCESS)
    {
        LOG_E(TAG, "PPP create failed: 0x%02X", status);
        return status;
    }
    LOG_I(TAG, "PPP instance created (id=0x%08lX)", (unsigned long)ppp_0.nx_ppp_id);

    /* ---- Step 2: Create IP instance (PPP driver is now ready) ---- */
    LOG_I(TAG, "Creating IP instance...");

    status = nx_ip_create(&ip_0, "NetX IP Instance", IP_ADDRESS(0, 0, 0, 0),
                           IP_ADDRESS(0, 0, 0, 0), &pool_0,
                           nx_ppp_driver,
                           ip_memory_ptr, IP_MEMORY_SIZE, 1);
    if (status != NX_SUCCESS)
    {
        LOG_E(TAG, "IP create failed: 0x%02X", status);
        return status;
    }

    /* Allow ip fragmentation */
    status = nx_ip_fragment_enable(&ip_0);

    /* Enable ARP (required by NetX even for PPP) */
    status = nx_arp_enable(&ip_0, arp_cache_ptr, ARP_CACHE_SIZE);
    if (status != NX_SUCCESS)
    {
        LOG_E(TAG, "ARP enable failed: 0x%02X", status);
        return status;
    }

    /* Enable ICMP (for ping) */
    status = nx_icmp_enable(&ip_0);
    if (status != NX_SUCCESS)
    {
        LOG_E(TAG, "ICMP enable failed: 0x%02X", status);
        return status;
    }

    /* Enable UDP (required for SNTP) */
    status = nx_udp_enable(&ip_0);
    if (status != NX_SUCCESS)
    {
        LOG_E(TAG, "UDP enable failed: 0x%02X", status);
        return status;
    }

    /* Enable TCP (optional, useful for other services) */
    status = nx_tcp_enable(&ip_0);
    if (status != NX_SUCCESS)
    {
        LOG_E(TAG, "TCP enable failed: 0x%02X", status);
        return status;
    }

    /* ---- Step 3: Configure PPP ---- */
    nx_ppp_link_up_notify(&ppp_0, ppp_link_up_callback);
    nx_ppp_link_down_notify(&ppp_0, ppp_link_down_callback);

    /* Let PPP negotiate IP addresses (0.0.0.0 = auto) */
    nx_ppp_ip_address_assign(&ppp_0, 0, 0);

    /* ---- Step 4: Resume PPP read thread ---- */
    status = tx_thread_resume(&ppp_read_thread);
    if (status != TX_SUCCESS)
    {
        LOG_E(TAG, "Failed to resume PPP read thread: 0x%02X", status);
        return status;
    }
    LOG_I(TAG, "PPP read thread resumed");

    ppp_created = 1;
    LOG_I(TAG, "PPP + IP instance ready");
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
        LOG_I(TAG, "PPP negotiation started — waiting for link-up...");
    }
    else
    {
        LOG_E(TAG, "nx_ppp_start failed: 0x%02X", status);
    }
    return status;
}

/* USER CODE BEGIN 1 */

/**
  * @brief  Resolve hostname or parse IP address string.
  *         If host is a dotted-decimal IP (e.g. "39.105.97.3"), parse directly.
  *         Otherwise, do DNS lookup via PPP DNS servers.
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

    /* Not an IP address — resolve via DNS */
    static NX_DNS dns_resolve;
    UINT status;

    status = nx_dns_create(&dns_resolve, &ip_0, (UCHAR *)"Resolver");
    if (status != NX_SUCCESS) return status;

    /* Add DNS servers from PPP IPCP negotiation (carrier-assigned) */
    ULONG dns1 = 0, dns2 = 0;
    nx_ppp_dns_address_get(&ppp_0, &dns1);
    nx_ppp_secondary_dns_address_get(&ppp_0, &dns2);

    if (dns1) nx_dns_server_add(&dns_resolve, dns1);
    if (dns2) nx_dns_server_add(&dns_resolve, dns2);

    /* Fallback DNS if carrier didn't provide any */
    if (!dns1 && !dns2) {
        nx_dns_server_add(&dns_resolve, IP_ADDRESS(8, 8, 8, 8));
        nx_dns_server_add(&dns_resolve, IP_ADDRESS(114, 114, 114, 114));
        LOG_W(TAG_DNS, "No carrier DNS, using fallback 8.8.8.8 / 114.114.114.114");
    } else {
        LOG_I(TAG_DNS, "DNS servers: %lu.%lu.%lu.%lu, %lu.%lu.%lu.%lu",
              (dns1 >> 24) & 0xFF, (dns1 >> 16) & 0xFF,
              (dns1 >> 8)  & 0xFF,  dns1 & 0xFF,
              (dns2 >> 24) & 0xFF, (dns2 >> 16) & 0xFF,
              (dns2 >> 8)  & 0xFF,  dns2 & 0xFF);
    }

    /* Log IP routing info for debugging */
    ULONG ip_addr = 0, mask = 0, gw = 0;
    nx_ip_address_get(&ip_0, &ip_addr, &mask);
    nx_ip_gateway_address_get(&ip_0, &gw);
    LOG_D(TAG_DNS, "IP: %lu.%lu.%lu.%lu  GW: %lu.%lu.%lu.%lu",
          (ip_addr >> 24) & 0xFF, (ip_addr >> 16) & 0xFF,
          (ip_addr >> 8)  & 0xFF,  ip_addr & 0xFF,
          (gw >> 24) & 0xFF, (gw >> 16) & 0xFF,
          (gw >> 8)  & 0xFF,  gw & 0xFF);

    status = nx_dns_host_by_name_get(&dns_resolve, (UCHAR *)host,
                                      ip_address, 5 * NX_IP_PERIODIC_RATE);
    if (status != NX_SUCCESS) {
        LOG_E(TAG_DNS, "DNS query failed: 0x%02X (host=%s)", status, host);
    }
    nx_dns_delete(&dns_resolve);
    return status;
}

/**
  * @brief  TCP client thread — connects to server, periodically sends data and prints replies
  */
static void tcp_client_thread_entry(ULONG param)
{
    (void)param;
    ULONG actual_flags;
    uint8_t rx_buf[CLIENT_RX_BUF_SIZE];

    /* Wait for PPP link-up */
    LOG_I(TAG_TCP, "Waiting for PPP link-up...");
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
            LOG_W(TAG_TCP, "Resolve attempt %u failed: 0x%02X", dns_retries + 1, status);
            tx_thread_sleep(2000 * (dns_retries + 1));
        }
    }
    if (status != NX_SUCCESS)
    {
        LOG_E(TAG_TCP, "Resolve '%s' failed after 5 attempts: 0x%02X", TCP_SERVER_HOST, status);
        return;
    }

    LOG_I(TAG_TCP, "Server: %s = %lu.%lu.%lu.%lu:%d", TCP_SERVER_HOST,
          (server_ip >> 24) & 0xFF, (server_ip >> 16) & 0xFF,
          (server_ip >> 8) & 0xFF, server_ip & 0xFF, TCP_SERVER_PORT);

    while (1)
    {
        /* Create TCP socket */
        status = nx_tcp_socket_create(&ip_0, &socket, "TCP Client",
                                      NX_IP_NORMAL, NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE, 2048,
                                      NX_NULL, NX_NULL);
        if (status != NX_SUCCESS)
        {
            LOG_E(TAG_TCP, "Socket create failed: 0x%02X", status);
            tx_thread_sleep(CLIENT_SEND_PERIOD);
            continue;
        }

        status = nx_tcp_client_socket_bind(&socket, NX_ANY_PORT, NX_IP_PERIODIC_RATE);
        if (status != NX_SUCCESS)
        {
            LOG_E(TAG_TCP, "Bind failed: 0x%02X", status);
            nx_tcp_socket_delete(&socket);
            tx_thread_sleep(CLIENT_SEND_PERIOD);
            continue;
        }

        status = nx_tcp_client_socket_connect(&socket, server_ip, TCP_SERVER_PORT,
                                              5 * NX_IP_PERIODIC_RATE);
        if (status != NX_SUCCESS)
        {
            LOG_E(TAG_TCP, "Connect failed: 0x%02X", status);
            nx_tcp_client_socket_unbind(&socket);
            nx_tcp_socket_delete(&socket);
            tx_thread_sleep(CLIENT_SEND_PERIOD);
            continue;
        }

        LOG_I(TAG_TCP, "Connected!");

        while (1)
        {
            /* Send data */
            const char *msg = "Hello from STM32 TCP\r\n";
            status = nx_packet_allocate(&pool_0, &packet_ptr, NX_TCP_PACKET, NX_WAIT_FOREVER);
            if (status != NX_SUCCESS)
            {
                LOG_E(TAG_TCP, "Packet alloc failed: 0x%02X", status);
                break;
            }
            status = nx_packet_data_append(packet_ptr, (void *)msg, strlen(msg),
                                           &pool_0, NX_WAIT_FOREVER);
            if (status != NX_SUCCESS)
            {
                LOG_E(TAG_TCP, "Packet append failed: 0x%02X", status);
                nx_packet_release(packet_ptr);
                break;
            }
            status = nx_tcp_socket_send(&socket, packet_ptr, NX_IP_PERIODIC_RATE);
            if (status != NX_SUCCESS)
            {
                LOG_E(TAG_TCP, "Send failed: 0x%02X", status);
                nx_packet_release(packet_ptr);
                break;
            }
            LOG_D(TAG_TCP, "TX: %s", msg);

            /* Receive response */
            status = nx_tcp_socket_receive(&socket, &packet_ptr, 2 * NX_IP_PERIODIC_RATE);
            if (status == NX_SUCCESS)
            {
                ULONG len = packet_ptr->nx_packet_length;
                if (len > CLIENT_RX_BUF_SIZE - 1) len = CLIENT_RX_BUF_SIZE - 1;
                memcpy(rx_buf, packet_ptr->nx_packet_prepend_ptr, len);
                rx_buf[len] = '\0';
                LOG_I(TAG_TCP, "RX [%lu]: %s", len, rx_buf);
                nx_packet_release(packet_ptr);
            }
            else if (status == NX_NO_PACKET)
            {
                /* Timeout — no data received, just continue */
            }
            else
            {
                LOG_W(TAG_TCP, "Receive error: 0x%02X", status);
                break;
            }

            tx_thread_sleep(CLIENT_SEND_PERIOD);
        }

        /* Disconnect and cleanup */
        nx_tcp_socket_disconnect(&socket, NX_IP_PERIODIC_RATE);
        nx_tcp_client_socket_unbind(&socket);
        nx_tcp_socket_delete(&socket);
        LOG_W(TAG_TCP, "Disconnected, reconnecting...");
        tx_thread_sleep(1000);
    }
}

/**
  * @brief  UDP client thread — periodically sends data to server and prints replies
  */
static void udp_client_thread_entry(ULONG param)
{
    (void)param;
    ULONG actual_flags;
    uint8_t rx_buf[CLIENT_RX_BUF_SIZE];

    /* Wait for PPP link-up */
    LOG_I(TAG_UDP, "Waiting for PPP link-up...");
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
            LOG_W(TAG_UDP, "Resolve attempt %u failed: 0x%02X", dns_retries + 1, status);
            tx_thread_sleep(2000 * (dns_retries + 1));
        }
    }
    if (status != NX_SUCCESS)
    {
        LOG_E(TAG_UDP, "Resolve '%s' failed after 5 attempts: 0x%02X", UDP_SERVER_HOST, status);
        return;
    }

    LOG_I(TAG_UDP, "Server: %s = %lu.%lu.%lu.%lu:%d", UDP_SERVER_HOST,
          (server_ip >> 24) & 0xFF, (server_ip >> 16) & 0xFF,
          (server_ip >> 8) & 0xFF, server_ip & 0xFF, UDP_SERVER_PORT);

    /* Create UDP socket */
    status = nx_udp_socket_create(&ip_0, &socket, "UDP Client",
                                  NX_IP_NORMAL, NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE, 512);
    if (status != NX_SUCCESS)
    {
        LOG_E(TAG_UDP, "Socket create failed: 0x%02X", status);
        return;
    }

    status = nx_udp_socket_bind(&socket, NX_ANY_PORT, NX_IP_PERIODIC_RATE);
    if (status != NX_SUCCESS)
    {
        LOG_E(TAG_UDP, "Bind failed: 0x%02X", status);
        nx_udp_socket_delete(&socket);
        return;
    }

    while (1)
    {
        /* Send data */
        const char *msg = "Hello from STM32 UDP\r\n";
        status = nx_packet_allocate(&pool_0, &packet_ptr, NX_UDP_PACKET, NX_WAIT_FOREVER);
        if (status != NX_SUCCESS)
        {
            LOG_E(TAG_UDP, "Packet alloc failed: 0x%02X", status);
            tx_thread_sleep(CLIENT_SEND_PERIOD);
            continue;
        }
        status = nx_packet_data_append(packet_ptr, (void *)msg, strlen(msg),
                                       &pool_0, NX_WAIT_FOREVER);
        if (status != NX_SUCCESS)
        {
            LOG_E(TAG_UDP, "Packet append failed: 0x%02X", status);
            nx_packet_release(packet_ptr);
            tx_thread_sleep(CLIENT_SEND_PERIOD);
            continue;
        }
        status = nx_udp_socket_send(&socket, packet_ptr, server_ip, UDP_SERVER_PORT);
        if (status != NX_SUCCESS)
        {
            LOG_E(TAG_UDP, "Send failed: 0x%02X", status);
            nx_packet_release(packet_ptr);
        }
        else
        {
            LOG_D(TAG_UDP, "TX: %s", msg);
        }

        /* Receive response */
        status = nx_udp_socket_receive(&socket, &packet_ptr, 2 * NX_IP_PERIODIC_RATE);
        if (status == NX_SUCCESS)
        {
            ULONG len = packet_ptr->nx_packet_length;
            if (len > CLIENT_RX_BUF_SIZE - 1) len = CLIENT_RX_BUF_SIZE - 1;
            memcpy(rx_buf, packet_ptr->nx_packet_prepend_ptr, len);
            rx_buf[len] = '\0';
            LOG_I(TAG_UDP, "RX [%lu]: %s", len, rx_buf);
            nx_packet_release(packet_ptr);
        }
        else if (status == NX_NO_PACKET)
        {
            /* Timeout — no response */
        }
        else
        {
            LOG_W(TAG_UDP, "Receive error: 0x%02X", status);
        }

        tx_thread_sleep(CLIENT_SEND_PERIOD);
    }
}

/* USER CODE END 1 */
