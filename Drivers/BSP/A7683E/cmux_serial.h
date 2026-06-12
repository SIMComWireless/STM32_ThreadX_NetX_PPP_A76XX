/**
  ******************************************************************************
  * @file    cmux_serial.h
  * @brief   CMUX virtual serial ports — bsp_serial_t for each data DLCI
  *
  * Data channels: DLCI 1 ~ CMUX_NUM_CHANNELS (DLCI 0 is control).
  * Each data DLCI gets its own ring buffer + event flags + bsp_serial_t.
  ******************************************************************************
  */

#ifndef CMUX_SERIAL_H
#define CMUX_SERIAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "bsp_serial.h"
#include "cmux.h"

#if CMUX_ENABLE

/**
 * @brief  Get the bsp_serial_t for a data DLCI (1 ~ CMUX_NUM_CHANNELS).
 * @param  dlci  DLCI number (1-based).
 * @return Pointer to bsp_serial_t, or NULL if dlci is out of range.
 */
bsp_serial_t *cmux_serial_get(uint8_t dlci);

/**
 * @brief  Initialize all CMUX data serial ports and register DLCI callbacks.
 *         Call after a7683e_cmux_start() succeeds.
 */
void cmux_serial_init_all(void);

#endif /* CMUX_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* CMUX_SERIAL_H */
