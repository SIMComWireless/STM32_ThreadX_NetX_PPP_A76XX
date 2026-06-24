/**
  ******************************************************************************
  * @file    bsp_usb.c
  * @brief   USB modem serial backend — bsp_serial_t over USB bulk endpoints
  ******************************************************************************
  * @note
  *   Each USB modem interface gets its own bsp_serial_t instance with:
  *   - write() → ux_host_class_modem_write (blocking bulk OUT)
  *   - read()  → lwrb_read + semaphore wait (event-driven, no busy-polling)
  *   - rx_available() → lwrb_get_full
  *   - init()  → start async reception on the interface
  *
  *   Adding a new interface: just add an entry to usb_ports[] array.
  ******************************************************************************
  */

#include "bsp_usb.h"
#include "ux_host_class_modem.h"
#include "lwrb.h"
#include "tx_api.h"
#include "elog.h"
#include <string.h>

#define TAG "BSP_USB"

/* ---------- Port descriptor (per-interface) --------------------------------- */

typedef struct {
    uint8_t               ifnum;        /* USB interface number                  */
    const char           *name;         /* Human-readable name                   */
    UX_HOST_CLASS_MODEM  *modem;        /* Active modem instance (or NULL)       */
    uint8_t               rx_buf[BSP_USB_RX_RB_SIZE]; /* Async RX ring buffer   */
    bsp_serial_t          serial;       /* The bsp_serial_t instance             */
} bsp_usb_port_t;

/* ---------- Generic function implementations -------------------------------- */

static void usb_port_init(bsp_serial_t *self)
{
    (void)self;
    /* no-op — reception started in bsp_usb_get */
}

static uint16_t usb_port_read(bsp_serial_t *self, uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    bsp_usb_port_t *port = (bsp_usb_port_t *)self->user_data;
    if (port == NULL) return 0;

    /* Capture modem pointer once — prevents TOCTOU race with
     * bsp_usb_notify_deactivated() which may NULL port->modem */
    UX_HOST_CLASS_MODEM *modem = port->modem;
    if (modem == NULL) return 0;

    ULONG got = lwrb_read(&modem->rx_rb, buf, len);
    if (got > 0) return (uint16_t)got;
    if (timeout_ms == 0) return 0;

    /* Wait for data signal from USB RX callback (event-driven, no busy-polling) */
    ULONG ticks = (timeout_ms == UINT32_MAX) ? TX_WAIT_FOREVER
                  : (timeout_ms * TX_TIMER_TICKS_PER_SECOND / 1000);
    _ux_host_semaphore_get(&modem->rx_data_sem, ticks);
    got = lwrb_read(&modem->rx_rb, buf, len);
    return (uint16_t)got;
}

static void usb_port_write(bsp_serial_t *self, const uint8_t *data, uint16_t len)
{
    bsp_usb_port_t *port = (bsp_usb_port_t *)self->user_data;
    if (port == NULL) return;
    UX_HOST_CLASS_MODEM *modem = port->modem;
    if (modem == NULL) return;

    ULONG actual;
    ux_host_class_modem_write(modem, (UCHAR *)data, len, &actual);
}

static uint16_t usb_port_rx_avail(bsp_serial_t *self)
{
    bsp_usb_port_t *port = (bsp_usb_port_t *)self->user_data;
    if (port == NULL) return 0;
    UX_HOST_CLASS_MODEM *modem = port->modem;
    if (modem == NULL) return 0;

    return (uint16_t)lwrb_get_full(&modem->rx_rb);
}

/* ---------- Port table — add new interfaces here --------------------------- */

static bsp_usb_port_t usb_ports[] = {
    { .ifnum = 1, .name = "USB AT 9028", .modem = NULL,
      .serial = { .name = "USB AT 9028", .user_data = NULL,
                  .init = usb_port_init, .read = usb_port_read,
                  .write = usb_port_write, .rx_available = usb_port_rx_avail }},
    { .ifnum = 4, .name = "USB AT", .modem = NULL,
      .serial = { .name = "USB AT", .user_data = NULL,
                  .init = usb_port_init, .read = usb_port_read,
                  .write = usb_port_write, .rx_available = usb_port_rx_avail }},
    { .ifnum = 5, .name = "USB Modem", .modem = NULL,
      .serial = { .name = "USB Modem", .user_data = NULL,
                  .init = usb_port_init, .read = usb_port_read,
                  .write = usb_port_write, .rx_available = usb_port_rx_avail }},
};

#define USB_PORT_COUNT  (sizeof(usb_ports) / sizeof(usb_ports[0]))

/* Initialize user_data pointers (called once at startup) */
static void usb_ports_init(void)
{
    static uint8_t inited = 0;
    if (inited) return;
    for (uint8_t i = 0; i < USB_PORT_COUNT; i++)
        usb_ports[i].serial.user_data = &usb_ports[i];
    inited = 1;
}

/* Find port descriptor by ifnum */
static bsp_usb_port_t *usb_port_find(uint8_t ifnum)
{
    for (uint8_t i = 0; i < USB_PORT_COUNT; i++)
        if (usb_ports[i].ifnum == ifnum)
            return &usb_ports[i];
    return NULL;
}

/* ---------- Public API ---------------------------------------------------- */

bsp_serial_t *bsp_usb_get(uint8_t ifnum)
{
    UX_HOST_CLASS_MODEM *modem;
    UINT status;

    usb_ports_init();

    bsp_usb_port_t *port = usb_port_find(ifnum);
    if (port == NULL)
    {
        elog_e(TAG, "ifnum=%u not configured", ifnum);
        return NULL;
    }

    modem = ux_host_class_modem_find(ifnum);
    if (modem == NULL)
    {
        elog_e(TAG, "ifnum=%u not found", ifnum);
        return NULL;
    }

    /* Stop stale async RX if the modem instance was replaced (re-enumeration) */
    if (port->modem != NULL && port->modem != modem)
    {
        elog_w(TAG, "ifnum=%u modem changed (%p -> %p) — clearing stale pointer",
               ifnum, (void *)port->modem, (void *)modem);
        port->modem = NULL;
    }

    /* Start async reception if not already started */
    if (modem->rx_state == UX_HOST_CLASS_MODEM_RX_STOPPED)
    {
        status = ux_host_class_modem_reception_start(
                     modem, port->rx_buf, BSP_USB_RX_RB_SIZE, BSP_USB_RX_BLOCK_SIZE);
        if (status != UX_SUCCESS)
        {
            elog_e(TAG, "ifnum=%u RX start failed: 0x%02X", ifnum, status);
            return NULL;
        }
    }

    port->modem = modem;
    elog_i(TAG, "ifnum=%u ready (modem=%p)", ifnum, (void *)modem);
    return &port->serial;
}

void bsp_usb_init_all(void)
{
    usb_ports_init();

    elog_i(TAG, "scanning %lu modem interfaces",
          (unsigned long)ux_host_class_modem_count);

    for (ULONG i = 0; i < ux_host_class_modem_count; i++)
    {
        UX_HOST_CLASS_MODEM *m = ux_host_class_modem_instances[i];
        if (m == NULL) continue;
        elog_i(TAG, "  ifnum=%lu bulk_in=%p bulk_out=%p",
              (unsigned long)ux_host_class_modem_ifnum(m),
              (void *)m->bulk_in, (void *)m->bulk_out);
    }
}

void bsp_usb_notify_deactivated(uint8_t ifnum)
{
    bsp_usb_port_t *port = usb_port_find(ifnum);
    if (port != NULL && port->modem != NULL)
    {
        elog_w(TAG, "ifnum=%u deactivated — clearing modem pointer", ifnum);
        port->modem = NULL;
    }
}

void bsp_usb_throughput_test(uint8_t ifnum, uint32_t duration_s)
{
    UX_HOST_CLASS_MODEM *modem;
    UINT status;

    modem = ux_host_class_modem_find(ifnum);
    if (modem == NULL)
    {
        elog_e(TAG, "[throughput] ifnum=%u not found", ifnum);
        return;
    }

    static uint8_t test_buf[1024*2];
    memset(test_buf, 'U', sizeof(test_buf));

    ULONG total_bytes = 0;
    ULONG write_count = 0;
    ULONG start_tick = tx_time_get();
    ULONG expire_tick = start_tick + duration_s * TX_TIMER_TICKS_PER_SECOND;
    ULONG timeout_errors = 0;

    elog_i(TAG, "=== USB Bulk TX Throughput Test Start (%lus, ifnum=%u) ===",
           (unsigned long)duration_s, (unsigned long)ifnum);

    while (tx_time_get() < expire_tick)
    {
        ULONG actual = 0;
        status = ux_host_class_modem_write(modem, test_buf, sizeof(test_buf), &actual);
        if (status == UX_SUCCESS && actual > 0)
        {
            total_bytes += actual;
            write_count++;
        }
        else if (status == UX_TRANSFER_TIMEOUT)
        {
            timeout_errors++;
            if (timeout_errors > 100)
            {
                elog_e(TAG, "[throughput] too many timeouts, aborting");
                break;
            }
        }
        else
        {
            elog_e(TAG, "[throughput] write failed: 0x%02X", status);
            break;
        }
    }

    ULONG end_tick = tx_time_get();
    ULONG elapsed_ticks = end_tick - start_tick;
    if (elapsed_ticks == 0) elapsed_ticks = 1;

    ULONG bytes_per_sec = total_bytes / elapsed_ticks * TX_TIMER_TICKS_PER_SECOND
                        + (total_bytes % elapsed_ticks) * TX_TIMER_TICKS_PER_SECOND / elapsed_ticks;
    ULONG throughput_kbps = bytes_per_sec * 8 / 1000;
    ULONG throughput_mbps_x10 = throughput_kbps / 100;

    elog_i(TAG, "=== USB Bulk TX Throughput Test Results ===");
    elog_i(TAG, "  Duration    : %lu.%lus", elapsed_ticks / TX_TIMER_TICKS_PER_SECOND,
           (elapsed_ticks % TX_TIMER_TICKS_PER_SECOND) * 10 / TX_TIMER_TICKS_PER_SECOND);
    elog_i(TAG, "  Write calls : %lu", write_count);
    elog_i(TAG, "  Total bytes : %lu bytes (%lu KB)", total_bytes, total_bytes / 1024);
    elog_i(TAG, "  Throughput  : %lu.%lu Mbps (%lu KB/s)",
           throughput_mbps_x10 / 10, throughput_mbps_x10 % 10, bytes_per_sec / 1024);
    elog_i(TAG, "  Timeouts    : %lu", timeout_errors);
    elog_i(TAG, "=========================================");
}
