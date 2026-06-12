/**
  ******************************************************************************
  * @file    cmux.h
  * @brief   GSM 07.10 / 3GPP TS 27.010 CMUX multiplexer
  ******************************************************************************
  * @note
  *   Multiplexes a single UART into multiple logical channels:
  *   - DLCI 0: Control channel (CMUX management)
  *   - DLCI 1: AT command channel
  *   - DLCI 2: PPP data channel
  *
  *   Supports Basic mode (no byte stuffing) and Advanced mode (byte stuffing).
  ******************************************************************************
  */

#ifndef CMUX_H
#define CMUX_H

/** Master enable switch — set to 0 to exclude all CMUX code */
#ifndef CMUX_ENABLE
#define CMUX_ENABLE             0
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* ---------- Configuration ------------------------------------------------- */

/** Maximum frame payload size (N1 parameter) */
#ifndef CMUX_N1
#define CMUX_N1                 1500
#endif

/** Response timeout in ms (T1) */
#ifndef CMUX_T1_MS
#define CMUX_T1_MS              1000
#endif

/** CMUX mode: 0 = Basic, 1 = Advanced */
#ifndef CMUX_MODE
#define CMUX_MODE               0
#endif

/** Number of logical channels (excluding DLCI 0 control) */
#ifndef CMUX_NUM_CHANNELS
#define CMUX_NUM_CHANNELS       2
#endif

/** DLCI assignments for data channels (1-based) */
#ifndef CMUX_AT_DLCI
#define CMUX_AT_DLCI            1       /* AT command channel */
#endif

#ifndef CMUX_PPP_DLCI
#define CMUX_PPP_DLCI           2       /* PPP data channel */
#endif

/** Maximum number of DLCIs (including control) */
#define CMUX_MAX_DLCI           (CMUX_NUM_CHANNELS + 1)

/* ---------- Frame types --------------------------------------------------- */

#define CMUX_SABM                0x2F    /* Set Asynchronous Balanced Mode */
#define CMUX_UA                  0x63    /* Unnumbered Acknowledgement */
#define CMUX_DM                  0x0F    /* Disconnected Mode */
#define CMUX_DISC                0x43    /* Disconnect */
#define CMUX_UIH                 0xEF    /* Unnumbered Information with Header check */

/** P/F (Poll/Final) bit in control field — must be set for SABM/DISC commands */
#define CMUX_PF_BIT             0x10

/* ---------- Control channel commands -------------------------------------- */

#define CMUX_CMD_CLD            0xC0    /* Close Down */
#define CMUX_CMD_MSC            0x38    /* Modem Status Command */

/* ---------- Address field encoding ---------------------------------------- */

#define CMUX_ADDR_EA            0x01    /* Extension bit (always 1 for last byte) */
#define CMUX_ADDR_CR            0x02    /* Command/Response bit */

/* ---------- Channel state ------------------------------------------------- */

typedef enum {
    CMUX_CH_CLOSED = 0,
    CMUX_CH_SABM_SENT,
    CMUX_CH_OPEN,
    CMUX_CH_DISC_SENT,
} cmux_ch_state_t;

/* ---------- Channel info -------------------------------------------------- */

typedef struct {
    uint8_t             dlci;           /* Data Link Connection Identifier */
    volatile cmux_ch_state_t state;
    uint16_t            mtu;            /* Negotiated max frame size */
    /* Callback: called when data is received on this channel */
    void               (*rx_callback)(uint8_t dlci, const uint8_t *data, uint16_t len);
} cmux_channel_t;

/* ---------- CMUX frame structure ------------------------------------------ */

typedef struct {
    uint8_t             address;        /* DLCI + C/R + EA */
    uint8_t             control;        /* Frame type + P/F */
    uint16_t            length;         /* Payload length */
    uint8_t            *data;           /* Payload pointer (points into rx buffer) */
} cmux_frame_t;

/* ---------- CMUX instance ------------------------------------------------- */

typedef struct {
    volatile uint8_t    active;         /* 1 = CMUX mode is active */
    cmux_channel_t      channels[CMUX_MAX_DLCI];

    /* RX state machine */
    uint8_t             rx_buf[CMUX_N1 + 16];  /* Frame assembly buffer */
    uint16_t            rx_pos;         /* Current position in rx_buf */
    enum {
        CMUX_RX_IDLE,                   /* Waiting for opening flag */
        CMUX_RX_FRAME,                  /* Receiving frame data */
        CMUX_RX_ESCAPED,               /* Next byte is escaped (XOR 0x20) */
    } rx_state;

    /* Statistics */
    uint32_t            rx_frames;
    uint32_t            tx_frames;
    uint32_t            rx_errors;
    uint32_t            fcs_errors;
} cmux_t;

/* ---------- Public API ---------------------------------------------------- */

#if CMUX_ENABLE

/** Global CMUX instance (defined in cmux_port.c) */
extern cmux_t g_cmux;

/**
 * @brief  Initialize CMUX instance. Call before sending AT+CMUX.
 */
void cmux_init(cmux_t *ctx);

/**
 * @brief  Start CMUX mode. Called after AT+CMUX is sent and modem enters CMUX.
 *         Sends SABM on DLCI 0, 1, 2 to establish channels.
 * @return 0 on success, negative on error
 */
int cmux_start(cmux_t *ctx);

/**
 * @brief  Stop CMUX mode. Sends DISC on all channels.
 */
void cmux_stop(cmux_t *ctx);

/**
 * @brief  Feed raw bytes from UART into CMUX parser.
 *         Call this from the UART RX callback.
 * @param  data  Received bytes
 * @param  len   Number of bytes
 */
void cmux_feed(cmux_t *ctx, const uint8_t *data, uint16_t len);

/**
 * @brief  Send data on a specific DLCI channel.
 * @param  dlci  Channel number (1=AT, 2=PPP)
 * @param  data  Payload
 * @param  len   Payload length
 * @return 0 on success, negative on error
 */
int cmux_send(cmux_t *ctx, uint8_t dlci, const uint8_t *data, uint16_t len);

/**
 * @brief  Register a receive callback for a channel.
 * @param  dlci      Channel number
 * @param  callback  Function called when data arrives on this channel
 */
void cmux_set_rx_callback(cmux_t *ctx, uint8_t dlci,
                          void (*callback)(uint8_t dlci, const uint8_t *data, uint16_t len));

/**
 * @brief  Check if CMUX mode is active.
 */
static inline uint8_t cmux_is_active(cmux_t *ctx)
{
    return ctx->active;
}

/* ---------- Port functions (defined in cmux_port.c) ---------------------- */

/**
 * @brief  Port write — sends raw CMUX frame bytes to UART.
 *         Called by cmux_frame_send(). Implemented in cmux_port.c.
 */
void cmux_port_write(const uint8_t *data, uint16_t len);

/* ---------- Bridge API (defined in cmux_port.c) -------------------------- */

/**
 * @brief  Start CMUX bridge — install UART3 RX hook so data goes to cmux_feed().
 *         Call this before cmux_start().
 */
void cmux_bridge_start(void);

/**
 * @brief  Stop CMUX bridge — remove RX hook, restore ring buffer mode.
 */
void cmux_bridge_stop(void);

#endif /* CMUX_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* CMUX_H */
