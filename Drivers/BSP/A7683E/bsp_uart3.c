/**
  ******************************************************************************
  * @file    bsp_uart3.c
  * @brief   USART3 DMA driver — bsp_serial_t implementation
  ******************************************************************************
  * @note
  *   Uses HAL_UARTEx_ReceiveToIdle_DMA for reception:
  *   - DMA receives data into buffer
  *   - Automatically stops on idle-line detection OR buffer full
  *   - HAL_UARTEx_RxEventCallback fires with received byte count
  *   - Callback restarts reception immediately
  *   - Ring buffer stores data for consumer thread
  ******************************************************************************
  */

#include "bsp_uart3.h"
#include "tx_api.h"
#include "stm32l4xx_hal.h"
#include <string.h>

/* ---------- External handles (from CubeMX-generated code) ---------------- */

extern UART_HandleTypeDef huart3;
extern DMA_HandleTypeDef  hdma_usart3_rx;
extern DMA_HandleTypeDef  hdma_usart3_tx;

/* ---------- Private state ------------------------------------------------ */

/* Double DMA receive buffers — toggled on each callback */
static uint8_t rx_dma_buf[2][BSP_UART3_RX_BUF_SIZE];
static volatile uint8_t rx_active_buf;  /* Index of buffer currently used by DMA */

/* Ring buffer — ISR writes, application reads */
static uint8_t  ring_buf[BSP_UART3_RING_BUF_SIZE];
static volatile uint16_t ring_head;
static volatile uint16_t ring_tail;

/* ThreadX primitives */
static TX_SEMAPHORE         tx_done_sem;
static TX_EVENT_FLAGS_GROUP rx_events;

#define EVT_DATA    0x01   /* Data available in ring buffer */

static uint8_t initialized = 0;

/* ---------- Ring buffer -------------------------------------------------- */

static uint16_t ring_count(void)
{
    uint16_t h = ring_head;
    uint16_t t = ring_tail;
    return (h >= t) ? (h - t) : (sizeof(ring_buf) - t + h);
}

static void ring_write(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
    {
        ring_buf[ring_head] = data[i];
        ring_head = (ring_head + 1) % sizeof(ring_buf);
    }
}

static uint16_t ring_read(uint8_t *buf, uint16_t max_len)
{
    uint16_t count = 0;
    while (count < max_len && ring_tail != ring_head)
    {
        buf[count++] = ring_buf[ring_tail];
        ring_tail = (ring_tail + 1) % sizeof(ring_buf);
    }
    return count;
}

/* ---------- HAL Callbacks ------------------------------------------------ */

/**
 * @brief  RX Event callback — called by HAL on idle-line, HT, or TC
 * @note   Double-buffer strategy:
 *         1. DMA writes into rx_dma_buf[rx_active_buf]
 *         2. On callback, push that buffer's data into ring buffer
 *         3. Switch to the other buffer for next DMA reception
 *         This way DMA never writes into a buffer being read by consumer.
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if (huart->Instance == USART3)
    {
        /* Push received bytes from active buffer into ring buffer */
        if (Size > 0)
        {
            ring_write(rx_dma_buf[rx_active_buf], Size);
            tx_event_flags_set(&rx_events, EVT_DATA, TX_OR);
        }

        /* Switch to the other buffer for next reception */
        rx_active_buf ^= 1;

        /* Restart DMA reception on the new buffer */
        HAL_UARTEx_ReceiveToIdle_DMA(&huart3, rx_dma_buf[rx_active_buf],
                                      BSP_UART3_RX_BUF_SIZE);

        /* Disable half-transfer interrupt to reduce callback noise.
         * We only care about idle-line and full-buffer events. */
        __HAL_DMA_DISABLE_IT(huart3.hdmarx, DMA_IT_HT);
    }
}

/**
 * @brief  DMA RX complete callback — fallback for when ReceiveToIdle is not used
 * @note   This is called if DMA completes without idle detection.
 *         With ReceiveToIdle_DMA, the RxEventCallback handles this case,
 *         but we keep this as a safety net.
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    /* Not expected when using ReceiveToIdle_DMA — RxEventCallback handles it */
    (void)huart;
}

/**
 * @brief  DMA TX complete callback — handles both USART3 and LPUART1
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3)
    {
        tx_semaphore_put(&tx_done_sem);
    }
    else if (huart->Instance == LPUART1)
    {
        extern void elog_port_dma_tx_complete(void);
        elog_port_dma_tx_complete();
    }
}

/* ---------- bsp_serial_t implementation ---------------------------------- */

static void uart3_init(void)
{
    if (initialized) return;

    tx_semaphore_create(&tx_done_sem, "UART3 TX Done", 0);
    tx_event_flags_create(&rx_events, "UART3 RX Events");

    ring_head = 0;
    ring_tail = 0;
    rx_active_buf = 0;

    memset(rx_dma_buf[0], 0, BSP_UART3_RX_BUF_SIZE);
    memset(rx_dma_buf[1], 0, BSP_UART3_RX_BUF_SIZE);

    /* Start DMA reception on buffer 0 with idle-line detection */
    HAL_UARTEx_ReceiveToIdle_DMA(&huart3, rx_dma_buf[0], BSP_UART3_RX_BUF_SIZE);

    /* Disable half-transfer interrupt — we only want idle and TC events */
    __HAL_DMA_DISABLE_IT(huart3.hdmarx, DMA_IT_HT);

    initialized = 1;
}

static uint16_t uart3_read(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    uint16_t total_read = 0;
    ULONG tx_timeout = (timeout_ms == 0xFFFFFFFF) ? TX_WAIT_FOREVER : timeout_ms;

    while (total_read < len)
    {
        uint16_t got = ring_read(buf + total_read, len - total_read);
        total_read += got;

        if (total_read >= len) break;

        if (timeout_ms == 0) break;

        if (total_read > 0) break;

        /* Wait for data */
        ULONG actual_flags;
        UINT status = tx_event_flags_get(&rx_events, EVT_DATA,
                                          TX_OR_CLEAR, &actual_flags, tx_timeout);
        if (status != TX_SUCCESS) break;
    }

    return total_read;
}

static void uart3_write(const uint8_t *data, uint16_t len)
{
    if (len == 0) return;

    if (HAL_UART_Transmit_DMA(&huart3, (uint8_t *)data, len) == HAL_OK)
    {
        /* Wait for DMA TX complete callback */
        tx_semaphore_get(&tx_done_sem, TX_WAIT_FOREVER);
    }
    else
    {
        /* Fallback: DMA busy or state error — use blocking transmit */
        HAL_UART_AbortTransmit(&huart3);
        HAL_UART_Transmit(&huart3, (uint8_t *)data, len, HAL_MAX_DELAY);
    }
}

static uint16_t uart3_rx_available(void)
{
    return ring_count();
}

/* ---------- Static serial instance --------------------------------------- */

static bsp_serial_t _serial_uart3 = {
    .name         = "UART3",
    .init         = uart3_init,
    .read         = uart3_read,
    .write        = uart3_write,
    .rx_available = uart3_rx_available,
};

bsp_serial_t *const bsp_serial_uart3 = &_serial_uart3;
