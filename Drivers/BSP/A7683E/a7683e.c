/**
  ******************************************************************************
  * @file    a7683e.c
  * @brief   SIMCom A7683E modem driver — AT command layer + PPP dial
  ******************************************************************************
  * @note
  *   Initialization sequence:
  *   1. Power on modem via PWR_EN toggle
  *   2. Wait for modem ready, send AT, verify OK
  *   3. Disable echo (ATE0)
  *   4. Check SIM status (AT+CPIN?)
  *   5. Check signal quality (AT+CSQ)
  *   6. Set APN (AT+CGDCONT)
  *   7. PPP dial (ATD*99***1#) → CONNECT
  ******************************************************************************
  */

#include "a7683e.h"
#include "elog.h"
#include "main.h"
#include <string.h>
#include <stdio.h>

#define TAG "MODEM"

/* ---------- Private variables -------------------------------------------- */

/** Serial port for modem communication (set by a7683e_set_serial) */
static bsp_serial_t *modem_serial = NULL;

static char resp_buf[A7683E_RESP_BUF_SIZE];

/* ---------- Private helpers ---------------------------------------------- */

static void flush_rx(void)
{
    uint8_t tmp[64];
    while (modem_serial && modem_serial->rx_available() > 0)
    {
        modem_serial->read(tmp, sizeof(tmp), 0);
    }
}

static UINT wait_for_response(const char *keyword, char *buf, uint16_t buf_size, uint32_t timeout_ms)
{
    uint16_t pos = 0;
    uint32_t deadline = tx_time_get() + (timeout_ms * TX_TIMER_TICKS_PER_SECOND / 1000);

    memset(buf, 0, buf_size);

    while (tx_time_get() < deadline)
    {
        /* Read available bytes (up to remaining buffer space) */
        uint16_t n = modem_serial->read((uint8_t *)buf + pos, buf_size - 1 - pos, 50);
        pos += n;
        buf[pos] = '\0';

        /* Check for success */
        if (strstr(buf, keyword))     return A7683E_OK;

        /* Check for error */
        if (strstr(buf, "ERROR"))
        {
            LOG_E(TAG, "AT error: %s", buf);
            return A7683E_ERROR;
        }
    }

    LOG_W(TAG, "Timeout waiting for '%s', got: '%s'", keyword, buf);
    return A7683E_TIMEOUT;
}

/* ---------- Public API Implementation ------------------------------------ */

void a7683e_set_serial(bsp_serial_t *serial)
{
    modem_serial = serial;
}

void a7683e_power_on(void)
{
    LOG_I(TAG, "Powering on modem...");

    HAL_GPIO_WritePin(MODEM_DTR_GPIO_Port, MODEM_DTR_Pin, GPIO_PIN_RESET);

    HAL_GPIO_WritePin(MODEM_PWR_EN_GPIO_Port, MODEM_PWR_EN_Pin, GPIO_PIN_SET);
    tx_thread_sleep(500);
    HAL_GPIO_WritePin(MODEM_PWR_EN_GPIO_Port, MODEM_PWR_EN_Pin, GPIO_PIN_RESET);
    tx_thread_sleep(500);
    HAL_GPIO_WritePin(MODEM_PWR_EN_GPIO_Port, MODEM_PWR_EN_Pin, GPIO_PIN_SET);
    tx_thread_sleep(500);

    LOG_I(TAG, "Modem power-on sequence complete");
}

void a7683e_power_off(void)
{
    LOG_I(TAG, "Powering off modem...");

    HAL_GPIO_WritePin(MODEM_PWR_EN_GPIO_Port, MODEM_PWR_EN_Pin, GPIO_PIN_RESET);
    tx_thread_sleep(2000);
    HAL_GPIO_WritePin(MODEM_PWR_EN_GPIO_Port, MODEM_PWR_EN_Pin, GPIO_PIN_SET);

    LOG_I(TAG, "Modem powered off");
}

UINT a7683e_send_at(const char *cmd, char *resp, uint16_t resp_len, uint32_t timeout_ms)
{
    if (!modem_serial) return A7683E_ERROR;

    LOG_D(TAG, "TX: %s", cmd);

    flush_rx();

    modem_serial->write((const uint8_t *)cmd, strlen(cmd));

    UINT status = wait_for_response("OK", resp, resp_len, timeout_ms);

    if (status == A7683E_OK)
    {
        LOG_D(TAG, "RX: %s", resp);
    }

    return status;
}

UINT a7683e_check_alive(void)
{
    for (int i = 0; i < A7683E_MAX_RETRIES; i++)
    {
        if (a7683e_send_at("AT\r\n", resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT) == A7683E_OK)
        {
            return A7683E_OK;
        }
        tx_thread_sleep(500);
    }
    return A7683E_ERROR;
}

UINT a7683e_init(void)
{
    UINT status;
    char cmd[128];

    if (!modem_serial)
    {
        LOG_E(TAG, "No serial port configured — call a7683e_set_serial() first");
        return A7683E_ERROR;
    }

    LOG_I(TAG, "=== A7683E Modem Initialization ===");
    LOG_I(TAG, "Serial port: %s", modem_serial->name);

    /* Step 0: Escape from PPP data mode if previously connected.
     * Send +++ (no CR/LF) and wait up to 1s for "OK".
     * If modem responds, it was in PPP mode — hang up with ATH. */
    LOG_I(TAG, "[0/7] Checking for PPP escape...");
    modem_serial->write((const uint8_t *)"+++", 3);
    {
        char esc_buf[64] = {0};
        uint16_t got = modem_serial->read((uint8_t *)esc_buf, sizeof(esc_buf) - 1, 1000);
        if (got > 0 && strstr(esc_buf, "OK"))
        {
            LOG_I(TAG, "Modem was in PPP mode, hanging up...");
            tx_thread_sleep(1000);  /* Guard time after +++ */
            a7683e_send_at("ATH\r", resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT);
            tx_thread_sleep(500);
        }
    }

    /* Step 1: Check modem is alive */
    LOG_I(TAG, "[1/7] Checking modem response...");
    status = a7683e_check_alive();
    if (status != A7683E_OK)
    {
        LOG_E(TAG, "Modem not responding to AT");
        return A7683E_ERROR;
    }
    LOG_I(TAG, "Modem is alive");

    /* Step 2: Disable echo */
    LOG_I(TAG, "[2/7] Disabling echo...");
    a7683e_send_at("ATE0\r", resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT);

    /* Step 3: Check SIM status */
    LOG_I(TAG, "[3/7] Checking SIM card...");
    status = a7683e_send_at("AT+CPIN?\r", resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT);
    if (status != A7683E_OK || strstr(resp_buf, "READY") == NULL)
    {
        LOG_E(TAG, "SIM card not ready: %s", resp_buf);
        return A7683E_NO_SIM;
    }
    LOG_I(TAG, "SIM card ready");

    /* Step 4: Check signal quality */
    LOG_I(TAG, "[4/7] Checking signal quality...");
    status = a7683e_send_at("AT+CSQ\r", resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT);
    if (status == A7683E_OK)
    {
        int rssi = 0;
        char *p = strstr(resp_buf, "+CSQ:");
        if (p)
        {
            sscanf(p, "+CSQ: %d", &rssi);
            LOG_I(TAG, "Signal RSSI: %d (%s)", rssi,
                  (rssi == 99) ? "Unknown" :
                  (rssi < 10) ? "Marginal" :
                  (rssi < 15) ? "OK" :
                  (rssi < 20) ? "Good" : "Excellent");
        }
    }

    /* Step 5: Set APN */
    LOG_I(TAG, "[5/7] Setting APN: %s", A7683E_APN);
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"\r", A7683E_APN);
    status = a7683e_send_at(cmd, resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT);
    if (status != A7683E_OK)
    {
        LOG_E(TAG, "Failed to set APN");
        return A7683E_ERROR;
    }
    LOG_I(TAG, "APN set successfully");

    /* Step 6: Wait for network registration */
    LOG_I(TAG, "[6/7] Waiting for network registration...");
    {
        int registered = 0;
        for (int retry = 0; retry < 60; retry++)  /* Max 60s */
        {
            status = a7683e_send_at("AT+CGREG?\r", resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT);
            if (status == A7683E_OK)
            {
                /* Response: +CGREG: <n>,<stat>
                 * stat=1: registered, home network
                 * stat=5: registered, roaming */
                char *p = strstr(resp_buf, "+CGREG:");
                if (p)
                {
                    int n, stat;
                    if (sscanf(p, "+CGREG: %d,%d", &n, &stat) == 2)
                    {
                        if (stat == 1 || stat == 5)
                        {
                            LOG_I(TAG, "Network registered (%s)",
                                  (stat == 1) ? "home" : "roaming");
                            registered = 1;
                            break;
                        }
                        LOG_D(TAG, "Registration status: %d, waiting...", stat);
                    }
                }
            }
            tx_thread_sleep(1000);
        }

        if (!registered)
        {
            LOG_E(TAG, "Network registration timeout");
            return A7683E_ERROR;
        }
    }

    LOG_I(TAG, "=== Modem initialization complete ===");
    return A7683E_OK;
}

UINT a7683e_ppp_dial(void)
{
    UINT status;

    if (!modem_serial) return A7683E_ERROR;

    LOG_I(TAG, "Starting PPP dial...");

    /* ---- Check EPS network registration (LTE) ---- */
    status = a7683e_send_at("AT+CEREG?\r", resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT);
    if (status == A7683E_OK)
    {
        int n = 0, stat = 0;
        char *p = strstr(resp_buf, "+CEREG:");
        if (p && sscanf(p, "+CEREG: %d,%d", &n, &stat) == 2)
        {
            LOG_I(TAG, "EPS registration: %s",
                  (stat == 1) ? "home" : (stat == 5) ? "roaming" : (stat == 8) ? "SMS only" : "not registered");
        }
    }

    /* ---- Check PDP context activation status ---- */
    status = a7683e_send_at("AT+CGACT?\r", resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT);
    if (status == A7683E_OK)
    {
        LOG_I(TAG, "PDP context status: %s", resp_buf);
        /* Check if context 1 is already active (+CGACT: 1,1) */
        if (strstr(resp_buf, "+CGACT: 1,1") == NULL)
        {
            /* PDP context not active — activate it explicitly */
            LOG_I(TAG, "Activating PDP context 1...");
            status = a7683e_send_at("AT+CGACT=1,1\r", resp_buf, sizeof(resp_buf), 15000);
            if (status != A7683E_OK)
            {
                LOG_E(TAG, "PDP context activation failed!");
                return A7683E_PPP_FAIL;
            }
            LOG_I(TAG, "PDP context 1 activated");
        }
        else
        {
            LOG_I(TAG, "PDP context 1 already active");
        }
    }

    /* ---- Verify PDP address (IP assigned by network) ---- */
    status = a7683e_send_at("AT+CGPADDR=1\r", resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT);
    if (status == A7683E_OK)
    {
        LOG_I(TAG, "PDP address: %s", resp_buf);
    }

    /* ---- PPP dial ---- */
    flush_rx();
    modem_serial->write((const uint8_t *)"ATD*99***1#\r", 13);

    status = wait_for_response("CONNECT", resp_buf, sizeof(resp_buf), 30000);

    if (status == A7683E_OK)
    {
        LOG_I(TAG, "PPP CONNECT received — modem is now in data mode");
        tx_thread_sleep(100);
    }
    else
    {
        LOG_E(TAG, "PPP dial failed — no CONNECT received");
        return A7683E_PPP_FAIL;
    }

    flush_rx();

    return A7683E_OK;
}
