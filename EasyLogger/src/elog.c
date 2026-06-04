/**
  ******************************************************************************
  * @file    elog.c
  * @brief   EasyLogger core implementation
  ******************************************************************************
  */

#include "elog.h"
#include "tx_api.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* ---------- Level labels ------------------------------------------------- */

static const char *level_labels[] = {
    [ELOG_LVL_ASSERT]  = "A",
    [ELOG_LVL_ERROR]   = "E",
    [ELOG_LVL_WARN]    = "W",
    [ELOG_LVL_INFO]    = "I",
    [ELOG_LVL_DEBUG]   = "D",
    [ELOG_LVL_VERBOSE] = "V",
};

#if ELOG_COLOR_ENABLE
static const char *level_colors[] = {
    [ELOG_LVL_ASSERT]  = ELOG_COLOR_RED,
    [ELOG_LVL_ERROR]   = ELOG_COLOR_RED,
    [ELOG_LVL_WARN]    = ELOG_COLOR_YELLOW,
    [ELOG_LVL_INFO]    = ELOG_COLOR_GREEN,
    [ELOG_LVL_DEBUG]   = ELOG_COLOR_CYAN,
    [ELOG_LVL_VERBOSE] = ELOG_COLOR_WHITE,
};
#endif

/* ---------- Private state ------------------------------------------------ */

static uint8_t  filter_lvl = ELOG_OUTPUT_LVL;
static uint8_t  logger_started = 0;
static TX_MUTEX output_mutex;

/* Async ring buffer */
#if ELOG_ASYNC_MODE_ENABLE
static char     async_buf[ELOG_ASYNC_BUF_SIZE];
static volatile uint16_t async_head;
static volatile uint16_t async_tail;
static TX_THREAD async_thread;
static TX_SEMAPHORE async_sem;

/* Async thread stack — allocated from the TX byte pool or static */
#define ASYNC_THREAD_STACK_SIZE  1024
static uint8_t async_stack[ASYNC_THREAD_STACK_SIZE];
#endif

/* ---------- Port functions (implemented in elog_port.c) ------------------ */

extern void elog_port_output(const char *data, uint16_t len);

/* ---------- Private helpers ---------------------------------------------- */

/**
 * @brief  Get timestamp in milliseconds (from ThreadX tick counter)
 */
static uint32_t get_timestamp(void)
{
    return (uint32_t)tx_time_get();
}

#if ELOG_ASYNC_MODE_ENABLE

/**
 * @brief  Write data into async ring buffer
 */
static void async_write(const char *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
    {
        async_buf[async_head] = data[i];
        async_head = (async_head + 1) % ELOG_ASYNC_BUF_SIZE;
    }
}

/**
 * @brief  Read data from async ring buffer
 */
static uint16_t async_read(char *buf, uint16_t max_len)
{
    uint16_t count = 0;
    while (count < max_len && async_tail != async_head)
    {
        buf[count++] = async_buf[async_tail];
        async_tail = (async_tail + 1) % ELOG_ASYNC_BUF_SIZE;
    }
    return count;
}

/**
 * @brief  Async output thread — drains ring buffer to physical output
 */
static void async_thread_entry(ULONG param)
{
    char drain_buf[128];
    (void)param;

    while (1)
    {
        /* Wait for data to be available */
        tx_semaphore_get(&async_sem, TX_WAIT_FOREVER);

        /* Drain all available data */
        uint16_t n;
        while ((n = async_read(drain_buf, sizeof(drain_buf))) > 0)
        {
            elog_port_output(drain_buf, n);
        }
    }
}

#endif /* ELOG_ASYNC_MODE_ENABLE */

/* ---------- Public API Implementation ------------------------------------ */

void elog_init(void)
{
    /* Create output mutex */
    tx_mutex_create(&output_mutex, "elog mutex", TX_INHERIT);

    async_head = 0;
    async_tail = 0;

#if ELOG_ASYNC_MODE_ENABLE
    /* Create async semaphore */
    tx_semaphore_create(&async_sem, "elog async sem", 0);

    /* Create async output thread at low priority */
    tx_thread_create(&async_thread, "elog async",
                     async_thread_entry, 0,
                     async_stack, ASYNC_THREAD_STACK_SIZE,
                     30, 30, TX_NO_TIME_SLICE, TX_AUTO_START);
#endif
}

void elog_start(void)
{
    logger_started = 1;

    /* Print banner */
    elog_output(ELOG_LVL_INFO, "ELOG", __FILE__, __LINE__,
                "EasyLogger initialized (async=%d, color=%d)",
                ELOG_ASYNC_MODE_ENABLE, ELOG_COLOR_ENABLE);
}

void elog_set_filter_lvl(uint8_t level)
{
    if (level > ELOG_LVL_VERBOSE) level = ELOG_LVL_VERBOSE;
    filter_lvl = level;
    elog_output(ELOG_LVL_INFO, "ELOG", __FILE__, __LINE__,
                "Log level set to %s", level_labels[level]);
}

void elog_output(uint8_t level, const char *tag, const char *file, long line, const char *fmt, ...)
{
    if (!logger_started) return;
    if (level > filter_lvl) return;

    char line_buf[ELOG_LINE_BUF_SIZE];
    int  pos = 0;

    /* Lock output */
    tx_mutex_get(&output_mutex, TX_WAIT_FOREVER);

    /* Build the log line */
#if ELOG_COLOR_ENABLE
    pos += snprintf(line_buf + pos, sizeof(line_buf) - pos,
                    "%s", level_colors[level]);
#endif

    /* Timestamp */
    pos += snprintf(line_buf + pos, sizeof(line_buf) - pos,
                    "[%010lu] ", (unsigned long)get_timestamp());

    /* Level tag */
    pos += snprintf(line_buf + pos, sizeof(line_buf) - pos,
                    "%s/", level_labels[level]);

    /* Module tag (padded to ELOG_TAG_MAX_LEN) */
    pos += snprintf(line_buf + pos, sizeof(line_buf) - pos,
                    "%-*s ", ELOG_TAG_MAX_LEN, tag);

    /* Source file:line — strip path, show only filename */
    {
        const char *fname = file;
        const char *p = file;
        while (*p) { if (*p == '/' || *p == '\\') fname = p + 1; p++; }
        pos += snprintf(line_buf + pos, sizeof(line_buf) - pos,
                        "%s:%ld: ", fname, line);
    }

    /* Message body */
    va_list args;
    va_start(args, fmt);
    pos += vsnprintf(line_buf + pos, sizeof(line_buf) - pos, fmt, args);
    va_end(args);

    /* Newline + color reset */
    pos += snprintf(line_buf + pos, sizeof(line_buf) - pos,
                    "%s%s", ELOG_NEWLINE_SIGN,
                    ELOG_COLOR_RESET ? ELOG_COLOR_RESET : "");

    /* Output */
#if ELOG_ASYNC_MODE_ENABLE
    async_write(line_buf, pos);
    tx_semaphore_put(&async_sem);
#else
    elog_port_output(line_buf, pos);
#endif

    tx_mutex_put(&output_mutex);
}

void elog_raw(const char *fmt, ...)
{
    char buf[ELOG_LINE_BUF_SIZE];
    va_list args;

    tx_mutex_get(&output_mutex, TX_WAIT_FOREVER);

    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len > 0)
    {
#if ELOG_ASYNC_MODE_ENABLE
        async_write(buf, len);
        tx_semaphore_put(&async_sem);
#else
        elog_port_output(buf, len);
#endif
    }

    tx_mutex_put(&output_mutex);
}

void elog_hexdump(const char *tag, const void *data, uint16_t len)
{
    if (!logger_started) return;
    if (ELOG_LVL_DEBUG > filter_lvl) return;

    const uint8_t *p = (const uint8_t *)data;
    char line[80];
    int pos;

    tx_mutex_get(&output_mutex, TX_WAIT_FOREVER);

    for (uint16_t i = 0; i < len; i += 16)
    {
        pos = snprintf(line, sizeof(line), "[%010lu] D/%-*s ",
                       (unsigned long)get_timestamp(), ELOG_TAG_MAX_LEN, tag);

        /* Hex bytes */
        for (uint16_t j = 0; j < 16; j++)
        {
            if (i + j < len)
                pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", p[i + j]);
            else
                pos += snprintf(line + pos, sizeof(line) - pos, "   ");
        }

        /* ASCII */
        pos += snprintf(line + pos, sizeof(line) - pos, " |");
        for (uint16_t j = 0; j < 16 && (i + j) < len; j++)
        {
            uint8_t c = p[i + j];
            pos += snprintf(line + pos, sizeof(line) - pos, "%c",
                            (c >= 0x20 && c <= 0x7E) ? c : '.');
        }
        pos += snprintf(line + pos, sizeof(line) - pos, "|%s", ELOG_NEWLINE_SIGN);

#if ELOG_ASYNC_MODE_ENABLE
        async_write(line, pos);
        tx_semaphore_put(&async_sem);
#else
        elog_port_output(line, pos);
#endif
    }

    tx_mutex_put(&output_mutex);
}
