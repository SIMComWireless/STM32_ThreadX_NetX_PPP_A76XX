/**
 ******************************************************************************
 * @file    app_anjay.h
 * @brief   Anjay LwM2M Client integration header
 ******************************************************************************
 */

#ifndef APP_ANJAY_H
#define APP_ANJAY_H

#include "nx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Public Functions ===== */

/**
 * @brief  Initialize Anjay LwM2M Client
 * @note   Call this after NetXDuo and PPP are initialized
 *         Creates a thread that waits for PPP link-up,
 *         then connects to Bootstrap Server via CoAP/UDP
 */
void MX_Anjay_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_ANJAY_H */
