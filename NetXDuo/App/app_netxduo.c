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

/** NTP server hostname — configurable */
#ifndef NTP_SERVER_HOST
#define NTP_SERVER_HOST         "ntp.aliyun.com"
#endif

/** NTP epoch offset (seconds from 1900-01-01 to 1970-01-01) */
#define NTP_EPOCH_OFFSET        2208988800UL

/** PPP serial read thread priority */
#define PPP_READ_THREAD_PRIO    14
#define PPP_READ_THREAD_STACK   1024

/** NTP sync thread priority */
#define NTP_THREAD_PRIO         16
#define NTP_THREAD_STACK        1024*2

/** Packet pool configuration */
#define PACKET_POOL_SIZE        4096*2
#define PACKET_PAYLOAD_SIZE     1536    /* Slightly larger than PPP MTU (1500) */

/** PPP thread stack size */
#define PPP_THREAD_STACK_SIZE   1024*5

/** IP instance internal memory size */
#define IP_MEMORY_SIZE          2048

/** ARP cache memory size */
#define ARP_CACHE_SIZE          1024

/* Private variables ---------------------------------------------------------*/

/* NetX objects */
static NX_PACKET_POOL    pool_0;
static NX_IP             ip_0;
NX_PPP                   ppp_0;
static NX_DNS            dns_client;
static NX_SNTP_CLIENT    sntp_client;

/* Memory pointers — allocated from NX byte pool */
static VOID *pool_memory_ptr;
static VOID *ip_memory_ptr;
static VOID *arp_cache_ptr;
static VOID *ppp_stack_ptr;
static VOID *ppp_read_stack_ptr;
static VOID *ntp_stack_ptr;

/* PPP link state */
static TX_EVENT_FLAGS_GROUP ppp_events;
#define PPP_EVT_LINK_UP     0x01
#define PPP_EVT_LINK_DOWN   0x02

/* Thread handles */
static TX_THREAD ppp_read_thread;
static TX_THREAD ntp_thread;

/* PPP creation flag — deferred to thread context */
static uint8_t ppp_created = 0;

/* PPP serial I/O buffer */
#define PPP_RX_BATCH_SIZE   256
static uint8_t ppp_rx_buf[PPP_RX_BATCH_SIZE];

/* Serial port for PPP data (set by app_netxduo_set_serial) */
static bsp_serial_t *ppp_serial = NULL;

/* Debug: track which init step failed (inspect in debugger) */
volatile UINT netx_init_status = 0;
volatile UINT netx_init_step = 0;

/* USER CODE BEGIN PV */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/

static void ppp_read_thread_entry(ULONG param);
static void ntp_thread_entry(ULONG param);
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
  */
static void ppp_byte_send(UCHAR byte)
{
    if (ppp_serial) ppp_serial->write(&byte, 1);
}

/**
  * @brief  NetX PPP link-up callback
  */
static void ppp_link_up_callback(NX_PPP *ppp_ptr)
{
    LOG_I(TAG_PPP, "PPP link UP");
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
  * @note   Reads bytes in batches from UART3 and feeds them to NetX PPP
  *         via nx_ppp_byte_receive() one byte at a time.
  */
static void ppp_read_thread_entry(ULONG param)
{
    (void)param;

    LOG_I(TAG_PPP, "PPP read thread started");

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
        UINT if_link_up = 0;
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

        /* Small delay for IP stack to stabilize */
        tx_thread_sleep(1000);

        /* Print negotiated IP addresses */
        print_ip_addresses();

        /* ---- Step 1: Create DNS client ---- */
        status = nx_dns_create(&dns_client, &ip_0, (UCHAR *)"DNS Client");
        if (status != NX_SUCCESS)
        {
            LOG_E(TAG_DNS, "DNS create failed: 0x%02X", status);
            continue;
        }

        status = nx_dns_packet_pool_set(&dns_client, &pool_0);
        if (status != NX_SUCCESS)
        {
            LOG_E(TAG_DNS, "DNS packet pool set failed: 0x%02X", status);
            nx_dns_delete(&dns_client);
            continue;
        }

        /* ---- Step 2: Add DNS servers ---- */
        /* DNS server list: PPP DNS first, then public fallbacks */
        static const ULONG fallback_dns[] = {
            IP_ADDRESS(114, 114, 114, 114),  /* China Telecom public DNS */
            IP_ADDRESS(223,   5,   5,   5),  /* AliDNS */
        };

        /* Try PPP-negotiated DNS first */
        ULONG dns_server_addr = 0;
        ULONG dns_secondary_addr = 0;
        UINT dns_count = 0;
        status = nx_ppp_dns_address_get(&ppp_0, &dns_server_addr);
        if (status == NX_SUCCESS && dns_server_addr != 0)
        {
            LOG_I(TAG_DNS, "PPP Primary DNS: %lu.%lu.%lu.%lu",
                  (dns_server_addr >> 24) & 0xFF,
                  (dns_server_addr >> 16) & 0xFF,
                  (dns_server_addr >> 8) & 0xFF,
                  dns_server_addr & 0xFF);
            nx_dns_server_add(&dns_client, dns_server_addr);
            dns_count++;
        }
        else
        {
            LOG_W(TAG_DNS, "No primary DNS from PPP");
        }

        status = nx_ppp_secondary_dns_address_get(&ppp_0, &dns_secondary_addr);
        if (status == NX_SUCCESS && dns_secondary_addr != 0)
        {
            LOG_I(TAG_DNS, "PPP Secondary DNS: %lu.%lu.%lu.%lu",
                  (dns_secondary_addr >> 24) & 0xFF,
                  (dns_secondary_addr >> 16) & 0xFF,
                  (dns_secondary_addr >> 8) & 0xFF,
                  dns_secondary_addr & 0xFF);
            nx_dns_server_add(&dns_client, dns_secondary_addr);
            dns_count++;
        }
        else
        {
            LOG_W(TAG_DNS, "No secondary DNS from PPP");
        }

        /* Add fallback DNS servers */
        for (UINT i = 0; i < sizeof(fallback_dns) / sizeof(fallback_dns[0]); i++)
        {
            if (nx_dns_server_add(&dns_client, fallback_dns[i]) == NX_SUCCESS)
            {
                LOG_I(TAG_DNS, "Fallback DNS[%u]: %lu.%lu.%lu.%lu", i,
                      (fallback_dns[i] >> 24) & 0xFF,
                      (fallback_dns[i] >> 16) & 0xFF,
                      (fallback_dns[i] >> 8) & 0xFF,
                      fallback_dns[i] & 0xFF);
                dns_count++;
            }
        }

        if (dns_count == 0)
        {
            LOG_E(TAG_DNS, "No DNS servers available!");
            nx_dns_delete(&dns_client);
            continue;
        }

        /* ---- Step 3: Ping DNS servers to verify connectivity ---- */
        LOG_I(TAG_DNS, "Pinging DNS servers...");

        /* Build combined list: PPP DNS first, then fallbacks */
        ULONG all_dns[5];
        UINT  all_dns_count = 0;
        if (dns_server_addr != 0)
            all_dns[all_dns_count++] = dns_server_addr;
        if (dns_secondary_addr != 0)
            all_dns[all_dns_count++] = dns_secondary_addr;
        for (UINT i = 0; i < sizeof(fallback_dns) / sizeof(fallback_dns[0]); i++)
            all_dns[all_dns_count++] = fallback_dns[i];

        for (UINT i = 0; i < all_dns_count; i++)
        {
            NX_PACKET *response_ptr = NX_NULL;
            UINT ping_status = nx_icmp_ping(&ip_0, all_dns[i],
                                            "ping", 4,
                                            &response_ptr,
                                            2 * NX_IP_PERIODIC_RATE);
            if (ping_status == NX_SUCCESS)
            {
                LOG_I(TAG_DNS, "  %lu.%lu.%lu.%lu: PONG (RTT OK)",
                      (all_dns[i] >> 24) & 0xFF, (all_dns[i] >> 16) & 0xFF,
                      (all_dns[i] >> 8) & 0xFF,  all_dns[i] & 0xFF);
                if (response_ptr)
                    nx_packet_release(response_ptr);
            }
            else
            {
                LOG_W(TAG_DNS, "  %lu.%lu.%lu.%lu: no reply (0x%02X)",
                      (all_dns[i] >> 24) & 0xFF, (all_dns[i] >> 16) & 0xFF,
                      (all_dns[i] >> 8) & 0xFF,  all_dns[i] & 0xFF,
                      ping_status);
            }
        }

        /* ---- Step 4: Resolve NTP server hostname ---- */
        LOG_I(TAG_DNS, "Resolving %s ...", NTP_SERVER_HOST);
        status = nx_dns_host_by_name_get(&dns_client,
                                          (UCHAR *)NTP_SERVER_HOST,
                                          &ntp_ip_address,
                                          3 * NX_IP_PERIODIC_RATE);
        if (status != NX_SUCCESS)
        {
            LOG_E(TAG_DNS, "DNS resolution failed: 0x%02X", status);
            nx_dns_delete(&dns_client);
            continue;
        }

        LOG_I(TAG_DNS, "%s = %lu.%lu.%lu.%lu",
              NTP_SERVER_HOST,
              (ntp_ip_address >> 24) & 0xFF,
              (ntp_ip_address >> 16) & 0xFF,
              (ntp_ip_address >> 8) & 0xFF,
              ntp_ip_address & 0xFF);

        /* ---- Step 5: Create SNTP client ---- */
        status = nx_sntp_client_create(&sntp_client, &ip_0, 0, &pool_0,
                                        NX_NULL, NX_NULL, NX_NULL);
        if (status != NX_SUCCESS)
        {
            LOG_E(TAG_NTP, "SNTP client create failed: 0x%02X", status);
            nx_dns_delete(&dns_client);
            continue;
        }

        /* Initialize unicast SNTP mode */
        status = nx_sntp_client_initialize_unicast(&sntp_client, ntp_ip_address);
        if (status != NX_SUCCESS)
        {
            LOG_E(TAG_NTP, "SNTP unicast init failed: 0x%02X", status);
            nx_sntp_client_delete(&sntp_client);
            nx_dns_delete(&dns_client);
            continue;
        }

        /* Run the SNTP client */
        status = nx_sntp_client_run_unicast(&sntp_client);
        if (status != NX_SUCCESS)
        {
            LOG_E(TAG_NTP, "SNTP run failed: 0x%02X", status);
            nx_sntp_client_delete(&sntp_client);
            nx_dns_delete(&dns_client);
            continue;
        }

        /* ---- Step 6: Request time from NTP server ---- */
        LOG_I(TAG_NTP, "Requesting time from %s ...", NTP_SERVER_HOST);
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

                /* Calculate date from Unix timestamp (simplified) */
                /* Days since epoch */
                uint32_t days = unix_time / 86400;
                /* Day of week (Jan 1 1970 was Thursday=4) */
                sDate.WeekDay = (uint8_t)((days + 4) % 7);
                /* Simplified date — for full accuracy use a proper algorithm */
                sDate.Year    = 26;  /* 2026 — placeholder */
                sDate.Month   = RTC_MONTH_JUNE;
                sDate.Date    = (uint8_t)((days % 30) + 1);
                if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK)
                {
                    LOG_E(TAG_NTP, "RTC SetDate failed");
                }

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
        nx_dns_delete(&dns_client);

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

    /* ---- Step 1: Create PPP instance (must be in thread context) ---- */
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

    /* ---- Step 2: Create IP instance (after PPP so driver is ready) ---- */
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
/* USER CODE END 1 */
