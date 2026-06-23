/**
  ******************************************************************************
  * @file    cmux_port.c
  * @brief   CMUX port layer — bridges UART3 serial driver with CMUX protocol
  ******************************************************************************
  * @note
  *   Architecture:
  *   - UART3 ISR hook writes raw bytes to a ring buffer (minimal ISR work)
  *   - Dedicated CMUX thread reads ring buffer, calls cmux_feed(), sends
  *     responses (UA, DISC) entirely in thread context
  *   - cmux_port_write() is called from thread context — safe to use
  *     DMA + semaphore via bsp_serial_uart3->write()
  *
  *   This avoids calling ThreadX primitives (semaphore/event) from ISR,
  *   which would cause a fault.
  *
  *   Controlled by CMUX_ENABLE macro (defined in cmux.h, default 1).
  ******************************************************************************
  */

#include "cmux.h"

#if CMUX_ENABLE

#include "lwrb/lwrb.h"
#include "bsp_uart3.h"
#include "tx_api.h"
#include "elog.h"
#include <string.h>

#define TAG "CMUX_PORT"

/* ---------- Configuration ------------------------------------------------- */

/** CMUX RX ring buffer size */
#define CMUX_RX_RING_SIZE    1024

/** CMUX thread configuration */
#define CMUX_THREAD_PRIO     13       /* Between Modem Init(12) and PPP Read(14) */
#define CMUX_THREAD_PREEMPT  13       /* Preempt threshold = same as priority */
#define CMUX_THREAD_STACK    4*1024

#define EVT_CMUX_DATA        0x01

/* ---------- Global CMUX instance ----------------------------------------- */

/** Global CMUX context — shared with a7683e.c and app_netxduo.c */
cmux_t g_cmux;

/* ---------- RX ring buffer (ISR writes, thread reads) -------------------- */

static uint8_t cmux_rb_data[CMUX_RX_RING_SIZE];
static lwrb_t  cmux_rb;

/* ---------- CMUX processing thread --------------------------------------- */

static TX_THREAD       cmux_thread;
static uint8_t         cmux_thread_stack[CMUX_THREAD_STACK];
static TX_EVENT_FLAGS_GROUP cmux_events;
static volatile uint8_t cmux_thread_running = 0;

/**
 * @brief  CMUX thread — reads raw bytes from ring buffer, feeds to cmux parser.
 *         All CMUX frame processing (SABM/UA/DISC/UIH) happens here,
 *         including sending responses via cmux_port_write().
 */
static void cmux_thread_entry(ULONG param)
{
    (void)param;
    uint8_t buf[256];

    elog_d(TAG, "CMUX thread started");

    while (cmux_thread_running)
    {
        /* Wait for data from ISR */
        ULONG actual_flags;
        UINT status = tx_event_flags_get(&cmux_events, EVT_CMUX_DATA,
                                          TX_OR_CLEAR, &actual_flags,
                                          TX_WAIT_FOREVER);
        if (status != TX_SUCCESS) continue;

        /* Read and process all available data */
        uint32_t n;
        while ((n = lwrb_read(&cmux_rb, buf, sizeof(buf))) > 0)
        {
            elog_d(TAG, "CMUX RX [%u bytes]", (unsigned)n);
            cmux_feed(&g_cmux, buf, (uint16_t)n);
        }
    }

    elog_d(TAG, "CMUX thread stopped");
}

/* ---------- ISR RX hook -------------------------------------------------- */

/**
 * @brief  RX hook called from UART3 ISR — writes raw bytes to ring buffer.
 *         No CMUX processing in ISR — the dedicated thread handles that.
 */
static void cmux_rx_isr_hook(const uint8_t *data, uint16_t len)
{
    lwrb_write(&cmux_rb, data, len);
    tx_event_flags_set(&cmux_events, EVT_CMUX_DATA, TX_OR);
}

/* ---------- Port functions required by cmux.c ---------------------------- */

/**
 * @brief  CMUX port write — sends raw CMUX frame bytes via UART3.
 *         Called by cmux_frame_send() from CMUX THREAD context (safe).
 */
void cmux_port_write(const uint8_t *data, uint16_t len)
{
    if (len == 0 || !bsp_serial_uart3) return;
    bsp_serial_uart3->write(data, len);
}

/* ---------- Bridge API --------------------------------------------------- */

/**
 * @brief  Start the CMUX bridge:
 *         1. Reset ring buffer
 *         2. Create CMUX processing thread
 *         3. Install UART3 RX hook (ISR → ring buffer)
 */
void cmux_bridge_start(void)
{
    /* Reset ring buffer */
    lwrb_init(&cmux_rb, cmux_rb_data, CMUX_RX_RING_SIZE);

    /* Create event flags */
    tx_event_flags_create(&cmux_events, "CMUX Events");

    /* Create and start CMUX processing thread */
    cmux_thread_running = 1;
    tx_thread_create(&cmux_thread, "CMUX RX", cmux_thread_entry, 0,
                     cmux_thread_stack, CMUX_THREAD_STACK,
                     CMUX_THREAD_PRIO, CMUX_THREAD_PREEMPT,
                     TX_NO_TIME_SLICE, TX_AUTO_START);

    /* Install ISR hook — data now goes to ring buffer */
    bsp_uart3_set_rx_hook(cmux_rx_isr_hook);

    elog_d(TAG, "CMUX bridge started (ISR → ring → CMUX thread)");
}

/**
 * @brief  Stop the CMUX bridge:
 *         1. Remove UART3 RX hook
 *         2. Stop and delete CMUX thread
 *         3. Delete event flags
 */
void cmux_bridge_stop(void)
{
    /* Remove ISR hook first — no more data from UART */
    bsp_uart3_set_rx_hook(NULL);

    /* Signal thread to exit */
    cmux_thread_running = 0;
    tx_event_flags_set(&cmux_events, EVT_CMUX_DATA, TX_OR);

    /* Give thread time to wake up and exit its entry function */
    tx_thread_sleep(5);

    /* Delete thread and cleanup */
    tx_thread_delete(&cmux_thread);
    tx_event_flags_delete(&cmux_events);

    elog_i(TAG, "CMUX bridge stopped");
}

#endif /* CMUX_ENABLE */
