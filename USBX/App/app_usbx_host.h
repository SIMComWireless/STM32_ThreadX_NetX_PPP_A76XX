/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_usbx_host.h
  * @author  MCD Application Team
  * @brief   USBX Host applicative header file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2021 STMicroelectronics.
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
#ifndef __APP_USBX_HOST_H__
#define __APP_USBX_HOST_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "ux_api.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "ux_system.h"
#include "ux_utility.h"
#include "ux_hcd_stm32.h"
#include "ux_host_class_modem.h"
#include "main.h"
#include "usb_otg.h"
#include "elog.h"
/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */
#define TAG_USBX  "USBX"
#define USBH_UsrLog(...)   elog_i(TAG_USBX, __VA_ARGS__)
#define USBH_ErrLog(...)   elog_e(TAG_USBX, __VA_ARGS__)

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
UINT MX_USBX_Host_Init(VOID *memory_ptr);

/* USER CODE BEGIN EFP */
UINT  MX_USB_Host_Init(void);
void  USBH_DriverVBUS(uint8_t state);
void  usbx_app_thread_entry(ULONG arg);
VOID  ux_host_error_callback(UINT system_level, UINT system_context, UINT error_code);
UINT  ux_host_event_callback(ULONG event, UX_HOST_CLASS *p_host_class, VOID *p_instance);
/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define APP_QUEUE_SIZE                               5
#define USBX_APP_STACK_SIZE                          1024
#define USBX_MEMORY_SIZE                             (96 * 1024)
#define NEW_RECEIVED_DATA                            0x01
#define NEW_DATA_TO_SEND                             0x02
#define BUTTON_KEY_PIN                               1

/* USB modem interface numbers — adjust per A7683E variant */
#ifndef A7683E_USB_AT_IFNUM
#define A7683E_USB_AT_IFNUM                          4
#endif
#ifndef A7683E_USB_MODEM_IFNUM
#define A7683E_USB_MODEM_IFNUM                       5
#endif

/* USER CODE END PD */

/* USER CODE BEGIN 1 */

typedef enum
{
  USB_VBUS_FALSE = 0,
  USB_VBUS_TRUE,
} USB_VBUS_State;

typedef enum
{
  Modem_Device = 1,
  CDC_ACM_Device,
  CDC_ECM_Device,
  Unsupported_Device,
  Unknown_Device,
} USB_Device_Type;

typedef enum
{
  Device_disconnected = 1,
  Device_connected,
  No_Device,
} Device_state;

typedef enum
{
  App_Ready = 1,
  App_Start,
  App_Idle,
} ux_app_stateTypeDef;

typedef struct
{
  USB_Device_Type Device_Type;
  Device_state    Dev_state;
} ux_app_devInfotypeDef;

/* USER CODE END 1 */

#ifdef __cplusplus
}
#endif
#endif /* __APP_USBX_HOST_H__ */
