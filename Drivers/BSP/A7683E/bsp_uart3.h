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

/** DMA receive buffer size (per half-buffer in double-buffer scheme) */
#ifndef BSP_UART3_RX_BUF_SIZE
#define BSP_UART3_RX_BUF_SIZE       512
#endif

/** Ring buffer size — must be >= RX_BUF_SIZE.
 *  4096 bytes absorbs scheduling jitter in ppp_read_thread at 115200 baud. */
#ifndef BSP_UART3_RING_BUF_SIZE
#define BSP_UART3_RING_BUF_SIZE     4096
#endif

/** Set to 1 to enable UART3 RX/TX hex dump logging (development only) */
#ifndef BSP_UART3_DEBUG
#define BSP_UART3_DEBUG             0
#endif

/* ---------- RX hook (for CMUX or other framing protocols) ---------------- */

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

/* ---------- Diagnostics -------------------------------------------------- */

/**
 * @brief  Get UART3 RX drop count — times ring buffer was full and data was lost.
 * @return Cumulative drop count since uart3_init().
 */
uint32_t bsp_uart3_get_rx_drop_count(void);

/* ---------- Exported serial instance ------------------------------------- */

/** USART3 serial instance — call ->init() before use */
extern bsp_serial_t *const bsp_serial_uart3;

#ifdef __cplusplus
}
#endif

#endif /* BSP_UART3_H */
