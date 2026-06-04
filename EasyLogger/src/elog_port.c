/**
  ******************************************************************************
  * @file    elog_port.c
  * @brief   EasyLogger port layer — LPUART1 DMA output for STM32L4
  ******************************************************************************
  */

#include "elog.h"
#include "main.h"
#include "stm32l4xx_hal.h"
#include "tx_api.h"
#include <string.h>

/* ---------- External handle (from CubeMX-generated usart.c) -------------- */

extern UART_HandleTypeDef hlpuart1;

/* ---------- DMA TX completion semaphore ---------------------------------- */

static TX_SEMAPHORE dma_tx_sem;
static uint8_t port_initialized = 0;

/* ---------- Port init (called once from elog_init) ----------------------- */

/**
 * @brief  Initialize the elog DMA port — create semaphore for TX completion
 * @note   Call this from elog_init() or automatically on first output
 */
void elog_port_dma_init(void)
{
    if (!port_initialized)
    {
        tx_semaphore_create(&dma_tx_sem, "elog DMA TX sem", 0);
        port_initialized = 1;
    }
}

/**
 * @brief  Signal DMA TX complete — called from HAL_UART_TxCpltCallback
 * @note   The actual callback is in bsp_uart3.c, which calls this function
 *         when LPUART1 TX completes.
 */
void elog_port_dma_tx_complete(void)
{
    if (port_initialized)
    {
        tx_semaphore_put(&dma_tx_sem);
    }
}

/* ---------- Port function ------------------------------------------------ */

/**
 * @brief  Output log data to LPUART1 via DMA
 * @note   Uses DMA transmit — the async log thread waits for completion
 *         before returning, ensuring sequential output.
 */
void elog_port_output(const char *data, uint16_t len)
{
    if (len == 0) return;

    if (!port_initialized) elog_port_dma_init();

    /* Start DMA transmit on LPUART1 (PG7/PG8, 115200 baud) */
    if (HAL_UART_Transmit_DMA(&hlpuart1, (uint8_t *)data, len) == HAL_OK)
    {
        /* Wait for DMA transfer complete */
        tx_semaphore_get(&dma_tx_sem, TX_WAIT_FOREVER);
    }
    else
    {
        /* Fallback to blocking transmit if DMA busy */
        HAL_UART_Transmit(&hlpuart1, (uint8_t *)data, len, HAL_MAX_DELAY);
    }
}
