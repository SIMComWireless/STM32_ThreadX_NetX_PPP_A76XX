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
#if A7683E_TRANSPORT == A7683E_TRANSPORT_USB
#include "bsp_usb.h"
#endif
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
#define APP_THREAD_PRIO         20
#define APP_THREAD_STACK_SIZE   512

/* Modem init thread — runs AT init sequence then PPP dial */
#define MODEM_THREAD_PRIO       10
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
extern void elog_entry(ULONG param);

/* Elog async output thread — low priority background task.
 * Higher prio number = lower priority in ThreadX. */
#define ELOG_THREAD_PRIO        11
#define ELOG_THREAD_STACK_SIZE  1024
static TX_THREAD elog_thread;
static uint8_t elog_stack[ELOG_THREAD_STACK_SIZE];

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

  /* Initialize EasyLogger (creates semaphores, ring buffer) */
  if (elog_init() == ELOG_NO_ERR) {
      elog_set_fmt(ELOG_LVL_ASSERT, ELOG_FMT_ALL);
      elog_set_fmt(ELOG_LVL_ERROR, ELOG_FMT_LVL | ELOG_FMT_TAG | ELOG_FMT_TIME);
      elog_set_fmt(ELOG_LVL_WARN, ELOG_FMT_LVL | ELOG_FMT_TAG | ELOG_FMT_TIME);
      elog_set_fmt(ELOG_LVL_INFO, ELOG_FMT_LVL | ELOG_FMT_TAG | ELOG_FMT_TIME);
      elog_set_fmt(ELOG_LVL_DEBUG, ELOG_FMT_ALL & ~ELOG_FMT_FUNC);
      elog_set_fmt(ELOG_LVL_VERBOSE, ELOG_FMT_ALL & ~ELOG_FMT_FUNC);
  #ifdef ELOG_COLOR_ENABLE
      elog_set_text_color_enabled(true);
  #endif
      elog_start();
  }
  else {
      /* elog not available — halt */
      extern UART_HandleTypeDef hlpuart1;
      const char msg[] = "elog_init failed!\r\n";
      HAL_UART_Transmit(&hlpuart1, (uint8_t *)msg, sizeof(msg) - 1, 100);
      while (1);
  }

  /* Create elog async output thread (after elog_init so semaphores exist) */
  if (tx_thread_create(&elog_thread, "elog async", elog_entry, 0,
                       elog_stack, ELOG_THREAD_STACK_SIZE,
                       ELOG_THREAD_PRIO, ELOG_THREAD_PRIO,
                       TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
      elog_e(TAG, "Failed to create elog async thread");
      while (1);
  }

  elog_d(TAG, "========================================");
  elog_d(TAG, "  A7683E PPP + NTP Time Sync");
  elog_d(TAG, "========================================");
  elog_d(TAG, "MCU: STM32L4R5ZIT6 @ 120MHz");
  elog_d(TAG, "RTOS: ThreadX");
  elog_d(TAG, "Modem: SIMCom A7683E");
  elog_d(TAG, "Transport: %s",
         A7683E_TRANSPORT == A7683E_TRANSPORT_UART ? "UART3" :
         A7683E_TRANSPORT == A7683E_TRANSPORT_CMUX ? "CMUX" : "USB");
  elog_d(TAG, "APN: %s", A7683E_APN);
  elog_d(TAG, "NTP: %s", NTP_SERVER_HOST);
  elog_d(TAG, "========================================");

#if A7683E_TRANSPORT == A7683E_TRANSPORT_UART
  /* --- UART3 direct mode --- */
  bsp_serial_uart3->init();
  elog_d(TAG, "Serial port '%s' initialized", bsp_serial_uart3->name);
  a7683e_set_serial(bsp_serial_uart3);
  app_netxduo_set_serial(bsp_serial_uart3);

#elif A7683E_TRANSPORT == A7683E_TRANSPORT_CMUX
  /* --- CMUX mode (UART3 + multiplexing) --- */
  bsp_serial_uart3->init();
  elog_d(TAG, "Serial port '%s' initialized (CMUX mode)", bsp_serial_uart3->name);
  a7683e_set_serial(bsp_serial_uart3);
  app_netxduo_set_serial(bsp_serial_uart3);

#elif A7683E_TRANSPORT == A7683E_TRANSPORT_USB
  /* --- USB mode — deferred: serial wired after USB enumeration --- */
  elog_d(TAG, "USB transport selected — waiting for modem enumeration...");
  /* a7683e_set_serial() will be called from USB event callback after
   * the modem is identified and bsp_usb_get() returns a valid port. */
#endif

  /* Create modem initialization thread */
  if (tx_thread_create(&modem_thread, "Modem Init",
                       modem_thread_entry, 0,
                       modem_stack, MODEM_THREAD_STACK_SIZE,
                       MODEM_THREAD_PRIO, MODEM_THREAD_PRIO,
                       TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS)
  {
    elog_e(TAG, "Failed to create modem thread");
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

/** Fatal error handler — blink red LED and halt.
 *  Kept for truly unrecoverable errors (e.g. pool exhaustion). */
static void modem_fatal(void)
{
  elog_e(TAG, "FATAL — system halted");
  while (1)
  {
    HAL_GPIO_TogglePin(LD3_GPIO_Port, LD3_Pin);
    tx_thread_sleep(200);
  }
}

/**
  * @brief  PPP fallback — existing dial flow
  */
static UINT try_ppp_mode(void)
{
  UINT status;

  elog_d(TAG, "Falling back to PPP mode...");

#if CMUX_ENABLE
  status = a7683e_cmux_start();
  if (status == A7683E_OK)
  {
    status = a7683e_pre_dial(a7683e_cmux_send_at_dlci2);
    if (status == A7683E_OK)
    {
      char cmux_resp[32];
      status = a7683e_cmux_send_at_dlci(2, "ATD*99***1#", cmux_resp, sizeof(cmux_resp), 30000);
      if (status == A7683E_OK && strstr(cmux_resp, "CONNECT") != NULL)
      {
        elog_d(TAG, "PPP CONNECT via CMUX");
        cmux_serial_init_all();
        app_netxduo_set_serial(cmux_serial_get(CMUX_PPP_DLCI));
        goto ppp_start;
      }
    }
    cmux_stop(&g_cmux);
    cmux_bridge_stop();
    a7683e_sync();
  }
#endif

  /* Direct UART PPP */
  status = a7683e_pre_dial(a7683e_send_at);
  if (status != A7683E_OK)
  {
    elog_e(TAG, "Pre-dial failed: 0x%02X", status);
    return status;
  }

  status = a7683e_ppp_dial();
  if (status != A7683E_OK)
  {
    elog_e(TAG, "PPP dial failed: 0x%02X", status);
    return status;
  }

ppp_start:
  status = app_netxduo_create_ppp();
  if (status != NX_SUCCESS)
  {
    elog_e(TAG, "PPP create failed: 0x%02X", status);
    return status;
  }

  status = app_netxduo_start_ppp();
  if (status != NX_SUCCESS)
  {
    elog_e(TAG, "PPP start failed: 0x%02X", status);
    return status;
  }

  return NX_SUCCESS;
}

/**
  * @brief  Cleanup all network and serial state on USB disconnect.
  *         Must be called before waiting for reconnect.
  */
static void modem_disconnect_cleanup(void)
{
    elog_w(TAG, "=== USB modem disconnected — cleanup start ===");

    /* Step 1: Destroy PPP + IP + network threads */
    app_netxduo_destroy_ppp();

    /* Step 2: Destroy ECM IP instance (prevents resource leak on reconnect) */
    app_netxduo_ecm_destroy();

    /* Step 3: Cleanup modem AT state (escape PPP, deactivate PDP).
     *         Only attempts AT commands if serial port is still valid;
     *         on USB disconnect the device is already gone, so this
     *         primarily clears internal state. */
    a7683e_cleanup();

    /* Step 4: Clear serial pointers — device is gone.
     *         Prevents use-after-free on next reconnect cycle. */
    a7683e_set_serial(NULL);
    app_netxduo_set_serial(NULL);

    elog_i(TAG, "=== Disconnect cleanup complete — waiting for USB reconnect ===");
}

/**
  * @brief  Modem initialization thread
  * @note   ECM-first strategy with PPP fallback
  *
  * Flow:
  *   1. AT init (sync, SIM, APN, network registration)
  *   2. Try ECM mode: AT+CUSB=1 → wait for USB ECM link-up
  *   3. If ECM fails → fallback to PPP (dial → PPP negotiation)
  *   4. On USB disconnect: cleanup and loop back to step 1
  */
static void modem_thread_entry(ULONG param)
{
  (void)param;
  UINT status;

  elog_d(TAG, "Modem thread started");

  while (1)
  {
#if A7683E_TRANSPORT == A7683E_TRANSPORT_USB
    /* USB mode: wait for enumeration and serial port wiring.
     * After disconnect, modem_serial is cleared to NULL, so
     * a7683e_is_serial_ready() returns false until USB re-enumerates
     * and the event callback re-calls a7683e_set_serial(). */
    elog_i(TAG, "Waiting for USB serial port...");
    while (!a7683e_is_serial_ready())
      tx_thread_sleep(100);
    elog_i(TAG, "USB serial port ready");
#endif

    //a7683e_power_on();
    status = a7683e_init();
    if (status != A7683E_OK)
    {
      elog_e(TAG, "Modem init failed: 0x%02X — retrying in 5s", status);
      /* Clear serial so we re-wait for USB on next iteration.
       * Don't call modem_fatal() — USB devices can have transient
       * enumeration failures that resolve on retry. */
      a7683e_set_serial(NULL);
      app_netxduo_set_serial(NULL);
      tx_thread_sleep(5000);
      continue;
    }

    elog_w(TAG, "ECM unavailable, falling back to PPP");
    status = try_ppp_mode();
    if (status != NX_SUCCESS)
    {
      elog_e(TAG, "PPP fallback failed: 0x%02X — retrying in 5s", status);
      a7683e_set_serial(NULL);
      app_netxduo_set_serial(NULL);
      tx_thread_sleep(5000);
      continue;
    }

    elog_i(TAG, "PPP mode active — waiting for USB disconnect...");

    /* Block until USB disconnect event — unblocks when modem is unplugged */
    app_netxduo_wait_disconnect();

    /* Full cleanup: PPP, network, serial pointers */
    modem_disconnect_cleanup();

    /* Loop back: wait for USB serial to become available again */
  }
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
