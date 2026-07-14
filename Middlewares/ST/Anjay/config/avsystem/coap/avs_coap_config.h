/*
 * Copyright 2017-2026 AVSystem <avsystem@avsystem.com>
 * AVSystem CoAP library
 * All rights reserved.
 *
 * Licensed under AVSystem Anjay LwM2M Client SDK - Non-Commercial License.
 * See the attached LICENSE file for details.
 */

#ifndef AVS_COAP_CONFIG_H
#define AVS_COAP_CONFIG_H

/*
 * avs_coap library configuration for STM32L4 + NetXDuo
 * CoAP-only mode (UDP transport, no TCP)
 */

/* Enable support for block-wise transfers (RFC 7959) */
#define WITH_AVS_COAP_BLOCK

/* Enable support for observations (RFC 7641) */
#define WITH_AVS_COAP_OBSERVE

/* Force cancelling observation on unacked error */
#define WITH_AVS_COAP_OBSERVE_FORCE_CANCEL_ON_UNACKED_ERROR

/* Enable support for observation persistence */
#define WITH_AVS_COAP_OBSERVE_PERSISTENCE

/* Enable support for the streaming API */
#define WITH_AVS_COAP_STREAMING_API

/* Enable support for UDP transport */
#define WITH_AVS_COAP_UDP

/* Disable TCP transport (use UDP/CoAP only) */
/* #define WITH_AVS_COAP_TCP */

/* Disable OSCORE */
/* #define WITH_AVS_COAP_OSCORE */

/* UDP notification cache size */
#define AVS_COAP_UDP_NOTIFY_CACHE_SIZE 4

/* Enable diagnostic messages in error responses */
#define WITH_AVS_COAP_DIAGNOSTIC_MESSAGES

/* Enable logging */
#define WITH_AVS_COAP_LOGS

/* Enable trace logs for debugging Bootstrap Request */
#define WITH_AVS_COAP_TRACE_LOGS

/* Disable poisoning (not needed for production) */
/* #define WITH_AVS_COAP_POISONING */

#endif /* AVS_COAP_CONFIG_H */
