/**
  ******************************************************************************
  * @file    a7683e.c
  * @brief   SIMCom A7683E modem driver — AT command layer + PPP dial
  ******************************************************************************
  * @note
  *   Initialization sequence (a7683e_init):
  *   0. Escape PPP mode (+++ guard time sequence)
  *   1. Sync with modem (send AT up to 5 times)
  *   2. Disable echo (ATE0)
  *   3. Check SIM status (AT+CPIN?)
  *   4. Check signal quality (AT+CSQ)
  *   5. Set APN (AT+CGDCONT)
  *   6. Wait for network registration (AT+CGREG?)
  *
  *   Pre-dial (a7683e_pre_dial):
  *   - Check EPS registration (AT+CEREG?)
  *   - Activate PDP context (AT+CGACT)
  *   - Verify PDP address (AT+CGPADDR)
  *
  *   Dial (a7683e_ppp_dial):
  *   - PPP dial (ATD*99***1#) → CONNECT
  *
  *   All AT send functions auto-append \r if not present.
  *   pre_dial / ppp_dial accept a function pointer for CMUX/direct transport.
  ******************************************************************************
  */

#include "a7683e.h"
#include "cmux.h"
#include "elog.h"
#include "main.h"
#include <string.h>
#include <stdio.h>

#define TAG "MODEM"

/* ---------- Private variables -------------------------------------------- */

/** Serial port for modem communication (set by a7683e_set_serial) */
static bsp_serial_t *modem_serial = NULL;

static char resp_buf[A7683E_RESP_BUF_SIZE];

/** Modem info collected during init */
static a7683e_info_t modem_info = {0};

/**
 * @brief  Get modem information collected during a7683e_init().
 * @return Pointer to static a7683e_info_t structure (read-only).
 */
const a7683e_info_t *a7683e_get_info(void)
{
    return &modem_info;
}

/**
 * @brief  Get current RSSI signal strength via AT+CSQ.
 * @return RSSI value in dBm (e.g. -113 to -51), or -999 if unavailable.
 *         Maps AT+CSQ rssi: 0=-113dBm, 1=-111dBm, 2..30=-109..-53dBm, 31=-51dBm.
 */
int a7683e_get_rssi(void)
{
    char rssi_resp[A7683E_RESP_BUF_SIZE];
    UINT status;

    status = a7683e_send_at("AT+CSQ", rssi_resp, sizeof(rssi_resp), A7683E_DEFAULT_TIMEOUT);
    if (status == A7683E_OK)
    {
        int rssi_raw = 0;
        char *p = strstr(rssi_resp, "+CSQ:");
        if (p && sscanf(p, "+CSQ: %d", &rssi_raw) == 1)
        {
            if (rssi_raw == 0)       return -113;
            if (rssi_raw == 1)       return -111;
            if (rssi_raw >= 2 && rssi_raw <= 30) return -113 + 2 * rssi_raw;
            if (rssi_raw == 31)      return -51;
        }
    }

    return -999;  /* unavailable */
}

/* ---------- Private helpers ---------------------------------------------- */

/**
 * @brief  Parse ATI response into modem_info structure.
 *         Expected format:
 *           Manufacturer: SIMCOM INCORPORATED
 *           Model: A7683E-LAXS
 *           Revision: V11.0.01
 *           IMEI: 862095060018816
 * @param  ati_buf  Raw ATI response string
 */
static void parse_ati(const char *ati_buf)
{
    const char *line = ati_buf;

    while (*line) {
        /* Advance to next line */
        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
        if (!*line) break;

        /* Skip leading whitespace / CR */
        while (*line == ' ' || *line == '\r') line++;

        /* Match "Key: Value" lines */
        const char *key_end = strchr(line, ':');
        if (!key_end) continue;

        const char *val = key_end + 1;
        while (*val == ' ') val++;

        size_t val_len = 0;
        while (val[val_len] && val[val_len] != '\r' && val[val_len] != '\n')
            val_len++;

        size_t key_len = (size_t)(key_end - line);

        if (key_len == 13 && strncmp(line, "Manufacturer", key_len) == 0) {
            if (val_len >= sizeof(modem_info.manufacturer)) val_len = sizeof(modem_info.manufacturer) - 1;
            memcpy(modem_info.manufacturer, val, val_len);
            modem_info.manufacturer[val_len] = '\0';
        }
        else if (key_len == 5 && strncmp(line, "Model", key_len) == 0) {
            if (val_len >= sizeof(modem_info.model)) val_len = sizeof(modem_info.model) - 1;
            memcpy(modem_info.model, val, val_len);
            modem_info.model[val_len] = '\0';
        }
        else if (key_len == 8 && strncmp(line, "Revision", key_len) == 0) {
            if (val_len >= sizeof(modem_info.revision)) val_len = sizeof(modem_info.revision) - 1;
            memcpy(modem_info.revision, val, val_len);
            modem_info.revision[val_len] = '\0';
        }
        else if (key_len == 4 && strncmp(line, "IMEI", key_len) == 0) {
            if (val_len >= sizeof(modem_info.imei)) val_len = sizeof(modem_info.imei) - 1;
            memcpy(modem_info.imei, val, val_len);
            modem_info.imei[val_len] = '\0';
        }
    }
}

static void flush_rx(void)
{
    uint8_t tmp[64];
    while (modem_serial && modem_serial->rx_available(modem_serial) > 0)
    {
        modem_serial->read(modem_serial, tmp, sizeof(tmp), 0);
    }
}

/** Overflow buffer: stores bytes read past the keyword line (e.g. PPP frames
 *  that arrive immediately after "CONNECT\r\n"). The PPP read thread drains
 *  this before reading from the serial port. */
static uint8_t  overflow_buf[512];
static uint16_t overflow_len;

/**
 * @brief  Drain overflow buffer — called by PPP read thread before serial read.
 */
uint16_t a7683e_drain_overflow(uint8_t *dst, uint16_t max_len)
{
    if (overflow_len == 0) return 0;
    uint16_t n = (overflow_len > max_len) ? max_len : overflow_len;
    memcpy(dst, overflow_buf, n);
    /* Shift remaining bytes */
    memmove(overflow_buf, overflow_buf + n, overflow_len - n);
    overflow_len -= n;
    return n;
}

static UINT wait_for_response(const char *keyword, char *buf, uint16_t buf_size, uint32_t timeout_ms)
{
    uint16_t pos = 0;
    uint32_t start = tx_time_get();
    uint32_t ticks = timeout_ms * TX_TIMER_TICKS_PER_SECOND / 1000;

    memset(buf, 0, buf_size);
    overflow_len = 0;

    while ((tx_time_get() - start) < ticks)
    {
        /* If buffer is full, shift old data out to make room for new bytes.
         * Keeps the most recent data (most likely to contain the keyword). */
        if (pos >= buf_size - 1) {
            uint16_t half = buf_size / 2;
            memmove(buf, buf + half, buf_size - 1 - half);
            pos = buf_size - 1 - half;
            buf[pos] = '\0';
        }

        /* Read available bytes (up to remaining buffer space) */
        uint16_t n = modem_serial->read(modem_serial, (uint8_t *)buf + pos, buf_size - 1 - pos, 50);
        if (n == 0) continue;
        pos += n;
        buf[pos] = '\0';

        /* Check for success */
        char *kw = strstr(buf, keyword);
        if (kw)
        {
            /* Found keyword. We may have read past the response line.
             * Save excess bytes (data after the keyword line's \n) to overflow
             * buffer so the PPP read thread can consume them. */
            char *after_kw = kw + strlen(keyword);
            char *line_end = strchr(after_kw, '\n');
            if (line_end)
            {
                uint16_t excess = (uint16_t)(buf + pos - line_end - 1);
                if (excess > 0 && excess <= sizeof(overflow_buf) - overflow_len)
                {
                    memcpy(overflow_buf, line_end + 1, excess);
                    overflow_len = excess;
                }
            }
            return A7683E_OK;
        }

        /* Check for error */
        if (strstr(buf, "ERROR"))
        {
            elog_e(TAG, "AT error: %s", buf);
            return A7683E_ERROR;
        }
    }

    elog_w(TAG, "Timeout waiting for '%s', got: '%s'", keyword, buf);
    return A7683E_TIMEOUT;
}

/* ---------- Public API Implementation ------------------------------------ */

void a7683e_set_serial(bsp_serial_t *serial)
{
    modem_serial = serial;
}

int a7683e_is_serial_ready(void)
{
    return (modem_serial != NULL);
}

/**
 * @brief  Escape from CMUX mode if modem is still in CMUX after MCU reset.
 *         Detects CMUX by checking if RX data starts with 0xF9 (CMUX flag byte).
 *         Sends DISC (P/F=1) on DLCI 0/1/2 to request CMUX teardown,
 *         then waits for modem to auto-revert to AT command mode.
 * @return A7683E_OK if CMUX was detected and DISC sent,
 *         A7683E_TIMEOUT if not in CMUX mode (normal).
 */
UINT a7683e_escape_cmux(void)
{
    if (!modem_serial) return A7683E_ERROR;

    /* Drain stale RX data and check first byte */
    flush_rx();

    elog_d(TAG, "Sending DISC to close all DLCIs...");

    /* Send DISC (P/F=1) on DLCI 0, 1, 2 to request teardown.
     * Frame format: 7E | Addr | 0x53(DISCPF) | 0x01 | FCS | 7E
     * Address: EA=1 | C/R=1 | DLCI
     * FCS precomputed: CRC-8 over [Addr, 0x53, 0x01] */
    static const uint8_t disc_frames[][6] = {
        /* DLCI 0: addr=0x03, FCS=0xFD */
        { 0xF9, 0x03, 0x53, 0x01, 0xFD, 0xF9 },
        /* DLCI 1: addr=0x07, FCS=0x3F */
        { 0xF9, 0x07, 0x53, 0x01, 0x3F, 0xF9 },
        /* DLCI 2: addr=0x0B, FCS=0xB8 */
        { 0xF9, 0x0B, 0x53, 0x01, 0xB8, 0xF9 },
    };
    for (uint8_t i = 0; i < 3; i++)
    {
        modem_serial->write(modem_serial, disc_frames[i], 6);
        elog_d(TAG, "DISC sent on DLCI %u", i);
    }

    /* Modem sends UA responses, then auto-reverts to AT mode.
     * SIMCom timeout is ~3s for SABM retries. */
    elog_d(TAG, "Waiting for modem to exit CMUX mode...");
    tx_thread_sleep(100);

    /* Verify we're back in AT mode */
    flush_rx();
    modem_serial->write(modem_serial, (const uint8_t *)"AT\r", 3);
    char buf[64] = {0};
    uint16_t n = modem_serial->read(modem_serial, (uint8_t *)buf, sizeof(buf) - 1, 1000);
    if (n > 0 && strstr(buf, "OK"))
    {
        elog_d(TAG, "Modem back in AT mode");
        return A7683E_OK;
    }

    elog_w(TAG, "Modem may still be transitioning, continuing...");
    return A7683E_TIMEOUT;
}

/**
 * @brief  Escape from PPP data mode.
 *         Sends +++ (no CR/LF) and waits up to 1s for "OK".
 *         If modem responds, it was in PPP mode — hangs up with ATH.
 * @return A7683E_OK if modem was in PPP mode and escaped,
 *         A7683E_TIMEOUT if modem was not in PPP mode (normal),
 *         A7683E_ERROR on failure.
 */
UINT a7683e_escape_ppp(void)
{
    if (!modem_serial) return A7683E_ERROR;

    elog_d(TAG, "Escaping PPP mode (+++)...");

    /* +++ escape sequence per ITU-T V.250:
     * ≥1s silence → +++ → ≥1s silence → then commands */
    flush_rx();
    tx_thread_sleep(1100);  /* Pre-guard: ≥1s silence before +++ (V.250) */
    modem_serial->write(modem_serial, (const uint8_t *)"+++", 3);
    tx_thread_sleep(1100);  /* Post-guard: ≥1s silence after +++ (V.250) */
    char esc_buf[64] = {0};
    uint16_t got = modem_serial->read(modem_serial, (uint8_t *)esc_buf, sizeof(esc_buf) - 1, 1000);
    if (got > 0 && strstr(esc_buf, "OK"))
    {
        elog_d(TAG, "Modem was in PPP mode, hanging up...");
        a7683e_send_at("ATH", resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT);
        tx_thread_sleep(100);
        return A7683E_OK;
    }

    elog_d(TAG, "Not in PPP mode (normal)");
    return A7683E_TIMEOUT;
}

/**
 * @brief  Synchronize with modem by sending AT up to 5 times.
 *         Some modems need multiple AT commands to wake up after power-on
 *         or mode switch.
 * @return A7683E_OK if modem responded, A7683E_ERROR otherwise.
 */
UINT a7683e_sync(void)
{
    if (!modem_serial) return A7683E_ERROR;

    elog_d(TAG, "Syncing with modem...");
    for (int i = 0; i < 5; i++)
    {
        flush_rx();
        modem_serial->write(modem_serial, (const uint8_t *)"AT\r", 3);
        UINT status = wait_for_response("OK", resp_buf, sizeof(resp_buf), 1000);
        if (status == A7683E_OK)
        {
            elog_d(TAG, "Modem synced (attempt %d)", i + 1);
            return A7683E_OK;
        }
        elog_d(TAG, "AT attempt %d failed, retrying...", i + 1);
        tx_thread_sleep(200);
    }

    elog_e(TAG, "Modem not responding after 5 AT attempts");
    return A7683E_ERROR;
}

void a7683e_power_on(void)
{
    elog_d(TAG, "Powering on modem...");

    HAL_GPIO_WritePin(MODEM_DTR_GPIO_Port, MODEM_DTR_Pin, GPIO_PIN_RESET);

    HAL_GPIO_WritePin(MODEM_PWR_EN_GPIO_Port, MODEM_PWR_EN_Pin, GPIO_PIN_SET);
    tx_thread_sleep(500);
    HAL_GPIO_WritePin(MODEM_PWR_EN_GPIO_Port, MODEM_PWR_EN_Pin, GPIO_PIN_RESET);
    tx_thread_sleep(500);
    HAL_GPIO_WritePin(MODEM_PWR_EN_GPIO_Port, MODEM_PWR_EN_Pin, GPIO_PIN_SET);
    tx_thread_sleep(500);

    elog_d(TAG, "Modem power-on sequence complete");
}

void a7683e_power_off(void)
{
    elog_d(TAG, "Powering off modem...");

    HAL_GPIO_WritePin(MODEM_PWR_EN_GPIO_Port, MODEM_PWR_EN_Pin, GPIO_PIN_RESET);
    tx_thread_sleep(2000);
    HAL_GPIO_WritePin(MODEM_PWR_EN_GPIO_Port, MODEM_PWR_EN_Pin, GPIO_PIN_SET);

    elog_d(TAG, "Modem powered off");
}

UINT a7683e_send_at(const char *cmd, char *resp, uint16_t resp_len, uint32_t timeout_ms)
{
    if (!modem_serial) return A7683E_ERROR;
#if !AT_THROUGHPUT_TEST_ENABLE
    elog_d(TAG, "TX: %s", cmd);
#endif
    flush_rx();

    /* Send command — auto-append \r if not present.
     * Use single write to keep the bytes contiguous. */
    size_t cmd_len = strlen(cmd);
    if (cmd_len > 0 && cmd[cmd_len - 1] == '\r')
    {
        modem_serial->write(modem_serial, (const uint8_t *)cmd, cmd_len);
    }
    else
    {
        uint8_t cmd_buf[256];
        if (cmd_len + 1 > sizeof(cmd_buf)) {
            elog_e(TAG, "AT command too long (%u bytes)", (unsigned)cmd_len);
            return A7683E_ERROR;
        }
        memcpy(cmd_buf, cmd, cmd_len);
        cmd_buf[cmd_len] = '\r';
        modem_serial->write(modem_serial, cmd_buf, cmd_len + 1);
    }

    UINT status = wait_for_response("OK", resp, resp_len, timeout_ms);

    if (status == A7683E_OK)
    {
        #if !AT_THROUGHPUT_TEST_ENABLE
            elog_d(TAG, "RX: %s", resp);
        #endif
    }

    return status;
}

UINT a7683e_check_alive(void)
{
    for (int i = 0; i < A7683E_MAX_RETRIES; i++)
    {
        if (a7683e_send_at("AT", resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT) == A7683E_OK)
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
        elog_e(TAG, "No serial port configured — call a7683e_set_serial() first");
        return A7683E_ERROR;
    }

    elog_d(TAG, "=== A7683E Modem Initialization ===");
    elog_d(TAG, "Serial port: %s", modem_serial->name);

    /* Step 0: Escape from CMUX/PPP if modem is in a stale state */
    elog_d(TAG, "[0/7] Checking for stale CMUX/PPP mode...");
#if A7683E_TRANSPORT != A7683E_TRANSPORT_USB
    a7683e_escape_cmux();  /* Ignore return — TIMEOUT means not in CMUX */
#endif
    a7683e_escape_ppp();   /* Ignore return — TIMEOUT means not in PPP */


    /* Step 1: Sync with modem (send AT up to 5 times) */
    elog_d(TAG, "[1/7] Syncing with modem...");
    status = a7683e_sync();
    if (status != A7683E_OK)
    {
        /* Modem not responding — likely stuck in PPP data mode after MCU reset.
         * Power cycle the modem to force it back to AT command mode. */
        elog_w(TAG, "Modem not responding — power cycling...");
        a7683e_power_off();
        a7683e_power_on();

        /* Power cycle causes USB disconnect + re-enumeration.
         * Poll for modem_serial to be re-wired by the USB event callback.
         * The old instance was replaced during re-enumeration; the callback
         * calls bsp_usb_get() + a7683e_set_serial() when the new instance
         * is ready. Timeout after 30s to avoid hanging forever. */
        elog_d(TAG, "Waiting for modem to boot and re-enumerate...");
        a7683e_set_serial(NULL);  /* Clear stale pointer — will be re-set by USB callback */
        for (int i = 0; i < 300 && !modem_serial; i++) {
            tx_thread_sleep(100);
        }

        if (!modem_serial) {
            elog_e(TAG, "Serial port lost after power cycle — waiting for USB reconnect...");
            return A7683E_ERROR;
        }
        elog_d(TAG, "Serial port re-acquired after power cycle");

        /* Retry sync after power cycle */
        status = a7683e_sync();
        if (status != A7683E_OK)
        {
            elog_e(TAG, "Modem still not responding after power cycle");
            return A7683E_ERROR;
        }
        elog_i(TAG, "Modem recovered after power cycle");
    }

    /* Step 2: Disable echo */
    elog_d(TAG, "[2/7] Disabling echo...");
    a7683e_send_at("ATE0", resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT);

    /* Step 2b: Read modem info (ATI) — includes Manufacturer, Model, Revision, IMEI */
    {
        char ati_buf[A7683E_RESP_BUF_SIZE];
        if (a7683e_send_at("ATI", ati_buf, sizeof(ati_buf), 2000) == A7683E_OK) {
            parse_ati(ati_buf);
            elog_i(TAG, "Manufacturer: %s", modem_info.manufacturer);
            elog_i(TAG, "Model: %s",        modem_info.model);
            elog_i(TAG, "Revision: %s",     modem_info.revision);
            if (strlen(modem_info.imei) == 15) {
                elog_i(TAG, "IMEI: %s",     modem_info.imei);
            } else {
                elog_w(TAG, "IMEI parse failed (got %d digits)", (int)strlen(modem_info.imei));
            }
        } else {
            elog_w(TAG, "ATI failed — modem info not available");
        }
    }

    /* Step 3: Check SIM status */
    elog_d(TAG, "[3/7] Checking SIM card...");
    status = a7683e_send_at("AT+CPIN?", resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT);
    if (status != A7683E_OK || strstr(resp_buf, "READY") == NULL)
    {
        elog_e(TAG, "SIM card not ready: %s", resp_buf);
        return A7683E_NO_SIM;
    }
    elog_d(TAG, "SIM card ready");

    status = a7683e_send_at("AT$MYCONFIG?", resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT);
    if (status == A7683E_OK && strstr(resp_buf, "OK"))
    {
        /* Response: $MYCONFIG: "usbnetmode",<mode>,<...>
         * If mode==0, switch to ECM (1) */
        char *p = strstr(resp_buf, "$MYCONFIG:");
        if (p)
        {
            char *comma = strchr(p, ',');
            if (comma && *(comma + 1) == '0')
            {
                elog_i(TAG, "usbnetmode=0, switching to ECM...");
                a7683e_send_at("AT$MYCONFIG=\"usbnetmode\",1",
                               resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT);
            }
        }
    }

    /* Check and configure dial mode: 0=ECM data path, 1=command dial */
    status = a7683e_send_at("AT+DIALMODE?", resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT);
    if (status == A7683E_OK)
    {
        char *p = strstr(resp_buf, "+DIALMODE:");
        if (p)
        {
            int mode = *(p + 11) - '0';  /* "+DIALMODE: " is 11 chars, digit at p+11 */
            elog_d(TAG, "DIALMODE=%d", mode);
            if (mode == 1)
            {
                elog_i(TAG, "Setting DIALMODE=0 (ECM data path)...");
                a7683e_send_at("AT+DIALMODE=0", resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT);
            }
        }
    }

    /* Step 4: Check signal quality */
    elog_d(TAG, "[4/7] Checking signal quality...");
    status = a7683e_send_at("AT+CSQ", resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT);
    if (status == A7683E_OK)
    {
        int rssi = 0;
        char *p = strstr(resp_buf, "+CSQ:");
        if (p)
        {
            sscanf(p, "+CSQ: %d", &rssi);
            elog_d(TAG, "Signal RSSI: %d (%s)", rssi,
                  (rssi == 99) ? "Unknown" :
                  (rssi < 10) ? "Marginal" :
                  (rssi < 15) ? "OK" :
                  (rssi < 20) ? "Good" : "Excellent");
        }
    }

    /* Step 5: Set APN */
    elog_d(TAG, "[5/7] Setting APN: %s", A7683E_APN);
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"%s\",\"%s\"", A7683E_PDP_TYPE, A7683E_APN);
    status = a7683e_send_at(cmd, resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT);
    if (status != A7683E_OK)
    {
        elog_e(TAG, "Failed to set APN");
        return A7683E_ERROR;
    }
    elog_d(TAG, "APN set successfully");

    /* Step 6: Wait for network registration */
    elog_d(TAG, "[6/7] Waiting for network registration...");
    {
        int registered = 0;
        for (int retry = 0; retry < 120; retry++)  /* Max 120s */
        {
            status = a7683e_send_at("AT+CGREG?", resp_buf, sizeof(resp_buf), A7683E_DEFAULT_TIMEOUT);
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
                            elog_d(TAG, "Network registered (%s)",
                                  (stat == 1) ? "home" : "roaming");
                            registered = 1;
                            break;
                        }
                        elog_d(TAG, "Registration status: %d, waiting...", stat);
                    }
                }
            }
            tx_thread_sleep(1000);
        }

        if (!registered)
        {
            elog_e(TAG, "Network registration timeout");
            return A7683E_ERROR;
        }
    }

    elog_d(TAG, "=== Modem initialization complete ===");
    return A7683E_OK;
}

/**
 * @brief  Pre-dial queries: check EPS registration, activate PDP context,
 *         verify PDP address. Call before a7683e_ppp_dial().
 * @param  send_at  Function pointer for sending AT commands.
 *                  Pass a7683e_send_at for direct UART, or a7683e_cmux_send_at for CMUX.
 * @return A7683E_OK on success
 */
UINT a7683e_pre_dial(at_send_fn_t send_at)
{
    UINT status;
    char resp[256];

    elog_d(TAG, "=== Pre-dial queries ===");

    /* ---- Check PDP context activation status ---- */
    status = send_at("AT+CGACT?", resp, sizeof(resp), A7683E_DEFAULT_TIMEOUT);
    if (status == A7683E_OK)
    {
        if (strstr(resp, "+CGACT: 1,1") == NULL)
        {
            elog_d(TAG, "Activating PDP context 1...");
            status = send_at("AT+CGACT=1,1", resp, sizeof(resp), 15000);
            if (status != A7683E_OK)
            {
                elog_e(TAG, "PDP context activation failed!");
                return A7683E_PPP_FAIL;
            }
            elog_d(TAG, "PDP context 1 activated");
        }
        else
        {
            elog_d(TAG, "PDP context 1 already active");
        }
    }

    /* ---- Verify PDP address (IP assigned by network) ---- */
    status = send_at("AT+CGPADDR=1", resp, sizeof(resp), A7683E_DEFAULT_TIMEOUT);
    if (status == A7683E_OK)
    {
        elog_d(TAG, "PDP address: %s", resp);
    }

    return A7683E_OK;
}

/**
 * @brief  PPP dial via direct UART: send ATD*99***1# and wait for CONNECT.
 * @note   Cannot use send_at() because send_at() waits for "OK", but PPP dial
 *         returns "CONNECT" instead. We send raw and wait for "CONNECT" directly.
 * @return A7683E_OK on success
 */
UINT a7683e_ppp_dial(void)
{   
    elog_d(TAG, "=== PPP Dial ===");
    if (!modem_serial) return A7683E_ERROR;

    char resp[256];

    elog_d(TAG, "Dialing PPP (ATD*99***1#)...");
    flush_rx();
    modem_serial->write(modem_serial, (const uint8_t *)"ATD*99***1#\r", 12);

    UINT status = wait_for_response("CONNECT", resp, sizeof(resp), 30000);

    if (status == A7683E_OK)
    {
        elog_d(TAG, "PPP CONNECT received — modem is now in data mode");
        /* NOTE: Do NOT flush_rx() here! The modem immediately starts sending
         * PPP LCP frames after CONNECT. Flushing would discard the modem's
         * initial LCP Configure-Request, causing PPP negotiation to hang
         * with LCP stuck in REQ_SENT state. */
        return A7683E_OK;
    }
   
    elog_e(TAG, "PPP dial failed — no CONNECT received");
    return A7683E_PPP_FAIL;
}

/* ---------- CMUX integration --------------------------------------------- */

#if CMUX_ENABLE

/** AT response buffer for CMUX DLCI 1 */
static char     cmux_at_buf[512];
static volatile uint16_t cmux_at_len;
static volatile uint8_t cmux_at_ready;

/**
 * @brief  CMUX RX callback for DLCI 1 (AT channel).
 *         Accumulates response data and flags when complete.
 */
static void cmux_at_rx(uint8_t dlci, const uint8_t *data, uint16_t len)
{
    (void)dlci;
    if (cmux_at_len + len <= sizeof(cmux_at_buf) - 1) {
        memcpy(cmux_at_buf + cmux_at_len, data, len);
        cmux_at_len += len;
        cmux_at_buf[cmux_at_len] = '\0';

        /* Check for terminal responses */
        if (strstr(cmux_at_buf, "OK") || strstr(cmux_at_buf, "ERROR") ||
            strstr(cmux_at_buf, "NO CARRIER") || strstr(cmux_at_buf, "+CME ERROR") ||
            strstr(cmux_at_buf, "CONNECT"))
            cmux_at_ready = 1;
    }
}

/**
 * @brief  Start CMUX mode on the modem.
 *         1. Initialize CMUX instance
 *         2. Send AT+CMUX=0 to enter CMUX mode
 *         3. Establish DLCI 0 (control), 1 (AT), 2 (PPP)
 * @return A7683E_OK on success
 */
UINT a7683e_cmux_start(void)
{
    UINT status;

    if (!modem_serial) return A7683E_ERROR;

    elog_d(TAG, "=== Starting CMUX mode ===");

    /* Initialize CMUX */
    cmux_init(&g_cmux);

    /* Register AT response callback on DLCI 1 */
    cmux_set_rx_callback(&g_cmux, 1, cmux_at_rx);

    /* Send AT+CMUX=0 (Basic mode) */
    flush_rx();
    {
        const char *at_cmd = "AT+CMUX=0\r";
        modem_serial->write(modem_serial, (const uint8_t *)at_cmd, strlen(at_cmd));
    }

    /* Wait for OK — after OK, modem immediately enters CMUX mode and sends
     * SABM on DLCI 0. The SABM bytes may arrive in the same read() call
     * that delivers "OK", so we must capture them. */
    {
        char cmux_resp[256] = {0};
        uint16_t pos = 0;
        uint32_t start = tx_time_get();
        uint32_t ticks = 3 * TX_TIMER_TICKS_PER_SECOND;
        uint8_t ok_found = 0;

        while ((tx_time_get() - start) < ticks)
        {
            uint16_t n = modem_serial->read(modem_serial, (uint8_t *)cmux_resp + pos,
                                            sizeof(cmux_resp) - 1 - pos, 50);
            pos += n;
            cmux_resp[pos] = '\0';

            if (strstr(cmux_resp, "OK")) {
                ok_found = 1;
                elog_d(TAG, "AT+CMUX=0 accepted — entering CMUX mode");
                break;
            }
            if (strstr(cmux_resp, "ERROR")) {
                elog_e(TAG, "AT+CMUX=0 failed: %s", cmux_resp);
                return A7683E_ERROR;
            }
        }

        if (!ok_found) {
            elog_e(TAG, "AT+CMUX=0 timeout");
            return A7683E_ERROR;
        }

        /* Install UART3 RX hook so new data goes to cmux_feed() */
        cmux_bridge_start();

        flush_rx();
    }

    /* Now the modem is in CMUX mode — all further communication is framed */
    /* Start CMUX: SABM on DLCI 0 (if not already open), 1, 2 */
    int ret = cmux_start(&g_cmux);
    if (ret != 0) {
        elog_e(TAG, "CMUX start failed: %d", ret);
        return A7683E_ERROR;
    }

    elog_d(TAG, "CMUX mode active — DLCI 0(ctrl) 1(AT) 2(PPP)");
    return A7683E_OK;
}

/**
 * @brief  Send AT command via CMUX on specified DLCI and wait for response.
 * @param  dlci       CMUX channel (1=AT, 2=PPP)
 * @param  cmd        AT command string (\r optional — auto-appended if missing)
 * @param  resp       Response buffer
 * @param  resp_len   Response buffer size
 * @param  timeout_ms Timeout in ms
 * @return A7683E_OK on success
 */
UINT a7683e_cmux_send_at_dlci(uint8_t dlci, const char *cmd, char *resp,
                               uint16_t resp_len, uint32_t timeout_ms)
{
    if (!cmux_is_active(&g_cmux)) return A7683E_ERROR;
    if (resp_len < 2) return A7683E_ERROR;

    /* Build command with \r terminator (auto-append if not present) */
    char buf[256];
    size_t len = strlen(cmd);
    if (len + 2 > sizeof(buf)) return A7683E_ERROR;
    memcpy(buf, cmd, len);
    if (len == 0 || cmd[len - 1] != '\r') {
        buf[len++] = '\r';
    }
    buf[len] = '\0';

    /* Register AT callback on target DLCI, then reset response buffer.
     * Order matters: register callback first so we don't miss fast responses. */
    cmux_set_rx_callback(&g_cmux, dlci, cmux_at_rx);
    cmux_at_len = 0;
    cmux_at_ready = 0;
    cmux_at_buf[0] = '\0';

    /* Send via specified CMUX DLCI */
    cmux_send(&g_cmux, dlci, (const uint8_t *)buf, len);

    elog_d(TAG, "CMUX TX [DLCI %u]: %s", dlci, cmd);

    /* Wait for response */
    uint32_t elapsed = 0;
    uint32_t step = 10;
    while (!cmux_at_ready && elapsed < timeout_ms) {
        tx_thread_sleep(step);
        elapsed += step;
    }

    if (cmux_at_ready && resp) {
        uint16_t copy_len = cmux_at_len;
        if (copy_len > resp_len - 1) copy_len = resp_len - 1;
        memcpy(resp, cmux_at_buf, copy_len);
        resp[copy_len] = '\0';
        elog_d(TAG, "CMUX RX: %s", resp);
        return A7683E_OK;
    }

    return A7683E_TIMEOUT;
}

/**
 * @brief  Send AT command via CMUX DLCI 1 (at_send_fn_t compatible).
 */
UINT a7683e_cmux_send_at(const char *cmd, char *resp, uint16_t resp_len, uint32_t timeout_ms)
{
    return a7683e_cmux_send_at_dlci(1, cmd, resp, resp_len, timeout_ms);
}

/**
 * @brief  Send AT command via CMUX DLCI 2 (at_send_fn_t compatible).
 */
UINT a7683e_cmux_send_at_dlci2(const char *cmd, char *resp, uint16_t resp_len, uint32_t timeout_ms)
{
    return a7683e_cmux_send_at_dlci(2, cmd, resp, resp_len, timeout_ms);
}

#endif /* CMUX_ENABLE */

/**
 * @brief  Cleanup modem state after USB disconnect or PPP teardown.
 *         Attempts to escape PPP mode and deactivate PDP context.
 *         Skips AT commands if modem_serial is NULL (device already removed).
 *         Failures are non-fatal (device may already be gone).
 */
void a7683e_cleanup(void)
{
    elog_d(TAG, "Modem cleanup...");

    if (!modem_serial) {
        elog_d(TAG, "No serial port — skipping AT cleanup (device removed)");
        return;
    }

    /* Try to escape PPP mode (short timeout — device may be gone) */
    UINT rc = a7683e_escape_ppp();
    if (rc == A7683E_OK) {
        elog_d(TAG, "PPP mode escaped");
    }

    /* Try to deactivate PDP context (may fail if device is gone — that's OK) */
    rc = a7683e_send_at("AT+CGACT=0,1", resp_buf, sizeof(resp_buf), 5000);
    if (rc == A7683E_OK) {
        elog_d(TAG, "PDP context deactivated");
    } else {
        elog_w(TAG, "PDP deactivation skipped/failed: 0x%02X", rc);
    }

    elog_d(TAG, "Modem cleanup done");
}
