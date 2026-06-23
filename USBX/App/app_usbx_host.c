/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_usbx_host.c
  * @author  MCD Application Team
  * @brief   USBX host applicative file
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

/* Includes ------------------------------------------------------------------*/
#include "app_usbx_host.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "ux_host_class_modem.h"
#include "ux_host_class_cdc_ecm.h"
#include "ux_network_driver.h"
#include "a7683e.h"
#if A7683E_TRANSPORT == A7683E_TRANSPORT_USB
#include "bsp_usb.h"
#endif
#include "app_netxduo.h"
#include "elog.h"
/* USER CODE END Includes */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
extern HCD_HandleTypeDef               hhcd_USB_OTG_FS;
TX_QUEUE                               ux_app_MsgQueue;
TX_EVENT_FLAGS_GROUP                   ux_app_EventFlag;
TX_THREAD                              ux_app_thread;
ux_app_stateTypeDef                    ux_app_state;
TX_BYTE_POOL                           *ux_app_byte_pool;

#if defined ( __ICCARM__ ) /* IAR Compiler */
  #pragma data_alignment=4
#endif /* defined ( __ICCARM__ ) */
__ALIGN_BEGIN ux_app_devInfotypeDef     ux_dev_info  __ALIGN_END;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
extern void MX_USB_OTG_FS_USB_Init(void);
extern void Error_Handler(void);
/* USER CODE END PFP */
/**
  * @brief  Application USBX Host Initialization.
  * @param memory_ptr: memory pointer
  * @retval int
  */
UINT MX_USBX_Host_Init(VOID *memory_ptr)
{
  UINT ret = UX_SUCCESS;
  TX_BYTE_POOL *byte_pool = (TX_BYTE_POOL*)memory_ptr;
  UINT status;

  /* USER CODE BEGIN MX_USBX_Host_MEM_POOL */

  /* USER CODE END MX_USBX_Host_MEM_POOL */

  /* USER CODE BEGIN MX_USBX_Host_Init */
  CHAR *pointer;

  /* Store byte_pool into ux_app_byte_pool */
  ux_app_byte_pool = byte_pool;

  /* Allocate memory for USBX system. */
  elog_d(TAG_USBX, "Allocating %u bytes for USBX system...", (unsigned)USBX_MEMORY_SIZE);
  status = tx_byte_allocate(byte_pool, (VOID **) &pointer,
                       USBX_MEMORY_SIZE, TX_NO_WAIT);
  if (status != TX_SUCCESS)
  {
    elog_d(TAG_USBX, "FAIL: tx_byte_allocate (system mem) = 0x%02X", status);
    return TX_POOL_ERROR;
  }
  elog_d(TAG_USBX, "USBX system memory allocated at 0x%08lX", (unsigned long)(void *)pointer);

  /* Initialize USBX memory. */
  elog_d(TAG_USBX, "Calling ux_system_initialize...");
  status = ux_system_initialize(pointer, USBX_MEMORY_SIZE, UX_NULL, 0);
  if (status != UX_SUCCESS)
  {
    elog_d(TAG_USBX, "FAIL: ux_system_initialize = 0x%02X", status);
    return UX_ERROR;
  }
  elog_d(TAG_USBX, "ux_system_initialize OK");

  /* register a callback error function */
  _ux_utility_error_callback_register(&ux_host_error_callback);

  /* Allocate the stack for the main App thread. */
  status = tx_byte_allocate(byte_pool, (VOID **) &pointer,
                       USBX_APP_STACK_SIZE, TX_NO_WAIT);
  if (status != TX_SUCCESS)
  {
    elog_d(TAG_USBX, "FAIL: allocate app thread stack = 0x%02X", status);
    return TX_POOL_ERROR;
  }

  /* Create the main App thread.  */
  status = tx_thread_create(&ux_app_thread, "usbx_app_thread", usbx_app_thread_entry, 0,
                       pointer, USBX_APP_STACK_SIZE, 25, 25, 0,
                       TX_AUTO_START);
  if (status != TX_SUCCESS)
  {
    elog_d(TAG_USBX, "FAIL: create usbx_app_thread = 0x%02X", status);
    return TX_THREAD_ERROR;
  }
  elog_d(TAG_USBX, "usbx_app_thread created");

  /* Allocate Memory for ux_app_MsgQueue */
  status = tx_byte_allocate(byte_pool, (VOID **) &pointer,
                       APP_QUEUE_SIZE * sizeof(ux_app_devInfotypeDef), TX_NO_WAIT);
  if (status != TX_SUCCESS)
  {
    elog_d(TAG_USBX, "FAIL: allocate msg queue = 0x%02X", status);
    return TX_POOL_ERROR;
  }

  /* Create ux_app_MsgQueue */
  status = tx_queue_create(&ux_app_MsgQueue, "Message Queue app", sizeof(ux_app_devInfotypeDef),
                      pointer, APP_QUEUE_SIZE * sizeof(ux_app_devInfotypeDef));
  if (status != TX_SUCCESS)
  {
    elog_d(TAG_USBX, "FAIL: create msg queue = 0x%02X", status);
    return TX_QUEUE_ERROR;
  }

  /* Create the event flags group. */
  status = tx_event_flags_create(&ux_app_EventFlag, "Event Flag");
  if (status != TX_SUCCESS)
  {
    elog_d(TAG_USBX, "FAIL: create event flags = 0x%02X", status);
    return TX_GROUP_ERROR;
  }

  elog_d(TAG_USBX, "MX_USBX_Host_Init completed successfully");

  /* USER CODE END MX_USBX_Host_Init */

  return ret;
}

/* USER CODE BEGIN 1 */

/**
  * @brief  List all active modem interfaces
  */
static void modem_list_interfaces(void)
{
  ULONG i;

  for (i = 0; i < ux_host_class_modem_count; i++)
  {
    UX_HOST_CLASS_MODEM *m = ux_host_class_modem_instances[i];
    if (m == NULL) continue;
    USBH_UsrLog("  ifnum=%lu bulk_in=%p bulk_out=%p",
                 (unsigned long)ux_host_class_modem_ifnum(m),
                 (void *)m->bulk_in, (void *)m->bulk_out);
  }
}

/**
  * @brief  USBX app thread — handles USB device events
  */
void  usbx_app_thread_entry(ULONG arg)
{
  /* Initialize USBX_Host */
  MX_USB_Host_Init();

  USBH_UsrLog("USB Host started — waiting for modem...");

  while (1)
  {
    if (tx_queue_receive(&ux_app_MsgQueue, &ux_dev_info, TX_WAIT_FOREVER) != TX_SUCCESS)
      continue;

    if (ux_dev_info.Dev_state == Device_connected)
    {
      USBH_UsrLog("=== Modem USB connected, %lu interface(s) active ===",
                   (unsigned long)ux_host_class_modem_count);
      modem_list_interfaces();

      /* Initialize CDC-ECM IP instance and start DHCP.
       * Idempotent — safe to call on every connect. */
      app_netxduo_ecm_init();

#if A7683E_TRANSPORT == A7683E_TRANSPORT_USB
      /* Wire the AT command port to the modem driver */
      {
        bsp_serial_t *at_port = bsp_usb_get(A7683E_USB_AT_IFNUM);
        if (at_port)
        {
          extern void a7683e_set_serial(bsp_serial_t *serial);
          extern void app_netxduo_set_serial(bsp_serial_t *serial);
          a7683e_set_serial(at_port);
          app_netxduo_set_serial(at_port);
          USBH_UsrLog("AT port wired to ifnum=%d — modem thread can start",
                       A7683E_USB_AT_IFNUM);
        }
        else
        {
          USBH_ErrLog("AT port ifnum=%d not found!", A7683E_USB_AT_IFNUM);
        }
      }
#endif
    }
    else if (ux_dev_info.Dev_state == Device_disconnected)
    {
      USBH_UsrLog("Modem disconnected");
      /* Signal modem thread to start PPP cleanup and reconnect cycle */
      app_netxduo_set_reconnect_event();
    }
  }
}

/**
  * @brief MX_USB_Host_Init
  *        Initialization of USB Host.
  * Init USB Host Library, add supported class and start the library
  * @retval None
  */
UINT MX_USB_Host_Init(void)
{
  UINT ret = UX_SUCCESS;
  /* USER CODE BEGIN USB_Host_Init_PreTreatment_0 */
  /* USER CODE END USB_Host_Init_PreTreatment_0 */

  /* The code below is required for installing the host portion of USBX.  */
  if (ux_host_stack_initialize(ux_host_event_callback) != UX_SUCCESS)
  {
    return UX_ERROR;
  }

  /* Register error callback — catches USB disconnection events that
   * UX_DEVICE_REMOVAL may miss (e.g. HCD-level disconnect detection) */
  ux_utility_error_callback_register(ux_host_error_callback);

  /*
   * Register modem class — handles vendor-specific (0xFF) interfaces.
   * ECM/RNDIS interfaces are skipped in QUERY so CDC-ECM can claim them.
   */
  
  if (ux_host_stack_class_register((UCHAR *)"modem", ux_host_class_modem_entry) != UX_SUCCESS)
  {
    elog_d(TAG_USBX, "FAIL: modem class register");
    return UX_ERROR;
  }
  elog_d(TAG_USBX, "modem class registered");

  #if 0
  /* Register CDC-ECM class for network-over-USB */
  if (ux_host_stack_class_register(_ux_system_host_class_cdc_ecm_name,
                                    _ux_host_class_cdc_ecm_entry) == UX_SUCCESS)
  {
    _ux_network_driver_init();
    elog_d(TAG_USBX, "CDC-ECM class registered");
  }
  else
  {
    elog_e(TAG_USBX, "CDC-ECM class register failed — ECM interfaces will not work");
  }
  #endif
  /* Initialize the LL driver */
  MX_USB_OTG_FS_USB_Init();

  /* Register all the USB host controllers available in this system. */
  if (ux_host_stack_hcd_register(_ux_system_host_hcd_stm32_name,
                                 _ux_hcd_stm32_initialize,
                                 USB_OTG_FS_PERIPH_BASE,
                                 (ULONG)&hhcd_USB_OTG_FS) != UX_SUCCESS)
  {
    return UX_ERROR;
  }

  /* Drive vbus to be addedhere */
  USBH_DriverVBUS(USB_VBUS_TRUE);

  /* Enable USB Global Interrupt */
  HAL_HCD_Start(&hhcd_USB_OTG_FS);

  /* USER CODE BEGIN USB_Host_Init_PreTreatment_1 */

  /* USER CODE END USB_Host_Init_PreTreatment_1 */


  /* USER CODE BEGIN USB_Host_Init_PostTreatment */
  /* USER CODE END USB_Host_Init_PostTreatment */
  return ret ;
}

/**
* @brief ux_host_event_callback
* @param ULONG event
           This parameter can be one of the these values:
             1 : UX_DEVICE_INSERTION
             2 : UX_DEVICE_REMOVAL
         UX_HOST_CLASS * Current_class
         VOID * Current_instance
* @retval Status
*/
UINT ux_host_event_callback(ULONG event, UX_HOST_CLASS *Current_class, VOID *Current_instance)
{
  UX_HOST_CLASS_MODEM *modem;

  switch (event)
  {
    case UX_DEVICE_INSERTION :

     if (ux_host_class_cdc_ecm_entry == Current_class->ux_host_class_entry_function)
      {
          USBH_UsrLog("USB CDC_ECM Detected");
          break;
      }

      modem = (UX_HOST_CLASS_MODEM *)Current_instance;
      if (modem == NULL)
        break;

      ux_dev_info.Dev_state = Device_connected;
      USBH_UsrLog("Modem interface ifnum=%u activated (bulk in=%p out=%p)",
                   (unsigned)ux_host_class_modem_ifnum(modem),
                   (void *)modem->bulk_in, (void *)modem->bulk_out);

      /* Send queue message to unblock usbx_app_thread on every activation.
       * On re-enumeration (e.g. after MCU reset or USB reconnect),
       * ux_app_state may still be App_Ready — the queue message is still
       * needed so the app thread can re-wire the serial port to the
       * latest modem instance. */
      ux_app_state = App_Ready;
      ux_dev_info.Device_Type = Modem_Device;
      tx_queue_send(&ux_app_MsgQueue, &ux_dev_info, TX_NO_WAIT);
      break;

    case UX_DEVICE_REMOVAL :

      modem = (UX_HOST_CLASS_MODEM *)Current_instance;
      if (modem == NULL)
      {
        USBH_UsrLog("USB device removed (non-modem)");
        break;
      }

      USBH_UsrLog("Modem interface ifnum=%u removed [%lu remaining]",
                   (unsigned)ux_host_class_modem_ifnum(modem),
                   (unsigned long)(ux_host_class_modem_count > 0
                                   ? ux_host_class_modem_count - 1 : 0));

      /* If all instances gone, mark disconnected */
      if (ux_host_class_modem_count == 0)
      {
        USBH_UsrLog("All modem interfaces removed — signaling reconnect");
        ux_app_state = App_Idle;
        ux_dev_info.Dev_state   = Device_disconnected;
        ux_dev_info.Device_Type = Modem_Device;
        tx_queue_send(&ux_app_MsgQueue, &ux_dev_info, TX_NO_WAIT);

        /* Signal modem thread to start PPP cleanup and reconnect cycle */
        app_netxduo_set_reconnect_event();
      }
      break;

    default:
      break;
  }

  return (UINT) UX_SUCCESS;
}

/**
* @brief ux_host_error_callback
* @param ULONG event
         UINT system_context
         UINT error_code
* @retval Status
*/
VOID ux_host_error_callback(UINT system_level, UINT system_context, UINT error_code)
{
  (void)system_level;
  (void)system_context;

  switch (error_code)
  {
    case UX_DEVICE_ENUMERATION_FAILURE :
      USBH_UsrLog("Enumeration Failure");
      break;

    case UX_NO_DEVICE_CONNECTED :
      USBH_UsrLog("USB Device disconnected");
      /* Signal modem thread to start PPP cleanup and reconnect cycle.
       * This is the primary disconnect detection path — UX_DEVICE_REMOVAL
       * callback may not fire for all class instances. */
      app_netxduo_set_reconnect_event();
      break;

    default:
      USBH_UsrLog("USB error: level=%u ctx=%u code=0x%02X",
                   system_level, system_context, error_code);
      break;
  }
}

/**
* @brief  Drive VBUS.
* @param  state : VBUS state
*          This parameter can be one of the these values:
*           1 : VBUS Active
*           0 : VBUS Inactive
* @retval Status
*/
void USBH_DriverVBUS(uint8_t state)
{
  /* USER CODE BEGIN 0 */

  /* USER CODE END 0*/

  if (state != USB_VBUS_TRUE)
  {
    /* Drive high Charge pump */
    /* Add IOE driver control */
    /* USER CODE BEGIN DRIVE_HIGH_CHARGE_FOR_HS */
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_6, GPIO_PIN_RESET);
    /* USER CODE END DRIVE_HIGH_CHARGE_FOR_HS */
  }
  else
  {
    /* Drive low Charge pump */
    /* Add IOE driver control */
    /* USER CODE BEGIN DRIVE_LOW_CHARGE_FOR_HS */
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_6, GPIO_PIN_SET);
    /* USER CODE END DRIVE_LOW_CHARGE_FOR_HS */
  }

  tx_thread_sleep(200);  /* Use ThreadX sleep instead of HAL_Delay to avoid blocking scheduler */
}
/* USER CODE END 1 */
