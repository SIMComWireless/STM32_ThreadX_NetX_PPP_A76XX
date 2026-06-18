/**
  ******************************************************************************
  * @file    bsp_usb.c
  * @brief   USB modem serial backend — bsp_serial_t over USB bulk endpoints
  ******************************************************************************
  * @note
  *   Each USB modem interface gets its own bsp_serial_t instance with:
  *   - write() → ux_host_class_modem_write (blocking bulk OUT)
  *   - read()  → lwrb_read from async reception (non-blocking poll with timeout)
  *   - rx_available() → lwrb_get_full
  *   - init()  → start async reception on the interface
  ******************************************************************************
  */

#include "bsp_usb.h"
#include "ux_host_class_modem.h"
#include "lwrb.h"
#include "tx_api.h"
#include "elog.h"
#include <string.h>

#define TAG "BSP_USB"

/* ---------- Static per-ifnum implementations ------------------------------ */

/*
 * bsp_serial_t uses function pointers (not method+context), so we need
 * separate static functions for each interface.  user_data stores the
 * UX_HOST_CLASS_MODEM* pointer.
 */

/* --- ifnum 4 (AT command port) --- */

static UX_HOST_CLASS_MODEM *usb_if4_modem = NULL;
static uint8_t              usb_if4_rx_buf[BSP_USB_RX_RB_SIZE];

static void if4_init(void) { /* no-op — reception started in bsp_usb_get */ }

static uint16_t if4_read(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    if (usb_if4_modem == NULL) return 0;
    ULONG got = lwrb_read(&usb_if4_modem->rx_rb, buf, len);
    if (got > 0 || timeout_ms == 0) return (uint16_t)got;

    uint32_t elapsed = 0;
    while (elapsed < timeout_ms)
    {
        tx_thread_sleep(5);
        elapsed += 5;
        got = lwrb_read(&usb_if4_modem->rx_rb, buf, len);
        if (got > 0) return (uint16_t)got;
    }
    return 0;
}

static void if4_write(const uint8_t *data, uint16_t len)
{
    if (usb_if4_modem == NULL) return;
    ULONG actual;
    ux_host_class_modem_write(usb_if4_modem, (UCHAR *)data, len, &actual);
}

static uint16_t if4_rx_avail(void)
{
    if (usb_if4_modem == NULL) return 0;
    return (uint16_t)lwrb_get_full(&usb_if4_modem->rx_rb);
}

static bsp_serial_t serial_if4 = {
    .name         = "USB AT",
    .user_data    = NULL,
    .init         = if4_init,
    .read         = if4_read,
    .write        = if4_write,
    .rx_available = if4_rx_avail,
};

/* --- ifnum 5 (Modem data port) --- */

static UX_HOST_CLASS_MODEM *usb_if5_modem = NULL;
static uint8_t              usb_if5_rx_buf[BSP_USB_RX_RB_SIZE];

static void if5_init(void) { /* no-op */ }

static uint16_t if5_read(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    if (usb_if5_modem == NULL) return 0;
    ULONG got = lwrb_read(&usb_if5_modem->rx_rb, buf, len);
    if (got > 0 || timeout_ms == 0) return (uint16_t)got;

    uint32_t elapsed = 0;
    while (elapsed < timeout_ms)
    {
        tx_thread_sleep(5);
        elapsed += 5;
        got = lwrb_read(&usb_if5_modem->rx_rb, buf, len);
        if (got > 0) return (uint16_t)got;
    }
    return 0;
}

static void if5_write(const uint8_t *data, uint16_t len)
{
    if (usb_if5_modem == NULL) return;
    ULONG actual;
    ux_host_class_modem_write(usb_if5_modem, (UCHAR *)data, len, &actual);
}

static uint16_t if5_rx_avail(void)
{
    if (usb_if5_modem == NULL) return 0;
    return (uint16_t)lwrb_get_full(&usb_if5_modem->rx_rb);
}

static bsp_serial_t serial_if5 = {
    .name         = "USB Modem",
    .user_data    = NULL,
    .init         = if5_init,
    .read         = if5_read,
    .write        = if5_write,
    .rx_available = if5_rx_avail,
};

/* ---------- Public API ---------------------------------------------------- */

bsp_serial_t *bsp_usb_get(uint8_t ifnum)
{
    UX_HOST_CLASS_MODEM *modem;
    UINT status;

    modem = ux_host_class_modem_find(ifnum);
    if (modem == NULL)
    {
        elog_e(TAG, "ifnum=%u not found", ifnum);
        return NULL;
    }

    /* Start async reception if not already started */
    if (modem->rx_state == UX_HOST_CLASS_MODEM_RX_STOPPED)
    {
        uint8_t *rb = NULL;
        if (ifnum == 4)      rb = usb_if4_rx_buf;
        else if (ifnum == 5) rb = usb_if5_rx_buf;
        else
        {
            elog_e(TAG, "ifnum=%u not supported (use 4 or 5)", ifnum);
            return NULL;
        }

        status = ux_host_class_modem_reception_start(
                     modem, rb, BSP_USB_RX_RB_SIZE, BSP_USB_RX_BLOCK_SIZE);
        if (status != UX_SUCCESS)
        {
            elog_e(TAG, "ifnum=%u RX start failed: 0x%02X", ifnum, status);
            return NULL;
        }
    }

    /* Map to the correct bsp_serial_t */
    if (ifnum == 4)
    {
        usb_if4_modem = modem;
        serial_if4.user_data = modem;
        elog_i(TAG, "ifnum=4 ready");
        return &serial_if4;
    }
    else if (ifnum == 5)
    {
        usb_if5_modem = modem;
        serial_if5.user_data = modem;
        elog_i(TAG, "ifnum=5 ready");
        return &serial_if5;
    }

    elog_e(TAG, "ifnum=%u not configured", ifnum);
    return NULL;
}

void bsp_usb_init_all(void)
{
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
