/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_threadx.c
  * @author  MCD Application Team
  * @brief   ThreadX applicative file
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

/* Includes ------------------------------------------------------------------*/
#include "app_threadx.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "main.h"
#include "elog.h"
#include "bsp_serial.h"
#include "bsp_uart3.h"
#include "a7683e.h"
#include "app_netxduo.h"
#include "cmux.h"
#include "cmux_serial.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#define TAG             "APP"

/** NTP server hostname — must match definition in app_netxduo.c */
#ifndef NTP_SERVER_HOST
#define NTP_SERVER_HOST         "ntp.aliyun.com"
#endif

/* Main app thread — heartbeat LED + modem management */
#define APP_THREAD_PRIO         10
#define APP_THREAD_STACK_SIZE   512

/* Modem init thread — runs AT init sequence then PPP dial */
#define MODEM_THREAD_PRIO       12
#define MODEM_THREAD_STACK_SIZE 1024*2

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
TX_THREAD tx_app_thread;
/* USER CODE BEGIN PV */

static TX_THREAD modem_thread;
static uint8_t modem_stack[MODEM_THREAD_STACK_SIZE];

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

static void modem_thread_entry(ULONG param);

/* USER CODE END PFP */

/**
  * @brief  Application ThreadX Initialization.
  * @param memory_ptr: memory pointer
  * @retval int
  */
UINT App_ThreadX_Init(VOID *memory_ptr)
{
  UINT ret = TX_SUCCESS;
  TX_BYTE_POOL *byte_pool = (TX_BYTE_POOL*)memory_ptr;
  /* USER CODE BEGIN App_ThreadX_MEM_POOL */

  /* USER CODE END App_ThreadX_MEM_POOL */
  CHAR *pointer;

  /* Allocate the stack for tx app thread  */
  if (tx_byte_allocate(byte_pool, (VOID**) &pointer,
                       TX_APP_STACK_SIZE, TX_NO_WAIT) != TX_SUCCESS)
  {
    return TX_POOL_ERROR;
  }
  /* Create tx app thread.  */
  if (tx_thread_create(&tx_app_thread, "tx app thread", tx_app_thread_entry, 0, pointer,
                       TX_APP_STACK_SIZE, TX_APP_THREAD_PRIO, TX_APP_THREAD_PREEMPTION_THRESHOLD,
                       TX_APP_THREAD_TIME_SLICE, TX_APP_THREAD_AUTO_START) != TX_SUCCESS)
  {
    return TX_THREAD_ERROR;
  }

  /* USER CODE BEGIN App_ThreadX_Init */

  /* USER CODE END App_ThreadX_Init */

  return ret;
}
/**
  * @brief  Function implementing the tx_app_thread_entry thread.
  * @param  thread_input: Hardcoded to 0.
  * @retval None
  */
void tx_app_thread_entry(ULONG thread_input)
{
  /* USER CODE BEGIN tx_app_thread_entry */

  /* Initialize EasyLogger */
  elog_init();
  elog_start();

  LOG_I(TAG, "========================================");
  LOG_I(TAG, "  A7683E PPP + NTP Time Sync");
  LOG_I(TAG, "========================================");
  LOG_I(TAG, "MCU: STM32L4R5ZIT6 @ 120MHz");
  LOG_I(TAG, "RTOS: ThreadX");
  LOG_I(TAG, "Modem: SIMCom A7683E on USART3");
  LOG_I(TAG, "APN: %s", A7683E_APN);
  LOG_I(TAG, "NTP: %s", NTP_SERVER_HOST);
  LOG_I(TAG, "========================================");

  /* Initialize UART3 serial port (DMA double-buffer) */
  bsp_serial_uart3->init();
  LOG_I(TAG, "Serial port '%s' initialized (DMA double-buffer, idle-line ISR)",
        bsp_serial_uart3->name);

  /* Wire serial port to modem driver and NetX PPP */
  a7683e_set_serial(bsp_serial_uart3);
  app_netxduo_set_serial(bsp_serial_uart3);

  /* Create modem initialization thread */
  if (tx_thread_create(&modem_thread, "Modem Init",
                       modem_thread_entry, 0,
                       modem_stack, MODEM_THREAD_STACK_SIZE,
                       MODEM_THREAD_PRIO, MODEM_THREAD_PRIO,
                       TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    LOG_E(TAG, "Failed to create modem thread");
  }

  /* Main heartbeat loop */
  while (1)
  {
    /* Toggle LD2 (blue LED) every 500ms — heartbeat indicator */
    HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
    tx_thread_sleep(500);
  }
  /* USER CODE END tx_app_thread_entry */
}

/* USER CODE BEGIN 1 */

/** Fatal error handler — blink red LED and halt */
static void modem_fatal(void)
{
  while (1)
  {
    HAL_GPIO_TogglePin(LD3_GPIO_Port, LD3_Pin);
    tx_thread_sleep(200);
  }
}

/**
  * @brief  Modem initialization and PPP dial thread
  * @note   Runs at priority 12 (higher than app thread)
  *
  * Flow:
  *   1. Power on modem, init (direct UART)
  *   2. [CMUX] Start CMUX → pre_dial → dial → switch PPP to CMUX virtual serial
  *   3. [Direct] pre_dial → dial → PPP uses UART3 directly
  *   4. Create PPP instance, start PPP negotiation
  */
static void modem_thread_entry(ULONG param)
{
  (void)param;
  UINT status;

  LOG_I(TAG, "Modem thread started");

  /* Step 1: Power on modem */
  //a7683e_power_on();

  /* Step 2: Init — always via direct UART (CMUX not active yet) */
  status = a7683e_init();
  if (status != A7683E_OK)
  {
    LOG_E(TAG, "Modem init failed: 0x%02X", status);
    modem_fatal();
  }

#if CMUX_ENABLE
  /* Step 3: Try CMUX mode */
  status = a7683e_cmux_start();
  if (status == A7683E_OK)
  {
    /* Pre-dial queries via CMUX DLCI 2 */
    status = a7683e_pre_dial(a7683e_cmux_send_at_dlci2);
    if (status != A7683E_OK)
    {
      LOG_E(TAG, "Pre-dial failed via CMUX: 0x%02X", status);
      goto cmux_fail;
    }

    /* PPP dial via CMUX DLCI 2 — ATD must be on same DLCI as PPP data */
    char cmux_resp[32];
    status = a7683e_cmux_send_at_dlci(2, "ATD*99***1#", cmux_resp, sizeof(cmux_resp), 30000);
    if (status == A7683E_OK && strstr(cmux_resp, "CONNECT") != NULL)
    {
      LOG_I(TAG, "PPP CONNECT via CMUX — DLCI 2 is now PPP data channel");
      /* Initialize CMUX virtual serial ports and switch PPP to DLCI 2 */
      cmux_serial_init_all();
      app_netxduo_set_serial(cmux_serial_get(CMUX_PPP_DLCI));
      goto ppp_create;
    }

    LOG_E(TAG, "PPP dial via CMUX failed");
cmux_fail:
    cmux_stop(&g_cmux);
    cmux_bridge_stop();
    a7683e_sync();
    LOG_I(TAG, "Falling back to direct UART mode");
  }
  else
  {
    LOG_E(TAG, "CMUX start failed: 0x%02X — using direct mode", status);
  }
#endif /* CMUX_ENABLE */

  /* Direct UART mode: pre-dial + PPP dial */
  status = a7683e_pre_dial(a7683e_send_at);
  if (status != A7683E_OK)
  {
    LOG_E(TAG, "Pre-dial failed: 0x%02X", status);
    modem_fatal();
  }

  status = a7683e_ppp_dial();
  if (status != A7683E_OK)
  {
    LOG_E(TAG, "PPP dial failed: 0x%02X", status);
    modem_fatal();
  }
  LOG_I(TAG, "Modem in PPP data mode (direct UART)");

ppp_create:
  /* Step 4: Create PPP instance (must be in thread context) */

  status = app_netxduo_create_ppp();
  if (status != NX_SUCCESS)
  {
    LOG_E(TAG, "PPP create failed: 0x%02X", status);
    modem_fatal();
  }

  /* Step 5: Start PPP negotiation */
  status = app_netxduo_start_ppp();
  if (status != NX_SUCCESS)
  {
    LOG_E(TAG, "PPP start failed: 0x%02X", status);
    modem_fatal();
  }

  /* Thread exits — PPP read thread and NTP thread handle the rest */
}

/**
  * @brief  Function that implements the kernel's initialization.
  * @param  None
  * @retval None
  */
void MX_ThreadX_Init(void)
{
  /* USER CODE BEGIN  Before_Kernel_Start */

  /* USER CODE END  Before_Kernel_Start */

  tx_kernel_enter();

  /* USER CODE BEGIN  Kernel_Start_Error */

  /* USER CODE END  Kernel_Start_Error */
}

/* USER CODE END 1 */
