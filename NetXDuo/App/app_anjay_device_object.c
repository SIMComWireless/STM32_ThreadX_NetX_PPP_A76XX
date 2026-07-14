/*
 * LwM2M Device Object (/3) implementation for Anjay
 * Adapted from AVSystem Anjay-stm32-azurertos-client
 *
 * Copyright 2023-2025 AVSystem <avsystem@avsystem.com>
 * Licensed under the Apache License, Version 2.0
 */

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include <anjay/anjay.h>
#include <avsystem/commons/avs_defs.h>

#include "stm32l4xx_hal.h"
#include "a7683e.h"
#include "elog.h"
#include "app_anjay_device_object.h"

/* LwM2M Device Object Resource IDs */
#define RID_MANUFACTURER                  0
#define RID_MODEL_NUMBER                  1
#define RID_SERIAL_NUMBER                 2
#define RID_FIRMWARE_VERSION              3
#define RID_REBOOT                        4
#define RID_ERROR_CODE                   11
#define RID_SUPPORTED_BINDING_AND_MODES  16
#define RID_SOFTWARE_VERSION             19

/* Device info — customize as needed */
#define DEVICE_MANUFACTURER     "SIMCom"
#define DEVICE_MODEL_NUMBER     "A7683E"
#define DEVICE_FIRMWARE_VERSION "1.0.0"

typedef struct device_object_struct {
    const anjay_dm_object_def_t *def;
    bool reboot;
} device_object_t;

static inline device_object_t *
get_obj(const anjay_dm_object_def_t *const *obj_ptr) {
    assert(obj_ptr);
    return AVS_CONTAINER_OF(obj_ptr, device_object_t, def);
}

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

    anjay_dm_emit_res(ctx, RID_MANUFACTURER, ANJAY_DM_RES_R,
                      ANJAY_DM_RES_PRESENT);
    anjay_dm_emit_res(ctx, RID_MODEL_NUMBER, ANJAY_DM_RES_R,
                      ANJAY_DM_RES_PRESENT);
    anjay_dm_emit_res(ctx, RID_SERIAL_NUMBER, ANJAY_DM_RES_R,
                      ANJAY_DM_RES_PRESENT);
    anjay_dm_emit_res(ctx, RID_FIRMWARE_VERSION, ANJAY_DM_RES_R,
                      ANJAY_DM_RES_PRESENT);
    anjay_dm_emit_res(ctx, RID_REBOOT, ANJAY_DM_RES_E, ANJAY_DM_RES_PRESENT);
    anjay_dm_emit_res(ctx, RID_ERROR_CODE, ANJAY_DM_RES_RM,
                      ANJAY_DM_RES_PRESENT);
    anjay_dm_emit_res(ctx, RID_SUPPORTED_BINDING_AND_MODES, ANJAY_DM_RES_R,
                      ANJAY_DM_RES_PRESENT);
    anjay_dm_emit_res(ctx, RID_SOFTWARE_VERSION, ANJAY_DM_RES_R,
                      ANJAY_DM_RES_PRESENT);
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

    switch (rid) {
    case RID_MANUFACTURER:
        assert(riid == ANJAY_ID_INVALID);
        return anjay_ret_string(ctx, DEVICE_MANUFACTURER);

    case RID_MODEL_NUMBER:
        assert(riid == ANJAY_ID_INVALID);
        return anjay_ret_string(ctx, DEVICE_MODEL_NUMBER);

    case RID_SERIAL_NUMBER:
        assert(riid == ANJAY_ID_INVALID);
        return anjay_ret_string(ctx, a7683e_get_imei());

    case RID_FIRMWARE_VERSION:
        assert(riid == ANJAY_ID_INVALID);
        return anjay_ret_string(ctx, DEVICE_FIRMWARE_VERSION);

    case RID_ERROR_CODE:
        assert(riid == 0);
        return anjay_ret_i32(ctx, 0);

    case RID_SUPPORTED_BINDING_AND_MODES:
        assert(riid == ANJAY_ID_INVALID);
        return anjay_ret_string(ctx, "U");

    case RID_SOFTWARE_VERSION:
        assert(riid == ANJAY_ID_INVALID);
        return anjay_ret_string(ctx, DEVICE_FIRMWARE_VERSION);

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

    device_object_t *obj = get_obj(obj_ptr);
    assert(obj);
    assert(iid == 0);

    switch (rid) {
    case RID_REBOOT:
        obj->reboot = true;
        return 0;

    default:
        return ANJAY_ERR_METHOD_NOT_ALLOWED;
    }
}

static int list_resource_instances(anjay_t *anjay,
                                   const anjay_dm_object_def_t *const *obj_ptr,
                                   anjay_iid_t iid,
                                   anjay_rid_t rid,
                                   anjay_dm_list_ctx_t *ctx) {
    (void) anjay;
    (void) obj_ptr;
    assert(iid == 0);

    switch (rid) {
    case RID_ERROR_CODE:
        anjay_dm_emit(ctx, 0);
        return 0;

    default:
        return ANJAY_ERR_METHOD_NOT_ALLOWED;
    }
}

static const anjay_dm_object_def_t OBJ_DEF = {
    .oid = 3,
    .handlers = {
        .list_instances = list_instances,
        .list_resources = list_resources,
        .resource_read = resource_read,
        .resource_execute = resource_execute,
        .list_resource_instances = list_resource_instances
    }
};

static device_object_t DEVICE_OBJECT = {
    .def = &OBJ_DEF
};

int device_object_install(anjay_t *anjay) {
    /* IMEI was already read during a7683e_init() — just verify it's available */
    const char *imei = a7683e_get_imei();
    if (imei[0] == '\0') {
        elog_w("DEVICE", "IMEI not available — Serial Number will be empty");
    }
    return anjay_register_object(anjay, &DEVICE_OBJECT.def);
}

void device_object_update(anjay_t *anjay) {
    (void) anjay;

    if (DEVICE_OBJECT.reboot) {
        /* Small delay to let Anjay send response */
        HAL_Delay(100);
        elog_e("DEVICE", "Rebooting device as requested by LwM2M Server...");
        HAL_NVIC_SystemReset();
    }
}
