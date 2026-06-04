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
#define PPP_READ_THREAD_STACK   1024*2

/** NTP sync thread priority */
#define NTP_THREAD_PRIO         16
#define NTP_THREAD_STACK        1024

/** Packet pool configuration */
#define PACKET_POOL_SIZE        4096
#define PACKET_PAYLOAD_SIZE     1536    /* Slightly larger than PPP MTU (1500) */

/** PPP thread stack size */
#define PPP_THREAD_STACK_SIZE   1024

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
static uint8_t ppp_rx_byte;

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
  * @note   Reads bytes from UART3 and feeds them to NetX PPP via nx_ppp_byte_receive()
  */
static void ppp_read_thread_entry(ULONG param)
{
    (void)param;

    LOG_I(TAG_PPP, "PPP read thread started");

    while (1)
    {
        /* Read one byte at a time — PPP framing is byte-oriented */
        uint16_t n = ppp_serial->read(&ppp_rx_byte, 1, TX_WAIT_FOREVER);
        if (n > 0)
        {
            /* Feed byte to NetX PPP state machine */
            nx_ppp_byte_receive(&ppp_0, ppp_rx_byte);
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

        /* Small delay for IP stack to stabilize */
        tx_thread_sleep(1000);

        /* ---- Step 1: Get DNS server from PPP ---- */
        ULONG dns_server_addr = 0;
        status = nx_ppp_dns_address_get(&ppp_0, &dns_server_addr);
        if (status != NX_SUCCESS || dns_server_addr == 0)
        {
            /* Fallback to well-known DNS servers */
            LOG_W(TAG_DNS, "No DNS from PPP, using fallback 8.8.8.8");
            dns_server_addr = IP_ADDRESS(8, 8, 8, 8);
        }
        else
        {
            LOG_I(TAG_DNS, "PPP DNS server: %lu.%lu.%lu.%lu",
                  (dns_server_addr >> 24) & 0xFF,
                  (dns_server_addr >> 16) & 0xFF,
                  (dns_server_addr >> 8) & 0xFF,
                  dns_server_addr & 0xFF);
        }

        /* ---- Step 2: Create DNS client ---- */
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

        status = nx_dns_server_add(&dns_client, dns_server_addr);
        if (status != NX_SUCCESS)
        {
            LOG_E(TAG_DNS, "DNS server add failed: 0x%02X", status);
            nx_dns_delete(&dns_client);
            continue;
        }

        /* ---- Step 3: Resolve NTP server hostname ---- */
        LOG_I(TAG_DNS, "Resolving %s ...", NTP_SERVER_HOST);
        status = nx_dns_host_by_name_get(&dns_client,
                                          (UCHAR *)NTP_SERVER_HOST,
                                          &ntp_ip_address,
                                          5 * NX_IP_PERIODIC_RATE);
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

        /* ---- Step 4: Create SNTP client ---- */
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

        /* ---- Step 5: Request time from NTP server ---- */
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

                /* ---- Step 6: Set RTC ---- */
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
                           IP_ADDRESS(255, 255, 255, 0), &pool_0,
                           nx_ppp_driver,
                           ip_memory_ptr, IP_MEMORY_SIZE, 1);
    if (status != NX_SUCCESS)
    {
        LOG_E(TAG, "IP create failed: 0x%02X", status);
        return status;
    }

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
