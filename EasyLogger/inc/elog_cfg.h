/**
  ******************************************************************************
  * @file    elog_cfg.h
  * @brief   EasyLogger configuration for STM32L4 + ThreadX
  ******************************************************************************
  */

#ifndef ELOG_CFG_H
#define ELOG_CFG_H

/* Enable color output (ANSI escape codes) */
#define ELOG_COLOR_ENABLE               1

/* Enable tag-based filtering */
#define ELOG_TAG_ENABLE                 1

/* Output newline character sequence */
#define ELOG_NEWLINE_SIGN               "\r\n"

/* Max log output line length (including tag, level, timestamp, message) */
#define ELOG_LINE_BUF_SIZE              256

/* Max tag string length */
#define ELOG_TAG_MAX_LEN                16

/* Default output level (can be changed at runtime) */
#define ELOG_OUTPUT_LVL                 ELOG_LVL_DEBUG

/* Enable async output mode (recommended for RTOS) */
#define ELOG_ASYNC_MODE_ENABLE          1

/* Async output ring buffer size */
#define ELOG_ASYNC_BUF_SIZE             4096

/* Enable assertion checks */
#define ELOG_ASSERT_ENABLE              1

#endif /* ELOG_CFG_H */
