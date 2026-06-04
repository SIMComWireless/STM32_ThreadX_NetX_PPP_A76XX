/**
  ******************************************************************************
  * @file    elog.h
  * @brief   EasyLogger — lightweight embedded logging framework
  ******************************************************************************
  * @note
  *   Inspired by EasyLogger (https://github.com/armink/EasyLogger)
  *   Self-contained implementation for STM32 + ThreadX
  *
  *   Features:
  *   - 6 log levels with color-coded output
  *   - Tag-based filtering
  *   - Printf-style formatting
  *   - Thread-safe via ThreadX mutex
  *   - Async output via ring buffer + dedicated thread
  *   - Runtime log level control
  *
  *   Output format:
  *   [timestamp] LEVEL/TAG: message
  *   Example: [00012345] I/MODEM: AT response OK
  ******************************************************************************
  */

#ifndef ELOG_H
#define ELOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "elog_cfg.h"

/* ---------- Log levels --------------------------------------------------- */

#define ELOG_LVL_ASSERT                 0
#define ELOG_LVL_ERROR                  1
#define ELOG_LVL_WARN                   2
#define ELOG_LVL_INFO                   3
#define ELOG_LVL_DEBUG                  4
#define ELOG_LVL_VERBOSE                5

/* ---------- ANSI color codes --------------------------------------------- */

#if ELOG_COLOR_ENABLE
#define ELOG_COLOR_RED                  "\033[31m"
#define ELOG_COLOR_YELLOW               "\033[33m"
#define ELOG_COLOR_GREEN                "\033[32m"
#define ELOG_COLOR_CYAN                 "\033[36m"
#define ELOG_COLOR_WHITE                "\033[37m"
#define ELOG_COLOR_BLUE                 "\033[34m"
#define ELOG_COLOR_RESET                "\033[0m"
#else
#define ELOG_COLOR_RED                  ""
#define ELOG_COLOR_YELLOW               ""
#define ELOG_COLOR_GREEN                ""
#define ELOG_COLOR_CYAN                 ""
#define ELOG_COLOR_WHITE                ""
#define ELOG_COLOR_BLUE                 ""
#define ELOG_COLOR_RESET                ""
#endif

/* ---------- Core API ----------------------------------------------------- */

/**
 * @brief  Initialize the logger (create mutex, start async thread if enabled)
 * @note   Call after ThreadX kernel is running
 */
void elog_init(void);

/**
 * @brief  Start the logger output (call after init)
 */
void elog_start(void);

/**
 * @brief  Set the runtime output filter level
 * @param  level ELOG_LVL_ASSERT .. ELOG_LVL_VERBOSE
 */
void elog_set_filter_lvl(uint8_t level);

/**
 * @brief  Output a log message (called by macros, not directly)
 * @param  level   Log level
 * @param  tag     Module tag string
 * @param  file    Source file name (__FILE__)
 * @param  line    Source line number (__LINE__)
 * @param  fmt     Printf-style format string
 * @param  ...     Format arguments
 */
void elog_output(uint8_t level, const char *tag, const char *file, long line, const char *fmt, ...);

/**
 * @brief  Raw output without level/tag prefix (for hex dumps etc.)
 * @param  fmt Printf-style format string
 * @param  ... Format arguments
 */
void elog_raw(const char *fmt, ...);

/**
 * @brief  Hex dump output
 * @param  tag  Module tag
 * @param  data Data pointer
 * @param  len  Data length
 */
void elog_hexdump(const char *tag, const void *data, uint16_t len);

/* ---------- Convenience macros ------------------------------------------- */

/* Tag-based log macros — use these in application code */
#define LOG_A(tag, fmt, ...)    elog_output(ELOG_LVL_ASSERT,  tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_E(tag, fmt, ...)    elog_output(ELOG_LVL_ERROR,   tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_W(tag, fmt, ...)    elog_output(ELOG_LVL_WARN,    tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_I(tag, fmt, ...)    elog_output(ELOG_LVL_INFO,    tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_D(tag, fmt, ...)    elog_output(ELOG_LVL_DEBUG,   tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_V(tag, fmt, ...)    elog_output(ELOG_LVL_VERBOSE, tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/* ---------- Assertion ---------------------------------------------------- */

#if ELOG_ASSERT_ENABLE
#define ELOG_ASSERT(EXPR)                                           \
    if (!(EXPR))                                                    \
    {                                                               \
        elog_output(ELOG_LVL_ASSERT, "ASSERT",                      \
                    "(%s) has assert failed at %s:%ld.",            \
                    #EXPR, __FILE__, (long)__LINE__);               \
        while (1) {}                                                \
    }
#else
#define ELOG_ASSERT(EXPR)   ((void)(EXPR))
#endif

#ifdef __cplusplus
}
#endif

#endif /* ELOG_H */
