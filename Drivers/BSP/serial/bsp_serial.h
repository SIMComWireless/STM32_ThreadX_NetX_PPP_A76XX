/**
  ******************************************************************************
  * @file    bsp_serial.h
  * @brief   Generic serial interface — abstracts UART/USB transport
  ******************************************************************************
  * @note
  *   Usage:
  *     // Get a concrete implementation (e.g. UART3)
  *     extern bsp_serial_t *bsp_serial_uart3;
  *     bsp_serial_uart3->init();
  *     bsp_serial_uart3->write(data, len);
  *     uint16_t n = bsp_serial_uart3->read(buf, sizeof(buf), 100);
  *
  *   To add a new backend (e.g. USB CDC):
  *     1. Create bsp_usb_cdc.c implementing the same vtable
  *     2. Export a bsp_serial_t* pointer
  *     3. Pass it to a7683e_init() / app_netxduo — zero code changes there
  ******************************************************************************
  */

#ifndef BSP_SERIAL_H
#define BSP_SERIAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ---------- Serial interface (virtual method table) ---------------------- */

typedef struct bsp_serial {
    const char *name;       /**< Human-readable name (e.g. "UART3", "USB CDC") */

    /**
     * @brief  Initialize the serial port
     */
    void (*init)(void);

    /**
     * @brief  Read data from the serial port
     * @param  buf        Destination buffer
     * @param  len        Max bytes to read
     * @param  timeout_ms Timeout in ms (0xFFFFFFFF = wait forever, 0 = non-blocking)
     * @return Number of bytes actually read (0 on timeout)
     */
    uint16_t (*read)(uint8_t *buf, uint16_t len, uint32_t timeout_ms);

    /**
     * @brief  Write data to the serial port (blocks until TX complete)
     * @param  data   Source buffer
     * @param  len    Number of bytes to send
     */
    void (*write)(const uint8_t *data, uint16_t len);

    /**
     * @brief  Check how many bytes are available to read
     * @return Number of bytes in the receive buffer
     */
    uint16_t (*rx_available)(void);

} bsp_serial_t;

#ifdef __cplusplus
}
#endif

#endif /* BSP_SERIAL_H */
