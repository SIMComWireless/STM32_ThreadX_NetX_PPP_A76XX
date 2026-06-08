/**
  ******************************************************************************
  * @file    a7683e.h
  * @brief   SIMCom A7683E modem driver — AT command layer + PPP dial
  ******************************************************************************
  */

#ifndef A7683E_H
#define A7683E_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "tx_api.h"
#include "bsp_serial.h"

/* ---------- User Configuration ------------------------------------------- */

/** APN for cellular data connection — change per carrier */
#ifndef A7683E_APN
#define A7683E_APN              "cmnet"
#endif

/** PDP context type: "IP", "IPV6", or "IPV4V6" */
#ifndef A7683E_PDP_TYPE
#define A7683E_PDP_TYPE         "IP"
#endif

/** AT command response buffer size */
#define A7683E_RESP_BUF_SIZE    512

/** Default AT command timeout (ms) */
#define A7683E_DEFAULT_TIMEOUT  1000

/** Max retries for critical commands */
#define A7683E_MAX_RETRIES      3

/* ---------- Return codes ------------------------------------------------- */

#define A7683E_OK               0x00
#define A7683E_ERROR            0x01
#define A7683E_TIMEOUT          0x02
#define A7683E_NO_SIM           0x03
#define A7683E_NO_SIGNAL        0x04
#define A7683E_PPP_FAIL         0x05

/* ---------- Public API --------------------------------------------------- */

/**
 * @brief  Set the serial port used for modem communication
 * @param  serial  Pointer to a bsp_serial_t instance (e.g. bsp_serial_uart3)
 * @note   Must be called before a7683e_init()
 */
void a7683e_set_serial(bsp_serial_t *serial);

/**
 * @brief  Power on the A7683E modem via PWR_EN pin toggle sequence
 */
void a7683e_power_on(void);

/**
 * @brief  Power off the A7683E modem
 */
void a7683e_power_off(void);

/**
 * @brief  Initialize modem: check AT, disable echo, verify SIM, set APN
 * @return A7683E_OK on success, error code otherwise
 */
UINT a7683e_init(void);

/**
 * @brief  Activate PDP context and start PPP dial
 * @note   Checks EPS registration, activates PDP context, then dials ATD*99#.
 *         After CONNECT, the serial port switches to raw PPP frame passthrough.
 * @return A7683E_OK on success
 */
UINT a7683e_ppp_dial(void);

/**
 * @brief  Send an AT command and wait for response
 * @param  cmd        AT command string (e.g. "AT\r")
 * @param  resp       Response buffer
 * @param  resp_len   Response buffer size
 * @param  timeout_ms Timeout in ms
 * @return A7683E_OK if "OK" found in response, A7683E_ERROR otherwise
 */
UINT a7683e_send_at(const char *cmd, char *resp, uint16_t resp_len, uint32_t timeout_ms);

/**
 * @brief  Check if modem responded to AT (connection health check)
 * @return A7683E_OK if modem responds
 */
UINT a7683e_check_alive(void);

#ifdef __cplusplus
}
#endif

#endif /* A7683E_H */
