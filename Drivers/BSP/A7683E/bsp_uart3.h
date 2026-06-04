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

/* ---------- Configuration ------------------------------------------------- */

/** DMA receive buffer size */
#ifndef BSP_UART3_RX_BUF_SIZE
#define BSP_UART3_RX_BUF_SIZE       256
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

#ifdef __cplusplus
}
#endif

#endif /* BSP_UART3_H */
