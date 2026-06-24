/**
  ******************************************************************************
  * @file    cmux_serial.c
  * @brief   CMUX 虚拟串口层 — 为每个数据 DLCI 提供 bsp_serial_t 接口
  *
  * 数据流：
  *   RX: UART3 → cmux_feed() 解析帧 → 按 DLCI 写入对应 ringbuf → 应用层 read()
  *   TX: 应用层 write() → cmux_send() 封装帧 → UART3 发出
  *
  * DLCI 0 是控制通道，不分配串口。
  * DLCI 1 ~ CMUX_NUM_CHANNELS 各自独立 ringbuf + event flags。
  ******************************************************************************
  */

#include "cmux_serial.h"
#include "lwrb.h"
#include "cmux.h"
#include "bsp_uart3.h"
#include "elog.h"
#include "tx_api.h"
#include <string.h>

#if CMUX_ENABLE

#define LOG_TAG "CMUX_SER"

/* ---------- 每个 DLCI 的接收缓冲区大小 ---------- */
#define RX_BUF_SIZE   4096u

/* ---------- 每个 DLCI 的上下文 ---------- */
typedef struct {
    lwrb_t                rx_ring;
    uint8_t               rx_buf[RX_BUF_SIZE];
    TX_EVENT_FLAGS_GROUP  events;
} dlci_ctx_t;

/* ---------- 静态存储 ---------- */
static dlci_ctx_t   g_ctx[CMUX_NUM_CHANNELS];
static bsp_serial_t g_serial[CMUX_NUM_CHANNELS];

/* ================================================================== */
/*  CMUX 帧接收回调 — cmux_feed() 解析后按 DLCI 分发到这里            */
/* ================================================================== */

static void dlci_rx_callback(uint8_t dlci, const uint8_t *data, uint16_t len)
{
    if (dlci < 1 || dlci > CMUX_NUM_CHANNELS) return;

    dlci_ctx_t *ctx = &g_ctx[dlci - 1];
    lwrb_write(&ctx->rx_ring, data, len);
    tx_event_flags_set(&ctx->events, 0x01, TX_OR);
}

/* ================================================================== */
/*  每个 DLCI 需要独立的 read/write 函数（bsp_serial_t 无 self 参数）  */
/*  用宏生成，避免手写重复代码                                         */
/* ================================================================== */

/*
 * 展开后每个 DLCI 生成两个 static 函数：
 *   cmux_read_0()  / cmux_write_0()   — DLCI 1
 *   cmux_read_1()  / cmux_write_1()   — DLCI 2
 *   ...
 */

/* Stub implementations for init/rx_available (CMUX uses event-driven read) */
static void cmux_port_init_stub(bsp_serial_t *self) { (void)self; }
static uint16_t cmux_port_rx_avail_stub(bsp_serial_t *self) { (void)self; return 0; }
#define DEFINE_DLCI_IO(n)                                                                    \
static uint16_t cmux_read_##n(bsp_serial_t *self, uint8_t *buf, uint16_t len, uint32_t timeout_ms) \
{                                                                                             \
    (void)self;                                                                               \
    dlci_ctx_t *ctx = &g_ctx[n];                                                              \
    while (lwrb_get_full(&ctx->rx_ring) == 0) {                                               \
        if (timeout_ms == 0) return 0;                                                        \
        ULONG flags;                                                                          \
        ULONG ticks = (timeout_ms == UINT32_MAX) ? TX_WAIT_FOREVER : timeout_ms;              \
        if (tx_event_flags_get(&ctx->events, 0x01, TX_OR_CLEAR, &flags, ticks)                \
            != TX_SUCCESS) return 0;                                                          \
    }                                                                                         \
    return (uint16_t)lwrb_read(&ctx->rx_ring, buf, len);                                      \
}                                                                                             \
static void cmux_write_##n(bsp_serial_t *self, const uint8_t *data, uint16_t len)             \
{                                                                                             \
    (void)self;                                                                               \
    if (cmux_is_active(&g_cmux))                                                              \
        cmux_send(&g_cmux, (n) + 1, data, len);                                              \
}

/* 按 CMUX_NUM_CHANNELS 生成，增减通道只需改 cmux.h 里的宏 */
DEFINE_DLCI_IO(0)   /* DLCI 1 */
#if CMUX_NUM_CHANNELS >= 2
DEFINE_DLCI_IO(1)   /* DLCI 2 */
#endif
#if CMUX_NUM_CHANNELS >= 3
DEFINE_DLCI_IO(2)   /* DLCI 3 */
#endif
#if CMUX_NUM_CHANNELS >= 4
DEFINE_DLCI_IO(3)   /* DLCI 4 */
#endif

/* 函数指针表 — 按 DLCI 索引（0=DLCI1, 1=DLCI2, ...） */
static uint16_t (*const read_fn[])(bsp_serial_t *, uint8_t *, uint16_t, uint32_t) = {
    cmux_read_0,
#if CMUX_NUM_CHANNELS >= 2
    cmux_read_1,
#endif
#if CMUX_NUM_CHANNELS >= 3
    cmux_read_2,
#endif
#if CMUX_NUM_CHANNELS >= 4
    cmux_read_3,
#endif
};

static void (*const write_fn[])(bsp_serial_t *, const uint8_t *, uint16_t) = {
    cmux_write_0,
#if CMUX_NUM_CHANNELS >= 2
    cmux_write_1,
#endif
#if CMUX_NUM_CHANNELS >= 3
    cmux_write_2,
#endif
#if CMUX_NUM_CHANNELS >= 4
    cmux_write_3,
#endif
};

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

bsp_serial_t *cmux_serial_get(uint8_t dlci)
{
    if (dlci < 1 || dlci > CMUX_NUM_CHANNELS) return NULL;
    return &g_serial[dlci - 1];
}

void cmux_serial_init_all(void)
{
    for (uint8_t i = 0; i < CMUX_NUM_CHANNELS; i++) {
        dlci_ctx_t   *ctx    = &g_ctx[i];
        bsp_serial_t *serial = &g_serial[i];

        /* 初始化 ringbuf + 事件 */
        memset(ctx, 0, sizeof(*ctx));
        lwrb_init(&ctx->rx_ring, ctx->rx_buf, RX_BUF_SIZE);
        tx_event_flags_create(&ctx->events, "cmux_ev");

        /* 绑定串口 */
        memset(serial, 0, sizeof(*serial));
        serial->name         = "CMUX";
        serial->read         = read_fn[i];
        serial->write        = write_fn[i];
        serial->init         = cmux_port_init_stub;
        serial->rx_available = cmux_port_rx_avail_stub;

        /* 注册 CMUX 帧回调 — cmux_feed 解析后调 dlci_rx_callback 写 ringbuf */
        cmux_set_rx_callback(&g_cmux, i + 1, dlci_rx_callback);

        elog_d(LOG_TAG, "DLCI %u ready (RX %uKB)", i + 1, RX_BUF_SIZE / 1024);
    }
}

#endif /* CMUX_ENABLE */
