/**
  ******************************************************************************
  * @file    app_netxduo.c
  * @brief   NetXDuo PPP core — initialization, PPP dial, read thread, teardown
  ******************************************************************************
  * @note
  *   This file owns the PPP/IP/NetX lifecycle:
  *     MX_NetXDuo_Init()      — allocate memory, create packet pool & threads
  *     app_netxduo_create_ppp() — create PPP + IP instance, enable protocols
  *     app_netxduo_start_ppp()  — begin PPP negotiation
  *     app_netxduo_destroy_ppp() — teardown on USB disconnect
  *
  *   Subsystems are in separate files:
  *     net_dns.c   — DNS client init + hostname resolution
  *     net_ntp.c   — NTP time synchronization (SNTP + RTC set)
  *     net_iperf.c — TCP/UDP throughput tests (conditional on IPERF_ENABLE)
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
#include "nx_ip.h"
#include "nx_ppp.h"
#include "nxd_dns.h"
#include "elog.h"
#include "a7683e.h"
#include "bsp_serial.h"
#include "main.h"
#include "net_dns.h"
#include "net_ntp.h"
#include "app_anjay.h"

/* USER CODE BEGIN Includes */
/* USER CODE END Includes */

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define TAG     "PPP"

/** PPP serial read thread */
#define PPP_READ_THREAD_PRIO    5
#define PPP_READ_THREAD_STACK   1024

/** PPP thread (used by nx_ppp_create) */
#define PPP_THREAD_STACK_SIZE   1024

/** Packet pool: 4096*30 = 120KB ≈ 80 × 1536B packets.
 *  TCP needs many in-flight packets; too small → send blocks. */
#define PACKET_POOL_SIZE        (4096 * 30)
#define PACKET_PAYLOAD_SIZE     1536

/** IP / ARP memory */
#define IP_MEMORY_SIZE          2048
#define ARP_CACHE_SIZE          1024

/** iperf thread priorities — defined here because MX_NetXDuo_Init creates them */
#define TCP_IPERF_THREAD_PRIO   32
#define TCP_IPERF_THREAD_STACK  (1024 * 3)
#define UDP_IPERF_THREAD_PRIO   33
#define UDP_IPERF_THREAD_STACK  (1024 * 3)

/* ============================================================================
 * NetX objects (exported via app_netxduo.h)
 * ============================================================================ */

NX_PACKET_POOL         pool_0;
NX_IP                  ip_0;
NX_PPP                 ppp_0;
TX_EVENT_FLAGS_GROUP   ppp_events;
TX_BYTE_POOL          *byte_pool_ptr = NULL;

/* PPP teardown flag — iperf/NTP threads check before network ops */
volatile uint8_t       ppp_destroying = 0;

/* ============================================================================
 * Private state
 * ============================================================================ */

/* Memory pointers — allocated from byte pool in MX_NetXDuo_Init */
static VOID *pool_memory_ptr;
static VOID *ip_memory_ptr;
static VOID *arp_cache_ptr;
static VOID *ppp_stack_ptr;
static VOID *ppp_read_stack_ptr;
static VOID *ntp_stack_ptr;
#if IPERF_ENABLE
static VOID *tcp_client_stack_ptr;
static VOID *udp_client_stack_ptr;
#endif

/* Thread handles */
static TX_THREAD ppp_read_thread;
static TX_THREAD ntp_thread;
#if IPERF_ENABLE
static TX_THREAD tcp_client_thread;
static TX_THREAD udp_client_thread;
#endif

/* PPP lifecycle flags */
static volatile uint8_t ppp_created = 0;

/* Modem reconnect event — USB disconnect signals this */
static TX_EVENT_FLAGS_GROUP modem_events;
static volatile uint8_t modem_events_created = 0;

/* PPP serial port (set by app_netxduo_set_serial) */
static bsp_serial_t *ppp_serial = NULL;

/* Debug: track which init step failed */
volatile UINT netx_init_status = 0;
volatile UINT netx_init_step   = 0;

/* ============================================================================
 * PPP byte send — frame-buffered DMA output
 * ============================================================================ */

#define PPP_TX_BUF_SIZE     4096
#define PPP_RX_BATCH_SIZE   1024

static uint8_t  ppp_tx_buf[PPP_TX_BUF_SIZE];
static uint16_t ppp_tx_pos = 0;
static uint8_t  ppp_tx_overflow_logged = 0;

static uint8_t  ppp_rx_buf[PPP_RX_BATCH_SIZE];

static void ppp_byte_send_reset(void)
{
    ppp_tx_pos = 0;
    ppp_tx_overflow_logged = 0;
}

/**
  * @brief  PPP byte send callback — buffers a full frame, flushes on 0x7E.
  *         One DMA write per PPP frame instead of per byte.
  */
static void ppp_byte_send(UCHAR byte)
{
    if (!ppp_serial) return;

    if (byte == 0x7E)
    {
        if (ppp_tx_pos == 0)
        {
            /* Start of new frame */
            ppp_tx_buf[ppp_tx_pos++] = 0x7E;
        }
        else if (ppp_tx_buf[0] == 0x7E)
        {
            /* End of frame — flush */
            if (ppp_tx_pos < sizeof(ppp_tx_buf))
                ppp_tx_buf[ppp_tx_pos++] = 0x7E;
            ppp_serial->write(ppp_serial, ppp_tx_buf, ppp_tx_pos);
        #ifdef NX_PPP_DEBUG_LOG_ENABLE
            elog_hexdump(TAG, 16, ppp_tx_buf, ppp_tx_pos);
        #endif
            ppp_tx_pos = 0;
        }
        else
        {
            /* Stale data — discard and start fresh */
            ppp_tx_pos = 0;
            ppp_tx_buf[ppp_tx_pos++] = 0x7E;
        }
        return;
    }

    if (ppp_tx_pos < sizeof(ppp_tx_buf))
    {
        ppp_tx_buf[ppp_tx_pos++] = byte;
    }
    else if (!ppp_tx_overflow_logged)
    {
        ppp_tx_overflow_logged = 1;
        elog_e(TAG, "PPP TX overflow! Frame > %u bytes", (unsigned)sizeof(ppp_tx_buf));
    }
}

/* ============================================================================
 * PPP callbacks
 * ============================================================================ */

static void print_ip_addresses(void)
{
    ULONG addr = 0, mask = 0;
    if (nx_ip_address_get(&ip_0, &addr, &mask) == NX_SUCCESS && addr != 0)
    {
        elog_d(TAG, "IPv4: %lu.%lu.%lu.%lu  Mask: %lu.%lu.%lu.%lu",
              (addr >> 24) & 0xFF, (addr >> 16) & 0xFF,
              (addr >> 8)  & 0xFF,  addr & 0xFF,
              (mask >> 24) & 0xFF, (mask >> 16) & 0xFF,
              (mask >> 8)  & 0xFF,  mask & 0xFF);

        ULONG gw = 0;
        nx_ip_gateway_address_get(&ip_0, &gw);
        elog_d(TAG, "Gateway: %lu.%lu.%lu.%lu",
              (gw >> 24) & 0xFF, (gw >> 16) & 0xFF,
              (gw >> 8)  & 0xFF,  gw & 0xFF);
    }
    else
    {
        elog_w(TAG, "IPv4: not assigned");
    }
}

static void ppp_link_up_callback(NX_PPP *ppp_ptr)
{
    (void)ppp_ptr;
    elog_d(TAG, "PPP link UP");
    print_ip_addresses();
    dns_client_init();
    tx_event_flags_set(&ppp_events, PPP_EVT_LINK_UP, TX_OR);
}

static void ppp_link_down_callback(NX_PPP *ppp_ptr)
{
    (void)ppp_ptr;
    elog_w(TAG, "PPP link DOWN");
    tx_event_flags_set(&ppp_events, PPP_EVT_LINK_DOWN, TX_OR);
}

static UINT ppp_generate_login(CHAR *name, CHAR *password)
{
    name[0] = '\0';
    password[0] = '\0';
    return NX_SUCCESS;
}

/* ============================================================================
 * PPP read thread — feeds serial bytes to NetX PPP parser
 * ============================================================================ */

static void ppp_read_thread_entry(ULONG param)
{
    (void)param;

    /* Wait for serial port to be configured */
    while (!ppp_serial) {
        tx_thread_sleep(10);
    }

    elog_d(TAG, "PPP read thread started (%s)", ppp_serial->name);

    /* Drain bytes captured during AT command parsing (first LCP frame) */
    {
        uint16_t n = a7683e_drain_overflow(ppp_rx_buf, PPP_RX_BATCH_SIZE);
        for (uint16_t i = 0; i < n; i++)
            nx_ppp_byte_receive(&ppp_0, ppp_rx_buf[i]);
        if (n > 0) elog_d(TAG, "Drained %u overflow bytes", n);
    }

    /* Main loop: read bytes → feed to PPP parser */
    while (1)
    {
        uint16_t n = ppp_serial->read(ppp_serial, ppp_rx_buf,
                                       PPP_RX_BATCH_SIZE, TX_WAIT_FOREVER);
        for (uint16_t i = 0; i < n; i++)
            nx_ppp_byte_receive(&ppp_0, ppp_rx_buf[i]);
    }
}

/* ============================================================================
 * Accessors (exported via app_netxduo.h)
 * ============================================================================ */

NX_IP          *app_netxduo_get_active_ip(void)   { return &ip_0; }
NX_PACKET_POOL *app_netxduo_get_active_pool(void) { return &pool_0; }

void app_netxduo_set_serial(bsp_serial_t *serial)
{
    ppp_serial = serial;
}

/* ============================================================================
 * MX_NetXDuo_Init — allocate memory, create threads (called from ThreadX init)
 * ============================================================================ */

UINT MX_NetXDuo_Init(VOID *memory_ptr)
{
    UINT status;
    TX_BYTE_POOL *byte_pool = (TX_BYTE_POOL *)memory_ptr;
    byte_pool_ptr = byte_pool;

    nx_system_initialize();

    /* ---- Allocate memory from byte pool ---- */

    #define ALLOC(ptr, size, step) \
        netx_init_step = (step); \
        status = tx_byte_allocate(byte_pool, &(ptr), (size), TX_NO_WAIT); \
        if (status != TX_SUCCESS) { netx_init_status = status; return status; }

    ALLOC(pool_memory_ptr,    PACKET_POOL_SIZE,         1)
    ALLOC(ip_memory_ptr,      IP_MEMORY_SIZE,           2)
    ALLOC(ppp_stack_ptr,      PPP_THREAD_STACK_SIZE,    3)
    ALLOC(ppp_read_stack_ptr, PPP_READ_THREAD_STACK,    4)
    ALLOC(ntp_stack_ptr,      NTP_THREAD_STACK,         5)
#if IPERF_ENABLE
    ALLOC(tcp_client_stack_ptr, TCP_IPERF_THREAD_STACK,  51)
    ALLOC(udp_client_stack_ptr, UDP_IPERF_THREAD_STACK,  52)
#endif
    ALLOC(arp_cache_ptr,      ARP_CACHE_SIZE,           6)

    #undef ALLOC

    /* ---- Create NetX objects ---- */

    netx_init_step = 7;
    status = nx_packet_pool_create(&pool_0, "Packet Pool",
                                    PACKET_PAYLOAD_SIZE, pool_memory_ptr,
                                    PACKET_POOL_SIZE);
    if (status != NX_SUCCESS) { netx_init_status = status; return status; }

    netx_init_step = 8;
    tx_event_flags_create(&ppp_events, "PPP Events");

    /* ---- Create threads ---- */

    /* PPP Read: TX_DONT_START — resumed later by app_netxduo_create_ppp() */
    netx_init_step = 9;
    status = tx_thread_create(&ppp_read_thread, "PPP Read",
                               ppp_read_thread_entry, 0,
                               ppp_read_stack_ptr, PPP_READ_THREAD_STACK,
                               PPP_READ_THREAD_PRIO, PPP_READ_THREAD_PRIO,
                               TX_NO_TIME_SLICE, TX_DONT_START);
    if (status != TX_SUCCESS) { netx_init_status = status; return status; }

    /* NTP, TCP/UDP iperf: TX_AUTO_START */
    netx_init_step = 10;
    status = tx_thread_create(&ntp_thread, "NTP Sync",
                               ntp_thread_entry, 0,
                               ntp_stack_ptr, NTP_THREAD_STACK,
                               NTP_THREAD_PRIO, NTP_THREAD_PRIO,
                               TX_NO_TIME_SLICE, TX_AUTO_START);
    if (status != TX_SUCCESS) { netx_init_status = status; return status; }

#if IPERF_ENABLE
    netx_init_step = 11;
    status = tx_thread_create(&tcp_client_thread, "TCP iperf TX",
                               tcp_iperf_thread_entry, 0,
                               tcp_client_stack_ptr, TCP_IPERF_THREAD_STACK,
                               TCP_IPERF_THREAD_PRIO, TCP_IPERF_THREAD_PRIO,
                               TX_NO_TIME_SLICE, TX_AUTO_START);
    if (status != TX_SUCCESS) { netx_init_status = status; return status; }

    netx_init_step = 12;
    status = tx_thread_create(&udp_client_thread, "UDP iperf TX",
                               udp_iperf_thread_entry, 0,
                               udp_client_stack_ptr, UDP_IPERF_THREAD_STACK,
                               UDP_IPERF_THREAD_PRIO, UDP_IPERF_THREAD_PRIO,
                               TX_NO_TIME_SLICE, TX_AUTO_START);
    if (status != TX_SUCCESS) { netx_init_status = status; return status; }
#endif

    /* NOTE: PPP + IP creation deferred to app_netxduo_create_ppp()
     * (needs thread context for nx_ppp_create) */
    MX_Anjay_Init();
    /* USER CODE BEGIN MX_NetXDuo_Init */
    /* USER CODE END MX_NetXDuo_Init */

    return NX_SUCCESS;
}

/* ============================================================================
 * PPP lifecycle — create, start, destroy
 * ============================================================================ */

/**
  * @brief  Create PPP + IP instance (must be called from thread context).
  *         Idempotent — safe to call multiple times.
  */
UINT app_netxduo_create_ppp(void)
{
    UINT status;
    if (ppp_created) return NX_SUCCESS;

    ppp_byte_send_reset();

    /* Step 1: Create PPP instance */
    status = nx_ppp_create(&ppp_0, "PPP", &ip_0,
                            ppp_stack_ptr, PPP_THREAD_STACK_SIZE, 15,
                            &pool_0, NX_NULL, ppp_byte_send);
    if (status != NX_SUCCESS) {
        elog_e(TAG, "PPP create failed: 0x%02X", status);
        return status;
    }

    /* Step 2: Create IP instance (PPP driver now ready) */
    status = nx_ip_create(&ip_0, "NetX IP", IP_ADDRESS(0,0,0,0),
                           IP_ADDRESS(0,0,0,0), &pool_0,
                           nx_ppp_driver, ip_memory_ptr, IP_MEMORY_SIZE, 1);
    if (status != NX_SUCCESS) {
        elog_e(TAG, "IP create failed: 0x%02X", status);
        return status;
    }

    /* Enable protocols */
    #define ENABLE(api, name) \
        status = (api); \
        if (status != NX_SUCCESS) { \
            elog_e(TAG, name " enable failed: 0x%02X", status); \
            return status; \
        }

    ENABLE(nx_ip_fragment_enable(&ip_0),                    "IP fragment")
    ENABLE(nx_arp_enable(&ip_0, arp_cache_ptr, ARP_CACHE_SIZE), "ARP")
    ENABLE(nx_icmp_enable(&ip_0),                           "ICMP")
    ENABLE(nx_udp_enable(&ip_0),                            "UDP")
    ENABLE(nx_tcp_enable(&ip_0),                            "TCP")

    #undef ENABLE

    /* Step 3: Configure PPP callbacks */
    nx_ppp_link_up_notify(&ppp_0, ppp_link_up_callback);
    nx_ppp_link_down_notify(&ppp_0, ppp_link_down_callback);
    nx_ppp_ip_address_assign(&ppp_0, 0, 0);

    /* Step 4: Start PPP read thread */
    if (!ppp_created) {
        status = tx_thread_resume(&ppp_read_thread);
    } else {
        status = tx_thread_create(&ppp_read_thread, "PPP Read",
                                   ppp_read_thread_entry, 0,
                                   ppp_read_stack_ptr, PPP_READ_THREAD_STACK,
                                   PPP_READ_THREAD_PRIO, PPP_READ_THREAD_PRIO,
                                   TX_NO_TIME_SLICE, TX_AUTO_START);
    }
    if (status != NX_SUCCESS) {
        elog_e(TAG, "PPP read thread start failed: 0x%02X", status);
        return status;
    }

    ppp_created = 1;
    elog_i(TAG, "PPP + IP instance ready");
    return NX_SUCCESS;
}

/**
  * @brief  Start PPP negotiation.
  */
UINT app_netxduo_start_ppp(void)
{
    UINT status = nx_ppp_start(&ppp_0);
    if (status == NX_SUCCESS)
        elog_d(TAG, "PPP negotiation started");
    else
        elog_e(TAG, "nx_ppp_start failed: 0x%02X", status);
    return status;
}

/**
  * @brief  Destroy PPP instance and release all resources.
  *         Call on USB disconnect. After this, create_ppp() can run again.
  *
  *  Teardown: ppp_destroying flag → stop read thread → stop PPP
  *            → delete DNS → delete PPP → delete IP → reset flags
  */
void app_netxduo_destroy_ppp(void)
{
    UINT status;
    elog_i(TAG, "=== Destroying PPP instance ===");

    ppp_destroying = 1;

    /* Terminate read thread (blocked on serial read, won't return) */
    if (ppp_read_thread.tx_thread_id == 0x54485244UL) {
        tx_thread_terminate(&ppp_read_thread);
        tx_thread_delete(&ppp_read_thread);
    }

    /* Stop PPP (triggers link-down → NTP/iperf threads wake and wait) */
    if (ppp_0.nx_ppp_id == NX_PPP_ID) {
        status = nx_ppp_stop(&ppp_0);
        elog_d(TAG, "PPP stop: 0x%02X", status);
    }

    /* Delete DNS client */
    if (dns_client_initialized) {
        nx_dns_delete(&dns_client);
        dns_client_initialized = 0;
    }

    /* Delete PPP + IP instances */
    if (ppp_0.nx_ppp_id == NX_PPP_ID) {
        nx_ppp_delete(&ppp_0);
    }
    if (ip_0.nx_ip_id == NX_IP_ID) {
        nx_ip_delete(&ip_0);
    }

    ppp_created = 0;
    ppp_destroying = 0;

    elog_i(TAG, "=== PPP destroyed — ready for reconnect ===");
}

/* ============================================================================
 * Modem reconnect — USB disconnect signaling
 * ============================================================================ */

void app_netxduo_set_reconnect_event(void)
{
    if (!modem_events_created) {
        tx_event_flags_create(&modem_events, "Modem Events");
        modem_events_created = 1;
    }
    tx_event_flags_set(&modem_events, MODEM_EVT_USB_DISCONNECT, TX_OR);
    elog_w(TAG, ">> USB disconnect signaled");
}

void app_netxduo_wait_disconnect(void)
{
    ULONG flags;
    if (!modem_events_created) {
        tx_event_flags_create(&modem_events, "Modem Events");
        modem_events_created = 1;
    }
    tx_event_flags_get(&modem_events, MODEM_EVT_USB_DISCONNECT,
                       TX_OR_CLEAR, &flags, TX_WAIT_FOREVER);
}

/* USER CODE BEGIN 1 */
/* USER CODE END 1 */
