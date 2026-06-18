/*
 * This file is part of the EasyLogger Library.
 *
 * Copyright (c) 2015, Armink, <armink.ztl@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * 'Software'), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Function: Portable interface for each platform.
 * Created on: 2015-04-28
 */

#include <elog.h>
#include <stdio.h>
#include "tx_api.h"
#include "stm32l4xx_hal.h"
#include "rtc.h"

/* ---------- External handles --------------------------------------------- */

extern UART_HandleTypeDef hlpuart1;

/* ---------- ThreadX primitives ------------------------------------------- */

/* Output lock — protects log output from concurrent access (mutex with
 * priority inheritance to avoid priority inversion between threads) */
static TX_MUTEX elog_lock_mutex;

/* Async notice — wakes the async output thread when new log arrives */
TX_SEMAPHORE elog_async_sem;

/* DMA completion — posted by HAL_UART_TxCpltCallback when DMA finishes */
static TX_SEMAPHORE elog_dma_sem;

/**
 * EasyLogger port initialize
 *
 * @return result
 */
ElogErrCode elog_port_init(void) {
    ElogErrCode result = ELOG_NO_ERR;

    tx_mutex_create(&elog_lock_mutex, "ELog Lock", TX_INHERIT);
    tx_semaphore_create(&elog_async_sem, "ELog Async", 0);
    tx_semaphore_create(&elog_dma_sem,   "ELog DMA",   1);

    return result;
}

/**
 * EasyLogger port deinitialize
 *
 */
void elog_port_deinit(void) {
    tx_mutex_delete(&elog_lock_mutex);
    tx_semaphore_delete(&elog_async_sem);
    tx_semaphore_delete(&elog_dma_sem);
}

/**
 * output log port interface
 *
 * @param log output of log
 * @param size log size
 */
void elog_port_output(const char *log, size_t size) {
    /* Wait for any previous DMA transfer to complete */
    tx_semaphore_get(&elog_dma_sem, TX_WAIT_FOREVER);

    if (HAL_UART_Transmit_DMA(&hlpuart1, (uint8_t *)log, size) != HAL_OK) {
        /* DMA busy — fall back to blocking TX */
        HAL_UART_Transmit(&hlpuart1, (uint8_t *)log, size, HAL_MAX_DELAY);
        tx_semaphore_put(&elog_dma_sem);
    } else {
        /* DMA started — wait for completion before returning so that the
         * caller's buffer (log_buf or poll_get_buf) remains valid for the
         * entire duration of the transfer.  The lock is held during this
         * wait, preventing other threads from overwriting the buffer.
         *
         * Use a 1-second timeout as a safety net: if the DMA transfer
         * stalls (hardware fault, etc.) we fall back to blocking TX
         * instead of hanging forever. */
        if (tx_semaphore_get(&elog_dma_sem, TX_TIMER_TICKS_PER_SECOND) != TX_SUCCESS) {
            /* DMA did not complete in time — abort and fall back */
            HAL_UART_AbortTransmit(&hlpuart1);
            HAL_UART_Transmit(&hlpuart1, (uint8_t *)log, size, HAL_MAX_DELAY);
            tx_semaphore_put(&elog_dma_sem);
        } else {
            tx_semaphore_put(&elog_dma_sem);
        }
    }
}

/**
 * DMA TX complete callback for LPUART1 — called from HAL IRQ context
 */
void elog_port_dma_tx_complete(void) {
    tx_semaphore_put(&elog_dma_sem);
}

/**
 * output lock
 */
void elog_port_output_lock(void) {
    tx_mutex_get(&elog_lock_mutex, TX_WAIT_FOREVER);
}

/**
 * output unlock
 */
void elog_port_output_unlock(void) {
    tx_mutex_put(&elog_lock_mutex);
}

/**
 * get current time interface
 *
 * @return current time in HH:MM:SS format from RTC
 */
const char *elog_port_get_time(void) {
    static char cur_system_time[16] = "";
    RTC_TimeTypeDef sTime;
    RTC_DateTypeDef sDate;

    HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN);  /* must read date to unlock shadow reg */

    snprintf(cur_system_time, sizeof(cur_system_time), "%02u:%02u:%02u",
             sTime.Hours, sTime.Minutes, sTime.Seconds);
    return cur_system_time;
}

/**
 * get current process name interface
 *
 * @return current process name
 */
const char *elog_port_get_p_info(void) {
    return "";
}

/**
 * get current thread name interface
 *
 * @return current thread name
 */
const char *elog_port_get_t_info(void) {
    return "";
}

/**
 * asynchronous output notice — called by elog_async_output() when new log
 * is placed in the ring buffer. Wakes the async output thread.
 */
void elog_async_output_notice(void) {
    tx_semaphore_put(&elog_async_sem);
}

/**
 * EasyLogger async output thread entry
 *
 * @note  Create this thread in your RTOS init code. Example (ThreadX):
 *        tx_thread_create(&elog_thread, "elog async", elog_entry, 0,
 *                         elog_stack, ELOG_THREAD_STACK_SIZE,
 *                         ELOG_THREAD_PRIO, ELOG_THREAD_PRIO,
 *                         TX_NO_TIME_SLICE, TX_AUTO_START);
 */
void elog_entry(ULONG param) {
    (void)param;
    size_t get_log_size = 0;
#ifdef ELOG_ASYNC_LINE_OUTPUT
    static char poll_get_buf[ELOG_LINE_BUF_SIZE - 4];
#else
    static char poll_get_buf[ELOG_ASYNC_OUTPUT_BUF_SIZE - 4];
#endif

    for (;;) {
        /* Waiting for log notice */
        tx_semaphore_get(&elog_async_sem, TX_WAIT_FOREVER);
        /* Polling gets and outputs the log */
        while (1) {
#ifdef ELOG_ASYNC_LINE_OUTPUT
            get_log_size = elog_async_get_line_log(poll_get_buf, sizeof(poll_get_buf));
#else
            get_log_size = elog_async_get_log(poll_get_buf, sizeof(poll_get_buf));
#endif
            if (get_log_size) {
                elog_port_output(poll_get_buf, get_log_size);
            } else {
                break;
            }
        }
    }
}
