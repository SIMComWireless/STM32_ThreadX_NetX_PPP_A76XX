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
#include "cmux.h"

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

/* ---------- AT send function type ---------------------------------------- */

/**
 * @brief  Function pointer type for sending AT commands.
 *         This abstraction allows pre_dial/ppp_dial to work with both
 *         direct UART (a7683e_send_at) and CMUX (a7683e_cmux_send_at).
 */
typedef UINT (*at_send_fn_t)(const char *cmd, char *resp, uint16_t resp_len, uint32_t timeout_ms);

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
 * @brief  Escape from CMUX mode if modem is stuck after MCU reset.
 *         Detects CMUX framing (0xF9, SIMCom variant), sends DISC on DLCI 0/1/2,
 *         waits for modem to auto-revert to AT mode.
 * @return A7683E_OK if CMUX was detected and escaped,
 *         A7683E_TIMEOUT if not in CMUX mode (normal),
 *         A7683E_ERROR on failure.
 */
UINT a7683e_escape_cmux(void);

/**
 * @brief  Escape from PPP data mode (+++ guard time sequence).
 * @return A7683E_OK if modem was in PPP and escaped,
 *         A7683E_TIMEOUT if not in PPP mode (normal),
 *         A7683E_ERROR on failure.
 */
UINT a7683e_escape_ppp(void);

/**
 * @brief  Synchronize with modem by sending AT up to 5 times.
 * @return A7683E_OK if modem responded, A7683E_ERROR otherwise.
 */
UINT a7683e_sync(void);

/**
 * @brief  Initialize modem: escape PPP, sync, disable echo, verify SIM,
 *         check signal, set APN, wait for network registration.
 * @return A7683E_OK on success, error code otherwise
 */
UINT a7683e_init(void);

/**
 * @brief  Pre-dial queries: check EPS registration, activate PDP context,
 *         verify PDP address. Call before a7683e_ppp_dial().
 * @param  send_at  AT send function (a7683e_send_at or a7683e_cmux_send_at)
 * @return A7683E_OK on success
 */
UINT a7683e_pre_dial(at_send_fn_t send_at);

/**
 * @brief  PPP dial via direct UART: send ATD*99***1# and wait for CONNECT.
 * @note   For CMUX path, use a7683e_cmux_send_at("ATD*99***1#", ...) directly.
 * @return A7683E_OK on success
 */
UINT a7683e_ppp_dial(void);

/**
 * @brief  Send an AT command and wait for response (direct UART)
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

#if CMUX_ENABLE

/**
 * @brief  Start CMUX mode. Call after a7683e_init() succeeds.
 *         Sends AT+CMUX=0, establishes DLCI 0/1/2 channels.
 * @return A7683E_OK on success
 */
UINT a7683e_cmux_start(void);

/**
 * @brief  Send AT command via CMUX and wait for response.
 * @param  dlci       CMUX channel (1=AT commands, 2=PPP dial)
 * @param  cmd        AT command string (\r optional — auto-appended if missing)
 * @param  resp       Response buffer
 * @param  resp_len   Response buffer size
 * @param  timeout_ms Timeout in ms
 * @return A7683E_OK on success
 */
UINT a7683e_cmux_send_at_dlci(uint8_t dlci, const char *cmd, char *resp,
                               uint16_t resp_len, uint32_t timeout_ms);

/**
 * @brief  Send AT command via CMUX DLCI 1 (at_send_fn_t compatible).
 *         Used as function pointer for a7683e_pre_dial().
 */
UINT a7683e_cmux_send_at(const char *cmd, char *resp, uint16_t resp_len, uint32_t timeout_ms);

/**
 * @brief  Send AT command via CMUX DLCI 2 (at_send_fn_t compatible).
 *         Used for pre-dial queries on the PPP channel.
 */
UINT a7683e_cmux_send_at_dlci2(const char *cmd, char *resp, uint16_t resp_len, uint32_t timeout_ms);

#endif /* CMUX_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* A7683E_H */
