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
#include "bsp_serial.h"

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
 * @brief  Initialize CDC-ECM IP instance and start DHCP
 * @note   Creates a second NX_IP using _ux_network_driver_entry for CDC-ECM.
 *         Idempotent — safe to call multiple times. Can be called before or
 *         after CDC-ECM link-up; DHCP thread will wait for link.
 * @return NX_SUCCESS on success
 */
UINT app_netxduo_ecm_init(void);

/**
 * @brief  Destroy CDC-ECM IP instance and release resources.
 *         Call on USB disconnect to prevent resource leaks on reconnect.
 */
void app_netxduo_ecm_destroy(void);

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
