/*
 * LwM2M Connectivity Statistics Object (/7) implementation for Anjay
 *
 * Resource definitions:
 *   0  SMS Tx Counter (R)        — SMS sent count (not implemented, returns 0)
 *   1  SMS Rx Counter (R)        — SMS received count (not implemented, returns 0)
 *   2  Tx Data (R)               — IP bytes sent (KB)
 *   3  Rx Data (R)               — IP bytes received (KB)
 *   4  Max Message Size (R)      — max IP packet size observed
 *   5  Average Message Size (R)  — average IP packet size
 *   6  Start / Reset (E)         — reset counters, begin collection
 *   7  Stop (E)                  — stop collection (keep counters)
 *   8  Collection Period (RW)    — collection period in seconds (0 = continuous)
 */

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include <anjay/anjay.h>
#include <anjay/core.h>
#include <avsystem/commons/avs_defs.h>

#include "nx_api.h"
#include "elog.h"
#include "app_anjay_conn_stats_object.h"

#define TAG "CONN_STATS"

/* LwM2M Connectivity Statistics Object Resource IDs */
#define RID_SMS_TX              0
#define RID_SMS_RX              1
#define RID_TX_DATA             2   /* KB */
#define RID_RX_DATA             3   /* KB */
#define RID_MAX_MSG_SIZE        4   /* Bytes */
#define RID_AVG_MSG_SIZE        5   /* Bytes */
#define RID_START_RESET         6   /* Execute */
#define RID_STOP                7   /* Execute */
#define RID_COLLECTION_PERIOD   8   /* Seconds */

/* External references */
extern NX_IP ip_0;

/* ---- Collection state ---- */

/* Snapshot at Start/Reset */
static ULONG base_tx_bytes = 0;
static ULONG base_rx_bytes = 0;
static ULONG base_tx_packets = 0;
static ULONG base_rx_packets = 0;

/* Cumulative stats (relative to base) */
static ULONG stat_tx_bytes = 0;
static ULONG stat_rx_bytes = 0;
static ULONG stat_tx_packets = 0;
static ULONG stat_rx_packets = 0;
static ULONG stat_max_msg_size = 0;

/* Collection control */
static UINT  collecting = 1;            /* 1 = collecting, 0 = stopped */
static ULONG collection_period = 0;     /* 0 = continuous */

/* ---- Object definition ---- */

typedef struct conn_stats_object_struct {
    const anjay_dm_object_def_t *def;
} conn_stats_object_t;

static inline conn_stats_object_t *
get_obj(const anjay_dm_object_def_t *const *obj_ptr) {
    assert(obj_ptr);
    return AVS_CONTAINER_OF(obj_ptr, conn_stats_object_t, def);
}

/* ---- Get current raw counters from NetXDuo ---- */

static void get_raw_stats(ULONG *tx_bytes, ULONG *rx_bytes,
                          ULONG *tx_packets, ULONG *rx_packets)
{
    ULONG t_pkt = 0, t_bytes = 0, r_pkt = 0, r_bytes = 0;

    nx_ip_info_get(&ip_0,
                   &t_pkt, &t_bytes, &r_pkt, &r_bytes,
                   NULL, NULL, NULL, NULL, NULL, NULL);

    if (tx_bytes)   *tx_bytes   = t_bytes;
    if (rx_bytes)   *rx_bytes   = r_bytes;
    if (tx_packets) *tx_packets = t_pkt;
    if (rx_packets) *rx_packets = r_pkt;
}

/* ---- Update collected stats (called on read) ---- */

static void update_stats(void)
{
    if (!collecting) return;

    ULONG cur_tx, cur_rx, cur_tx_pkt, cur_rx_pkt;
    get_raw_stats(&cur_tx, &cur_rx, &cur_tx_pkt, &cur_rx_pkt);

    stat_tx_bytes   = cur_tx   - base_tx_bytes;
    stat_rx_bytes   = cur_rx   - base_rx_bytes;
    stat_tx_packets = cur_tx_pkt - base_tx_packets;
    stat_rx_packets = cur_rx_pkt - base_rx_packets;

    /* Track max message size (approximate: total bytes / packets) */
    if (stat_tx_packets > 0) {
        ULONG avg_tx = stat_tx_bytes / stat_tx_packets;
        if (avg_tx > stat_max_msg_size) stat_max_msg_size = avg_tx;
    }
    if (stat_rx_packets > 0) {
        ULONG avg_rx = stat_rx_bytes / stat_rx_packets;
        if (avg_rx > stat_max_msg_size) stat_max_msg_size = avg_rx;
    }
}

/* ---- LwM2M handlers ---- */

static int list_instances(anjay_t *anjay,
                          const anjay_dm_object_def_t *const *obj_ptr,
                          anjay_dm_list_ctx_t *ctx) {
    (void) anjay;
    (void) obj_ptr;
    anjay_dm_emit(ctx, 0);
    return 0;
}

static int list_resources(anjay_t *anjay,
                          const anjay_dm_object_def_t *const *obj_ptr,
                          anjay_iid_t iid,
                          anjay_dm_resource_list_ctx_t *ctx) {
    (void) anjay;
    (void) obj_ptr;
    (void) iid;

    anjay_dm_emit_res(ctx, RID_SMS_TX,            ANJAY_DM_RES_R,  ANJAY_DM_RES_PRESENT);
    anjay_dm_emit_res(ctx, RID_SMS_RX,            ANJAY_DM_RES_R,  ANJAY_DM_RES_PRESENT);
    anjay_dm_emit_res(ctx, RID_TX_DATA,           ANJAY_DM_RES_R,  ANJAY_DM_RES_PRESENT);
    anjay_dm_emit_res(ctx, RID_RX_DATA,           ANJAY_DM_RES_R,  ANJAY_DM_RES_PRESENT);
    anjay_dm_emit_res(ctx, RID_MAX_MSG_SIZE,      ANJAY_DM_RES_R,  ANJAY_DM_RES_PRESENT);
    anjay_dm_emit_res(ctx, RID_AVG_MSG_SIZE,      ANJAY_DM_RES_R,  ANJAY_DM_RES_PRESENT);
    anjay_dm_emit_res(ctx, RID_START_RESET,       ANJAY_DM_RES_E,  ANJAY_DM_RES_PRESENT);
    anjay_dm_emit_res(ctx, RID_STOP,              ANJAY_DM_RES_E,  ANJAY_DM_RES_PRESENT);
    anjay_dm_emit_res(ctx, RID_COLLECTION_PERIOD, ANJAY_DM_RES_RW, ANJAY_DM_RES_PRESENT);
    return 0;
}

static int resource_read(anjay_t *anjay,
                         const anjay_dm_object_def_t *const *obj_ptr,
                         anjay_iid_t iid,
                         anjay_rid_t rid,
                         anjay_riid_t riid,
                         anjay_output_ctx_t *ctx) {
    (void) anjay;
    (void) obj_ptr;
    assert(iid == 0);
    assert(riid == ANJAY_ID_INVALID);

    update_stats();

    switch (rid) {
    case RID_SMS_TX:
        return anjay_ret_i32(ctx, 0);  /* Not implemented */

    case RID_SMS_RX:
        return anjay_ret_i32(ctx, 0);  /* Not implemented */

    case RID_TX_DATA:
        /* Convert bytes to KB */
        return anjay_ret_i32(ctx, (int32_t)(stat_tx_bytes));

    case RID_RX_DATA:
        /* Convert bytes to KB */
        return anjay_ret_i32(ctx, (int32_t)(stat_rx_bytes));

    case RID_MAX_MSG_SIZE:
        return anjay_ret_i32(ctx, (int32_t)stat_max_msg_size);

    case RID_AVG_MSG_SIZE: {
        ULONG total_packets = stat_tx_packets + stat_rx_packets;
        ULONG total_bytes = stat_tx_bytes + stat_rx_bytes;
        if (total_packets > 0)
            return anjay_ret_i32(ctx, (int32_t)(total_bytes / total_packets));
        return anjay_ret_i32(ctx, 0);
    }

    case RID_COLLECTION_PERIOD:
        return anjay_ret_i32(ctx, (int32_t)collection_period);

    default:
        return ANJAY_ERR_METHOD_NOT_ALLOWED;
    }
}

static int resource_write(anjay_t *anjay,
                          const anjay_dm_object_def_t *const *obj_ptr,
                          anjay_iid_t iid,
                          anjay_rid_t rid,
                          anjay_riid_t riid,
                          anjay_input_ctx_t *ctx) {
    (void) anjay;
    (void) obj_ptr;
    assert(iid == 0);

    switch (rid) {
    case RID_COLLECTION_PERIOD: {
        assert(riid == ANJAY_ID_INVALID);
        int32_t period;
        int result = anjay_get_i32(ctx, &period);
        if (result) return result;
        if (period < 0) return ANJAY_ERR_BAD_REQUEST;
        collection_period = (ULONG)period;
        elog_i(TAG, "Collection period set to %lu s", collection_period);
        return 0;
    }

    default:
        return ANJAY_ERR_METHOD_NOT_ALLOWED;
    }
}

static int resource_execute(anjay_t *anjay,
                            const anjay_dm_object_def_t *const *obj_ptr,
                            anjay_iid_t iid,
                            anjay_rid_t rid,
                            anjay_execute_ctx_t *arg_ctx) {
    (void) arg_ctx;
    (void) obj_ptr;
    assert(iid == 0);

    switch (rid) {
    case RID_START_RESET:
        /* Reset counters and start collection */
        get_raw_stats(&base_tx_bytes, &base_rx_bytes,
                      &base_tx_packets, &base_rx_packets);
        stat_tx_bytes = 0;
        stat_rx_bytes = 0;
        stat_tx_packets = 0;
        stat_rx_packets = 0;
        stat_max_msg_size = 0;
        collecting = 1;
        elog_i(TAG, "Statistics reset and collection started");
        return 0;

    case RID_STOP:
        /* Stop collection, keep counters */
        collecting = 0;
        elog_i(TAG, "Statistics collection stopped");
        return 0;

    default:
        return ANJAY_ERR_METHOD_NOT_ALLOWED;
    }
}

static const anjay_dm_object_def_t OBJ_DEF = {
    .oid = 7,
    .handlers = {
        .list_instances = list_instances,
        .list_resources = list_resources,
        .resource_read = resource_read,
        .resource_write = resource_write,
        .resource_execute = resource_execute
    }
};

static conn_stats_object_t CONN_STATS_OBJECT = {
    .def = &OBJ_DEF
};

int conn_stats_object_install(anjay_t *anjay) {
    /* Take initial snapshot */
    get_raw_stats(&base_tx_bytes, &base_rx_bytes,
                  &base_tx_packets, &base_rx_packets);
    collecting = 1;
    elog_i(TAG, "Installed Object /7 (Connectivity Statistics)");
    return anjay_register_object(anjay, &CONN_STATS_OBJECT.def);
}
