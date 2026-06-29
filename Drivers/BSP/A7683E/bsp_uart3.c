/**
  ******************************************************************************
  * @file    bsp_uart3.c
  * @brief   USART3 DMA driver — bsp_serial_t implementation
  ******************************************************************************
  * @note
  *   Uses HAL_UARTEx_ReceiveToIdle_DMA for reception:
  *   - DMA receives into double-buffer, toggled on each callback
  *   - Idle-line or buffer-full triggers HAL_UARTEx_RxEventCallback
  *   - Callback pushes data into ring buffer, restarts DMA immediately
  *   - Application reads from ring buffer via bsp_serial_t::read()
  *
  *   An optional RX hook can intercept raw ISR data before the ring buffer
  *   (used by CMUX or other framing protocols).
  ******************************************************************************
  */

#include "bsp_uart3.h"
#include "lwrb.h"
#include "tx_api.h"
#include "stm32l4xx_hal.h"
#include "elog.h"
#include <string.h>

/* ---------- External handles (from CubeMX-generated code) ---------------- */

extern UART_HandleTypeDef huart3;
extern DMA_HandleTypeDef  hdma_usart3_rx;
extern DMA_HandleTypeDef  hdma_usart3_tx;

/* ---------- Private state ------------------------------------------------ */

/* Double DMA receive buffers — toggled on each callback */
static uint8_t rx_dma_buf[2][BSP_UART3_RX_BUF_SIZE];
static volatile uint8_t rx_active_buf;  /* Index of buffer currently used by DMA */

/* Ring buffer — ISR writes, application thread reads */
static uint8_t uart3_rb_data[BSP_UART3_RING_BUF_SIZE];
static lwrb_t  uart3_rb;

/* RX data hook — when set, ISR routes data to hook instead of ring buffer */
static bsp_uart3_rx_hook_t rx_hook;

/* RX overflow counter — incremented in ISR when ring buffer is full.
 * Readable from application context via bsp_uart3_get_rx_drop_count().
 * No lock needed: single-writer (ISR) on Cortex-M4 is naturally atomic for uint32. */
static volatile uint32_t rx_drop_count;

/* ThreadX primitives */
static TX_EVENT_FLAGS_GROUP rx_events;
static TX_SEMAPHORE tx_sem;

#define EVT_DATA    0x01   /* Data available in ring buffer */

static volatile uint8_t initialized = 0;

/* ---------- RX hook ------------------------------------------------------ */

void bsp_uart3_set_rx_hook(bsp_uart3_rx_hook_t hook)
{
    rx_hook = hook;
}

/* ---------- Diagnostics -------------------------------------------------- */

uint32_t bsp_uart3_get_rx_drop_count(void)
{
    return rx_drop_count;
}

/* ---------- HAL Callbacks (ISR context) ---------------------------------- */

/**
 * @brief  RX Event callback — called by HAL on idle-line, HT, or TC.
 * @note   Double-buffer strategy:
 *         1. DMA writes into rx_dma_buf[rx_active_buf]
 *         2. If RX hook is set, route data to hook;
 *            otherwise push into ring buffer for bsp_serial_t read.
 *         3. Switch to the other buffer and restart DMA.
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if (huart->Instance != USART3)
        return;

    if (Size > 0)
    {
#if BSP_UART3_DEBUG
        elog_hexdump("U3RX", 16, rx_dma_buf[rx_active_buf], Size);
#endif

        if (rx_hook)
        {
            /* Hook installed — route raw data to protocol layer */
            rx_hook(rx_dma_buf[rx_active_buf], Size);
        }
        else
        {
            /* No hook — push into ring buffer for serial read */
            size_t written = lwrb_write(&uart3_rb, rx_dma_buf[rx_active_buf], Size);
            if (written < Size)
                rx_drop_count++;
            tx_event_flags_set(&rx_events, EVT_DATA, TX_OR);
        }
    }

    /* Switch to the other buffer and restart DMA */
    rx_active_buf ^= 1;
    HAL_UARTEx_ReceiveToIdle_DMA(&huart3, rx_dma_buf[rx_active_buf],
                                  BSP_UART3_RX_BUF_SIZE);

    /* Disable half-transfer interrupt — only idle-line and TC events matter */
    __HAL_DMA_DISABLE_IT(huart3.hdmarx, DMA_IT_HT);
}

/**
 * @brief  DMA TX complete callback — dispatches USART3 and LPUART1.
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3)
    {
        tx_semaphore_put(&tx_sem);
    }
    else if (huart->Instance == LPUART1)
    {
        extern void elog_port_dma_tx_complete(void);
        elog_port_dma_tx_complete();
    }
}

/* ---------- bsp_serial_t vtable implementation --------------------------- */

static void uart3_init(bsp_serial_t *self)
{
    (void)self;
    if (initialized) return;

    tx_event_flags_create(&rx_events, "UART3 RX Events");
    tx_semaphore_create(&tx_sem, "UART3 TX Sem", 1);

    lwrb_init(&uart3_rb, uart3_rb_data, BSP_UART3_RING_BUF_SIZE);
    rx_active_buf = 0;
    rx_drop_count = 0;

    memset(rx_dma_buf[0], 0, BSP_UART3_RX_BUF_SIZE);
    memset(rx_dma_buf[1], 0, BSP_UART3_RX_BUF_SIZE);

    /* Start DMA reception on buffer 0 with idle-line detection */
    HAL_UARTEx_ReceiveToIdle_DMA(&huart3, rx_dma_buf[0], BSP_UART3_RX_BUF_SIZE);

    /* Disable half-transfer interrupt — we only want idle and TC events */
    __HAL_DMA_DISABLE_IT(huart3.hdmarx, DMA_IT_HT);

    initialized = 1;
}

static uint16_t uart3_read(bsp_serial_t *self, uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    (void)self;

    /* Non-blocking: return whatever is available immediately */
    uint32_t got = lwrb_read(&uart3_rb, buf, len);
    if (got > 0 || timeout_ms == 0)
        return (uint16_t)got;

    /* Convert ms to ticks */
    ULONG ticks = (timeout_ms == 0xFFFFFFFF) ? TX_WAIT_FOREVER
                  : (timeout_ms * TX_TIMER_TICKS_PER_SECOND / 1000);
    if (ticks == 0)
        ticks = 1;

    /* Wait for data notification from ISR */
    ULONG flags;
    if (tx_event_flags_get(&rx_events, EVT_DATA, TX_OR_CLEAR, &flags, ticks) != TX_SUCCESS)
        return 0;

    return (uint16_t)lwrb_read(&uart3_rb, buf, len);
}

static void uart3_write(bsp_serial_t *self, const uint8_t *data, uint16_t len)
{
    (void)self;
    if (len == 0) return;

#if BSP_UART3_DEBUG
    elog_hexdump("U3TX", 16, data, len);
#endif

    /* Wait for any previous TX DMA to complete */
    tx_semaphore_get(&tx_sem, TX_WAIT_FOREVER);

    if (HAL_UART_Transmit_DMA(&huart3, (uint8_t *)data, len) != HAL_OK)
    {
        /* DMA busy — fall back to blocking TX, then release semaphore */
        HAL_UART_Transmit(&huart3, (uint8_t *)data, len, HAL_MAX_DELAY);
        tx_semaphore_put(&tx_sem);
        return;
    }

    /* DMA started — HAL_UART_TxCpltCallback will release semaphore */
}

static uint16_t uart3_rx_available(bsp_serial_t *self)
{
    (void)self;
    return (uint16_t)lwrb_get_full(&uart3_rb);
}

/* ---------- Exported serial instance ------------------------------------- */

static bsp_serial_t _serial_uart3 = {
    .name         = "UART3",
    .init         = uart3_init,
    .read         = uart3_read,
    .write        = uart3_write,
    .rx_available = uart3_rx_available,
};

bsp_serial_t *const bsp_serial_uart3 = &_serial_uart3;
