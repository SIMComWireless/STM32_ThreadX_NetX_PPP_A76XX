/*
 * LwM2M Device Object (/3) for Anjay
 */

#ifndef APP_ANJAY_DEVICE_OBJECT_H
#define APP_ANJAY_DEVICE_OBJECT_H

#include <anjay/anjay.h>

/**
 * Register Device object (/3) with Anjay.
 * Reads IMEI from modem via AT+GSN (fallback: STM32 UID).
 * @return 0 on success, negative on error.
 */
int device_object_install(anjay_t *anjay);

/**
 * Get the IMEI string read during install.
 * @return Pointer to 15-digit IMEI string, or empty string if not available.
 */
const char *device_object_get_imei(void);

/**
 * Call periodically from the Anjay event loop.
 * Handles deferred reboot after execute command.
 */
void device_object_update(anjay_t *anjay);

#endif /* APP_ANJAY_DEVICE_OBJECT_H */
