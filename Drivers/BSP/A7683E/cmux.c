/**
  ******************************************************************************
  * @file    cmux.c
  * @brief   GSM 07.10 / 3GPP TS 27.010 CMUX multiplexer implementation
  ******************************************************************************
  * @note
  *   Frame format (Basic mode — no byte stuffing):
  *     7E | Address | Control | Length | [Data] | FCS | 7E
  *
  *   Frame format (Advanced mode — byte stuffing with escape sequences):
  *     7E | Address(escaped) | Control(escaped) | Length(escaped) | [Data(escaped)] | FCS(escaped) | 7E
  ******************************************************************************
  */

#include "cmux.h"
#include "elog.h"
#include "tx_api.h"
#include <string.h>

#if CMUX_ENABLE

#define TAG "CMUX"

/** CMUX frame flag byte (SIMCom modems use 0xF9, not standard 0x7E) */
#define CMUX_FLAG   0xF9

/** CMUX frame escape byte */
#define CMUX_ESC    0x7D

/* ---------- FCS table (CRC-8 per GSM 07.10) ------------------------------ */

static const uint8_t fcs_table[256] = {
    0x00, 0x91, 0xE3, 0x72, 0x07, 0x96, 0xE4, 0x75,
    0x0E, 0x9F, 0xED, 0x7C, 0x09, 0x98, 0xEA, 0x7B,
    0x1C, 0x8D, 0xFF, 0x6E, 0x1B, 0x8A, 0xF8, 0x69,
    0x12, 0x83, 0xF1, 0x60, 0x15, 0x84, 0xF6, 0x67,
    0x38, 0xA9, 0xDB, 0x4A, 0x3F, 0xAE, 0xDC, 0x4D,
    0x36, 0xA7, 0xD5, 0x44, 0x31, 0xA0, 0xD2, 0x43,
    0x24, 0xB5, 0xC7, 0x56, 0x23, 0xB2, 0xC0, 0x51,
    0x2A, 0xBB, 0xC9, 0x58, 0x2D, 0xBC, 0xCE, 0x5F,
    0x70, 0xE1, 0x93, 0x02, 0x77, 0xE6, 0x94, 0x05,
    0x7E, 0xEF, 0x9D, 0x0C, 0x79, 0xE8, 0x9A, 0x0B,
    0x6C, 0xFD, 0x8F, 0x1E, 0x6B, 0xFA, 0x88, 0x19,
    0x62, 0xF3, 0x81, 0x10, 0x65, 0xF4, 0x86, 0x17,
    0x48, 0xD9, 0xAB, 0x3A, 0x4F, 0xDE, 0xAC, 0x3D,
    0x46, 0xD7, 0xA5, 0x34, 0x41, 0xD0, 0xA2, 0x33,
    0x54, 0xC5, 0xB7, 0x26, 0x53, 0xC2, 0xB0, 0x21,
    0x5A, 0xCB, 0xB9, 0x28, 0x5D, 0xCC, 0xBE, 0x2F,
    0xE0, 0x71, 0x03, 0x92, 0xE7, 0x76, 0x04, 0x95,
    0xEE, 0x7F, 0x0D, 0x9C, 0xE9, 0x78, 0x0A, 0x9B,
    0xFC, 0x6D, 0x1F, 0x8E, 0xFB, 0x6A, 0x18, 0x89,
    0xF2, 0x63, 0x11, 0x80, 0xF5, 0x64, 0x16, 0x87,
    0xD8, 0x49, 0x3B, 0xAA, 0xDF, 0x4E, 0x3C, 0xAD,
    0xD6, 0x47, 0x35, 0xA4, 0xD1, 0x40, 0x32, 0xA3,
    0xC4, 0x55, 0x27, 0xB6, 0xC3, 0x52, 0x20, 0xB1,
    0xCA, 0x5B, 0x29, 0xB8, 0xCD, 0x5C, 0x2E, 0xBF,
    0x90, 0x01, 0x73, 0xE2, 0x97, 0x06, 0x74, 0xE5,
    0x9E, 0x0F, 0x7D, 0xEC, 0x99, 0x08, 0x7A, 0xEB,
    0x8C, 0x1D, 0x6F, 0xFE, 0x8B, 0x1A, 0x68, 0xF9,
    0x82, 0x13, 0x61, 0xF0, 0x85, 0x14, 0x66, 0xF7,
    0xA8, 0x39, 0x4B, 0xDA, 0xAF, 0x3E, 0x4C, 0xDD,
    0xA6, 0x37, 0x45, 0xD4, 0xA1, 0x30, 0x42, 0xD3,
    0xB4, 0x25, 0x57, 0xC6, 0xB3, 0x22, 0x50, 0xC1,
    0xBA, 0x2B, 0x59, 0xC8, 0xBD, 0x2C, 0x5E, 0xCF
};

/**
 * @brief  Calculate FCS over a buffer using the GSM 07.10 CRC table.
 */
static uint8_t fcs_calc(const uint8_t *data, uint16_t len)
{
    uint8_t fcs = 0xFF;
    for (uint16_t i = 0; i < len; i++)
        fcs = fcs_table[fcs ^ data[i]];
    return 0xFF - fcs;
}

/* ---------- Address field helpers ----------------------------------------- */

/**
 * @brief  Build address field. Format per GSM 07.10:
 *         Bit 0: EA (always 1 for 1-byte address)
 *         Bit 1: C/R (1=command from DTE, 0=response from DTE)
 *         Bit 2-7: DLCI (6 bits)
 *         Note: P/F bit is ONLY in the control field, NOT in address.
 */
static uint8_t make_address(uint8_t dlci, uint8_t cr)
{
    return (dlci << 2) | (cr ? CMUX_ADDR_CR : 0) | CMUX_ADDR_EA;
}

static uint8_t get_dlci(uint8_t addr)
{
    return (addr >> 2) & 0x3F;
}

/* ---------- Frame name for debug ----------------------------------------- */

static const char *frame_name(uint8_t ctrl)
{
    switch (ctrl & ~0x10) {  /* Mask P/F bit */
    case CMUX_SABM: return "SABM";
    case CMUX_UA:   return "UA";
    case CMUX_DM:   return "DM";
    case CMUX_DISC: return "DISC";
    case CMUX_UIH:  return "UIH";
    default:        return "???";
    }
}

/* ---------- Send a raw CMUX frame ---------------------------------------- */

/**
 * @brief  Build and send a CMUX frame.
 * @param  dlci   Channel number
 * @param  ctrl   Frame control byte (SABM, UA, DM, DISC, UIH)
 * @param  data   Payload (NULL for SABM/UA/DM/DISC)
 * @param  len    Payload length
 * @param  cr     1 = Command, 0 = Response
 */
static void cmux_frame_send(cmux_t *ctx, uint8_t dlci, uint8_t ctrl,
                            const uint8_t *data, uint16_t len, uint8_t cr)
{
    /* Static buffer — avoid 1516-byte stack allocation that overflows
     * the 1024-byte ThreadX thread stack. Safe because this function
     * is not reentrant: thread context calls it for SABM/DISC/UIH,
     * ISR context calls it only for UA responses, and UART ISR has
     * higher priority than thread code (no concurrent access). */
    static uint8_t buf[CMUX_N1 + 16];
    uint16_t pos = 0;

    /* Opening flag */
    buf[pos++] = CMUX_FLAG;

    /* Address: DLCI + C/R + EA (P/F bit is in control field only) */
    buf[pos++] = make_address(dlci, cr);

    /* Control field */
    buf[pos++] = ctrl;

    /* Length (1 or 2 bytes, EA=1 for 1-byte, EA=0 for 2-byte) */
    if (len <= 127) {
        buf[pos++] = (uint8_t)(len << 1) | CMUX_ADDR_EA;
    } else {
        buf[pos++] = (uint8_t)(len & 0x7F);           /* Low 7 bits, EA=0 */
        buf[pos++] = (uint8_t)(len >> 7);              /* High 8 bits */
    }

    /* Data */
    if (data && len > 0) {
        memcpy(&buf[pos], data, len);
        pos += len;
    }

    /* FCS — calculated over address + control + length (+ data for non-UIH) */
    /* For UIH: FCS covers address + control + length only (not data) */
    uint16_t fcs_len = 2;  /* address + control */
    if (len <= 127) fcs_len += 1;
    else fcs_len += 2;
    buf[pos++] = fcs_calc(&buf[1], fcs_len);

    /* Closing flag */
    buf[pos++] = CMUX_FLAG;

    /* Send via UART */
    cmux_port_write(buf, pos);

    ctx->tx_frames++;

    elog_d(TAG, "TX [%u] DLCI=%u %s:", pos, dlci, frame_name(ctrl & ~0x10));
    //elog_hexdump(TAG, 16, buf, pos);
}

/* ---------- Process received frame --------------------------------------- */

static void cmux_process_frame(cmux_t *ctx, cmux_frame_t *frame)
{
    uint8_t dlci = get_dlci(frame->address);
    uint8_t ctrl = frame->control & ~0x10;  /* Mask P/F bit */

    elog_d(TAG, "RX DLCI=%u %s len=%u", dlci, frame_name(ctrl), frame->length);

    switch (ctrl) {
    case CMUX_SABM:
        /* Peer requests channel setup — send UA response */
        if (dlci < CMUX_MAX_DLCI) {
            ctx->channels[dlci].state = CMUX_CH_OPEN;
            ctx->channels[dlci].dlci = dlci;
        }
        cmux_frame_send(ctx, dlci, CMUX_UA, NULL, 0, 0);
        elog_d(TAG, "DLCI %u opened (SABM received, UA sent)", dlci);
        break;

    case CMUX_UA:
        /* Acknowledge to our SABM or DISC */
        if (dlci < CMUX_MAX_DLCI) {
            if (ctx->channels[dlci].state == CMUX_CH_SABM_SENT) {
                ctx->channels[dlci].state = CMUX_CH_OPEN;
                elog_d(TAG, "DLCI %u opened (UA received)", dlci);
            } else if (ctx->channels[dlci].state == CMUX_CH_DISC_SENT) {
                ctx->channels[dlci].state = CMUX_CH_CLOSED;
                elog_d(TAG, "DLCI %u closed (UA received)", dlci);
            }
        }
        break;

    case CMUX_DM:
        /* Peer refuses channel */
        if (dlci < CMUX_MAX_DLCI) {
            ctx->channels[dlci].state = CMUX_CH_CLOSED;
            elog_w(TAG, "DLCI %u refused (DM received)", dlci);
        }
        break;

    case CMUX_DISC:
        /* Peer requests disconnect */
        if (dlci < CMUX_MAX_DLCI)
            ctx->channels[dlci].state = CMUX_CH_CLOSED;
        cmux_frame_send(ctx, dlci, CMUX_UA, NULL, 0, 0);
        elog_d(TAG, "DLCI %u closed (DISC received, UA sent)", dlci);
        break;

    case CMUX_UIH:
        if (dlci == 0) {
            /* Control channel command */
            if (frame->length >= 2) {
                uint8_t cmd = frame->data[0];
                if (cmd == CMUX_CMD_CLD) {
                    elog_d(TAG, "Modem requests CMUX close-down");
                    ctx->active = 0;
                } else if (cmd == CMUX_CMD_MSC && frame->length >= 3) {
                    elog_d(TAG, "MSC on DLCI %u", frame->data[2] >> 2);
                } else {
                    elog_d(TAG, "Control cmd 0x%02X len=%u", cmd, frame->data[1] >> 1);
                }
            }
        } else if (dlci < CMUX_MAX_DLCI && ctx->channels[dlci].rx_callback) {
            /* Data channel — dispatch to registered callback */
            ctx->channels[dlci].rx_callback(dlci, frame->data, frame->length);
        }
        break;

    default:
        elog_w(TAG, "Unknown frame type 0x%02X on DLCI %u", ctrl, dlci);
        break;
    }
}

/* ---------- RX byte-by-byte parser --------------------------------------- */

void cmux_feed(cmux_t *ctx, const uint8_t *data, uint16_t len)
{
    /* elog_hexdump disabled — snprintf uses ~300 bytes stack,
     * can overflow CMUX thread stack with nested calls */
    // elog_d(TAG, "cmux_feed: %u bytes, state=%u", len, ctx->rx_state);
    // elog_hexdump(TAG, 16, data, len);

    for (uint16_t i = 0; i < len; i++) {
        uint8_t b = data[i];

        /* State machine */
        switch (ctx->rx_state) {
        case CMUX_RX_IDLE:
            if (b == CMUX_FLAG) {
                ctx->rx_pos = 0;
                ctx->rx_state = CMUX_RX_FRAME;
                elog_d(TAG, "RX frame start (byte %u/%u)", i, len);
            }
            break;

        case CMUX_RX_ESCAPED:
            /* Escaped byte — XOR with 0x20 to get actual value */
            if (ctx->rx_pos < sizeof(ctx->rx_buf)) {
                ctx->rx_buf[ctx->rx_pos++] = b ^ 0x20;
            }
            ctx->rx_state = CMUX_RX_FRAME;
            break;

        case CMUX_RX_FRAME:
            if (b == CMUX_FLAG) {
                /* Closing flag — process frame if we have enough data */
                if (ctx->rx_pos >= 4) {
                    /* Minimum frame: addr + ctrl + len + FCS = 4 bytes */
                    /* Verify FCS */
                    uint16_t fcs_len = 2;  /* addr + ctrl */
                    uint8_t len_byte = ctx->rx_buf[2];
                    if (len_byte & CMUX_ADDR_EA) {
                        fcs_len += 1;
                    } else {
                        fcs_len += 2;
                    }

                    uint8_t expected_fcs = fcs_calc(ctx->rx_buf, fcs_len);
                    uint8_t actual_fcs = ctx->rx_buf[fcs_len];

                    /* Log received raw frame — hexdump disabled to save stack */
                    elog_d(TAG, "RX [%u+2] FCS=%s:", ctx->rx_pos,
                          (expected_fcs == actual_fcs) ? "OK" : "ERR");
                    // elog_hexdump(TAG, 16, ctx->rx_buf, ctx->rx_pos);

                    if (expected_fcs == actual_fcs) {
                        /* Parse frame */
                        cmux_frame_t frame;
                        frame.address = ctx->rx_buf[0];
                        frame.control = ctx->rx_buf[1];

                        /* Parse length */
                        if (len_byte & CMUX_ADDR_EA) {
                            frame.length = len_byte >> 1;
                            frame.data = &ctx->rx_buf[3];
                        } else {
                            frame.length = (len_byte >> 1) | (ctx->rx_buf[3] << 7);
                            frame.data = &ctx->rx_buf[4];
                        }

                        cmux_process_frame(ctx, &frame);
                        ctx->rx_frames++;
                    } else {
                        ctx->fcs_errors++;
                        elog_w(TAG, "FCS error: expected 0x%02X got 0x%02X",
                              expected_fcs, actual_fcs);
                    }
                }

                /* Reset for next frame (may be start of new frame) */
                ctx->rx_pos = 0;
                /* Stay in CMUX_RX_FRAME — next CMUX_FLAG might be start of new frame */
            } else if (b == CMUX_ESC) {
                /* Escape byte — next byte is XOR'd with 0x20 */
                ctx->rx_state = CMUX_RX_ESCAPED;
            } else {
                /* Store byte */
                if (ctx->rx_pos < sizeof(ctx->rx_buf)) {
                    ctx->rx_buf[ctx->rx_pos++] = b;
                } else {
                    /* Buffer overflow — discard frame */
                    ctx->rx_errors++;
                    ctx->rx_state = CMUX_RX_IDLE;
                    elog_w(TAG, "RX buffer overflow, discarding frame");
                }
            }
            break;
        }
    }
}

/* ---------- Public API --------------------------------------------------- */

void cmux_init(cmux_t *ctx)
{
    memset(ctx, 0, sizeof(cmux_t));

    /* Initialize channel info */
    for (uint8_t i = 0; i < CMUX_MAX_DLCI; i++) {
        ctx->channels[i].dlci = i;
        ctx->channels[i].state = CMUX_CH_CLOSED;
        ctx->channels[i].mtu = CMUX_N1;
    }
}

int cmux_start(cmux_t *ctx)
{
    elog_d(TAG, "Starting CMUX (mode=%d, N1=%d)", CMUX_MODE, CMUX_N1);

    ctx->active = 1;

    /* DTE-initiated CMUX setup: send SABM on DLCI 0, 1, 2 sequentially.
     * Wait for UA response after each SABM before proceeding.
     * If modem sends SABM first (some modems do), cmux_process_frame()
     * auto-responds with UA and marks the channel OPEN. */
    for (uint8_t dlci = 0; dlci < CMUX_MAX_DLCI; dlci++) {
        /* Skip if already opened by modem-initiated SABM */
        if (ctx->channels[dlci].state == CMUX_CH_OPEN) {
            elog_d(TAG, "DLCI %u already open (modem-initiated)", dlci);
            tx_thread_sleep(10);  /* Small delay to let modem finish setup */
            continue;
        }

        /* Send SABM (P/F=1) */
        ctx->channels[dlci].state = CMUX_CH_SABM_SENT;
        cmux_frame_send(ctx, dlci, CMUX_SABM, NULL, 0, 1);
        elog_d(TAG, "SABM sent on DLCI %u, waiting for UA...", dlci);

        /* Wait for UA response */
        uint32_t timeout = CMUX_T1_MS / 10;
        while (timeout-- > 0 && ctx->channels[dlci].state != CMUX_CH_OPEN) {
            tx_thread_sleep(10);
        }

        if (ctx->channels[dlci].state != CMUX_CH_OPEN) {
            elog_e(TAG, "DLCI %u SABM failed (no UA received, rx_frames=%u)",
                  dlci, ctx->rx_frames);
            ctx->active = 0;
            return -1;
        }

        elog_d(TAG, "DLCI %u opened", dlci);
    }

    elog_d(TAG, "CMUX active — DLCI 0(ctrl) 1(AT) 2(PPP) ready");
    return 0;
}

void cmux_stop(cmux_t *ctx)
{
    /* Send DISC on all open channels (reverse order) */
    for (int i = CMUX_MAX_DLCI - 1; i >= 0; i--) {
        if (ctx->channels[i].state == CMUX_CH_OPEN) {
            ctx->channels[i].state = CMUX_CH_DISC_SENT;
            cmux_frame_send(ctx, i, CMUX_DISC | CMUX_PF_BIT, NULL, 0, 1);
            tx_thread_sleep(10);
        }
    }
    ctx->active = 0;
}

int cmux_send(cmux_t *ctx, uint8_t dlci, const uint8_t *data, uint16_t len)
{
    if (!ctx->active) return -1;
    if (dlci >= CMUX_MAX_DLCI) return -2;
    if (ctx->channels[dlci].state != CMUX_CH_OPEN) return -3;

    /* Split into chunks if payload exceeds MTU */
    uint16_t offset = 0;
    while (offset < len) {
        uint16_t chunk = len - offset;
        if (chunk > ctx->channels[dlci].mtu)
            chunk = ctx->channels[dlci].mtu;

        cmux_frame_send(ctx, dlci, CMUX_UIH, data + offset, chunk, 1);
        offset += chunk;
    }
    return 0;
}

void cmux_set_rx_callback(cmux_t *ctx, uint8_t dlci,
                          void (*callback)(uint8_t dlci, const uint8_t *data, uint16_t len))
{
    if (dlci < CMUX_MAX_DLCI)
        ctx->channels[dlci].rx_callback = callback;
}

#endif