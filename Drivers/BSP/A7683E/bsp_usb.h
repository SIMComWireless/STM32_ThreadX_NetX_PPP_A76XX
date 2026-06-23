/**
  ******************************************************************************
  * @file    bsp_usb.h
  * @brief   USB modem serial backend — bsp_serial_t interface over USB
  ******************************************************************************
  * @note
  *   Provides bsp_serial_t instances for USB modem interfaces (ifnum 4/5).
  *   Reception is async via lwrb (USB bulk IN callback writes to ring buffer).
  *   Transmit is blocking via ux_host_class_modem_write.
  *
  *   Usage:
  *     bsp_serial_t *at_port = bsp_usb_get(4);  // AT command port
  *     if (at_port) {
  *         at_port->write("AT\r\n", 4);
  *         uint16_t n = at_port->read(buf, sizeof(buf), 1000);
  *     }
  ******************************************************************************
  */

#ifndef BSP_USB_H
#define BSP_USB_H

#ifdef __cplusplus
extern "C" {
#endif

#include "bsp_serial.h"
#include <stdint.h>

/** RX ring buffer size per interface */
#ifndef BSP_USB_RX_RB_SIZE
#define BSP_USB_RX_RB_SIZE      2048
#endif

/** USB bulk IN transfer block size */
#ifndef BSP_USB_RX_BLOCK_SIZE
#define BSP_USB_RX_BLOCK_SIZE   512
#endif

/**
 * @brief  Get bsp_serial_t for a specific USB interface number.
 * @param  ifnum  USB interface number (e.g. 4 for AT, 5 for Modem)
 * @return Pointer to bsp_serial_t, or NULL if interface not ready.
 *         On first call, starts async RX reception on the interface.
 */
bsp_serial_t *bsp_usb_get(uint8_t ifnum);

/**
 * @brief  List all active modem interfaces to log.
 */
void bsp_usb_init_all(void);

/**
 * @brief  Notify BSP USB that a modem interface was deactivated.
 *         Clears the cached modem pointer for the given interface number
 *         to prevent use-after-free when the instance is freed.
 *         Called from ux_host_class_modem DEACTIVATE handler.
 * @param  ifnum  USB interface number that was deactivated
 */
void bsp_usb_notify_deactivated(uint8_t ifnum);

#ifdef __cplusplus
}
#endif

#endif /* BSP_USB_H */
