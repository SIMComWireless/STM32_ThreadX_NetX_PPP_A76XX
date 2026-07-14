/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_netxduo.h
  * @author  MCD Application Team
  * @brief   NetXDuo applicative header file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2020-2021 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __APP_NETXDUO_H__
#define __APP_NETXDUO_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "nx_api.h"
#include "nxd_dns.h"
#include "bsp_serial.h"
#include "net_iperf.h"   /* IPERF_ENABLE, PPP_EVT_*IPERF_DONE */

/* Private includes ----------------------------------------------------------*/

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
UINT MX_NetXDuo_Init(VOID *memory_ptr);

/* USER CODE BEGIN EFP */

/**
 * @brief  Set the serial port used for PPP data transport
 * @param  serial  Pointer to a bsp_serial_t instance (e.g. bsp_serial_uart3)
 * @note   Must be called before MX_NetXDuo_Init() or before PPP starts
 */
void app_netxduo_set_serial(bsp_serial_t *serial);

/**
 * @brief  Create and configure PPP instance — must be called from a thread
 * @note   Call this from the modem thread after a7683e_ppp_dial() succeeds,
 *         before nx_ppp_start().
 * @return NX_SUCCESS on success
 */
UINT app_netxduo_create_ppp(void);

/**
 * @brief  Start PPP negotiation
 * @return NX_SUCCESS on success
 */
UINT app_netxduo_start_ppp(void);

/**
 * @brief  Destroy PPP instance and release all associated resources.
 *         Call on USB disconnect to clean up before reconnection.
 *         After this call, app_netxduo_create_ppp() can be called again.
 */
void app_netxduo_destroy_ppp(void);

/**
 * @brief  Signal that USB modem has been disconnected.
 *         Called from USBX host event callback. Sets an event flag
 *         that unblocks the modem thread to start cleanup.
 */
void app_netxduo_set_reconnect_event(void);

/**
 * @brief  Wait for USB disconnect event (blocking).
 *         Call from modem thread after PPP is running.
 */
void app_netxduo_wait_disconnect(void);

/** Event flag for USB disconnect — modem thread waits on this */
#define MODEM_EVT_USB_DISCONNECT   0x01

/* ---- PPP event flags (shared between PPP core, iperf, NTP, Anjay) ---- */

#define PPP_EVT_LINK_UP     0x01
#define PPP_EVT_LINK_DOWN   0x02
#define PPP_EVT_NTP_DONE    0x10

extern TX_EVENT_FLAGS_GROUP ppp_events;

/* ---- NetX objects — used by iperf, DNS, NTP, Anjay ---- */

extern NX_IP             ip_0;
extern NX_PACKET_POOL    pool_0;

/* ---- PPP teardown flag — iperf threads check before network ops ---- */

extern volatile uint8_t  ppp_destroying;

/* ---- Global DNS client — used by Anjay port for hostname resolution ---- */

extern NX_DNS            dns_client;
extern volatile UINT     dns_client_initialized;

/* ---- Byte pool pointer — used by external modules ---- */

extern TX_BYTE_POOL     *byte_pool_ptr;

/**
 * @brief  Get the active IP instance (returns &ip_0)
 */
NX_IP *app_netxduo_get_active_ip(void);

/**
 * @brief  Get the active packet pool (returns &pool_0)
 */
NX_PACKET_POOL *app_netxduo_get_active_pool(void);

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

#ifdef __cplusplus
}
#endif
#endif /* __APP_NETXDUO_H__ */
