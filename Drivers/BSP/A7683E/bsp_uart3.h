/**
  ******************************************************************************
  * @file    bsp_uart3.h
  * @brief   USART3 DMA driver — implements bsp_serial_t interface
  ******************************************************************************
  * @note
  *   Uses HAL_UARTEx_ReceiveToIdle_DMA for automatic idle-line detection.
  *   No manual idle ISR needed — HAL calls HAL_UARTEx_RxEventCallback.
  ******************************************************************************
  */

#ifndef BSP_UART3_H
#define BSP_UART3_H

#ifdef __cplusplus
extern "C" {
#endif

#include "bsp_serial.h"
#include <stdint.h>

/* ---------- Configuration ------------------------------------------------- */

/** DMA receive buffer size */
#ifndef BSP_UART3_RX_BUF_SIZE
#define BSP_UART3_RX_BUF_SIZE       512
#endif

/** Ring buffer size (must be >= RX_BUF_SIZE) */
#ifndef BSP_UART3_RING_BUF_SIZE
#define BSP_UART3_RING_BUF_SIZE     (BSP_UART3_RX_BUF_SIZE * 2)
#endif

/* ---------- Exported serial instance ------------------------------------- */

/**
 * @brief  USART3 serial instance
 */
extern bsp_serial_t *const bsp_serial_uart3;

/* ---------- RX data hook (for CMUX or other protocols) ------------------- */

/**
 * @brief  RX data hook callback type.
 *         When set, ISR calls this instead of writing to ring buffer.
 *         This allows protocol layers (e.g. CMUX) to intercept raw UART data
 *         without bsp_uart3 knowing about them.
 */
typedef void (*bsp_uart3_rx_hook_t)(const uint8_t *data, uint16_t len);

/**
 * @brief  Set or clear the RX data hook.
 * @param  hook  Callback to receive raw RX data from ISR, or NULL to disable.
 * @note   When hook is set, bsp_serial_uart3->read() returns no data (ring buffer
 *         is bypassed). The hook must handle all received data.
 */
void bsp_uart3_set_rx_hook(bsp_uart3_rx_hook_t hook);

#ifdef __cplusplus
}
#endif

#endif /* BSP_UART3_H */
