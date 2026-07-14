/**
 ******************************************************************************
 * @file    app_anjay.c
 * @brief   Anjay LwM2M Client integration for NetXDuo + ThreadX
 ******************************************************************************
 * @note
 *   Flow:
 *   1. MX_Anjay_Init() creates Anjay thread
 *   2. Anjay thread waits for PPP link-up
 *   3. Configures Bootstrap or Direct LwM2M Server (DTLS PSK)
 *   4. Runs Anjay event loop
 ******************************************************************************
 */

#include "app_netxduo.h"
#include "nx_api.h"
#include "nxd_dns.h"
#include "elog.h"
#include <string.h>
#include <stdlib.h>

/* Anjay headers */
#include <anjay/anjay.h>
#include <anjay/core.h>
#include <anjay/security.h>
#include <anjay/server.h>

/* Standalone objects */
#include <standalone_security.h>
#include <standalone_server.h>
#include "app_anjay_device_object.h"

/* ===== avs_log handler — redirect to elog ===== */
#include <avsystem/commons/avs_log.h>

/* ===== Thread Configuration ===== */
#define ANJAY_THREAD_PRIO       25
#define ANJAY_THREAD_STACK      (1024 * 30)

/* ===== Connection Mode ===== */
/* 1 = Bootstrap (credentials provisioned by Bootstrap Server) */
/* 0 = Direct LwM2M Server (credentials configured below)     */
#ifndef ANJAY_USE_BOOTSTRAP
#define ANJAY_USE_BOOTSTRAP     1
#endif

/* ===== Bootstrap Server Configuration ===== */
/* AVSystem test Bootstrap Server (PSK) */
#ifndef BOOTSTRAP_SERVER_URI
#define BOOTSTRAP_SERVER_URI    "coaps://lwm2m-test.avsystem.io:5694"
#endif

#ifndef BOOTSTRAP_PSK_IDENTITY
#define BOOTSTRAP_PSK_IDENTITY  "862095060018816"
#endif

#ifndef BOOTSTRAP_PSK_KEY
#define BOOTSTRAP_PSK_KEY       "123456789"
#endif

#ifndef ANJAY_USE_BOOTSTRAP
/* ===== LwM2M Server Configuration ===== */
/* Used only when ANJAY_USE_BOOTSTRAP=0 (direct connection) */
#ifndef LWM2M_SERVER_URI
#define LWM2M_SERVER_URI        "coaps://lwm2m-test.avsystem.io:5684"
#endif

#ifndef LWM2M_PSK_IDENTITY
#define LWM2M_PSK_IDENTITY      "XIOT_Client01"
#endif

#ifndef LWM2M_PSK_KEY
#define LWM2M_PSK_KEY           "g9p8RkSKaayNWxN9"
#endif

#endif /* ANJAY_USE_BOOTSTRAP */

/* Endpoint name - must be unique per device */
#ifndef ANJAY_ENDPOINT_NAME
#define ANJAY_ENDPOINT_NAME    "XIOT_Client01"
#endif

/* ===== External References ===== */
extern TX_EVENT_FLAGS_GROUP ppp_events;
extern TX_BYTE_POOL       *byte_pool_ptr;

/* ===== Static Variables ===== */
static TX_THREAD anjay_thread;
static VOID     *anjay_stack_ptr;

/* Socket registry accessor — native NetX event loop uses this */
extern avs_net_socket_t **avs_net_get_sockets(int *count);

static void anjay_log_handler(avs_log_level_t level,
                              const char *module,
                              const char *message) {
    switch (level) {
    case AVS_LOG_TRACE:
    case AVS_LOG_DEBUG:
        elog_i("AVS TRACE", "[%s] %s", module, message);
        break;
    case AVS_LOG_INFO:
        elog_i("AVS INFO", "[%s] %s", module, message);
        break;
    case AVS_LOG_WARNING:
        elog_w("AVS WARNING", "[%s] %s", module, message);
        break;
    case AVS_LOG_ERROR:
    case AVS_LOG_QUIET:
    default:
        elog_e("AVS ERROR", "[%s] %s", module, message);
        break;
    }
}

/* ===== Anjay Thread Entry ===== */
static void anjay_thread_entry(ULONG input) {
    anjay_t *anjay = NULL;
    const anjay_dm_object_def_t **security_obj = NULL;
    const anjay_dm_object_def_t **server_obj = NULL;
    int result;

    /* Install avs_log handler so we can see internal errors */
    avs_log_set_handler(anjay_log_handler);
    avs_log_set_default_level(AVS_LOG_TRACE);

    elog_i("ANJAY", "Waiting for NTP sync (implies PPP link-up)...");

    /* Wait for NTP sync — PPP link-up is a prerequisite for NTP,
     * so this implicitly waits for both. */
    ULONG ppp_flags;
    tx_event_flags_get(&ppp_events, PPP_EVT_NTP_DONE, TX_OR_CLEAR,
                       &ppp_flags, TX_WAIT_FOREVER);

    elog_i("ANJAY", "NTP synced, starting Anjay...");

    /* ===== 1. Configure Anjay ===== */
    anjay_configuration_t config = {
        .endpoint_name = ANJAY_ENDPOINT_NAME,
        .confirmable_notifications = false,
        .in_buffer_size  = 1500,   /* CoAP receive buffer — must be > 0 */
        .out_buffer_size = 1500,   /* CoAP send buffer — must be > 0 */
#ifndef IP_MTU
        .socket_config = {
            .forced_mtu = 1400  /* Safe margin below PPP MRU (1480) */
        },
#endif
    };
    elog_i("ANJAY", "Config: in_buf=%d, out_buf=%d, forced_mtu=%d",
           config.in_buffer_size, config.out_buffer_size,
           config.socket_config.forced_mtu);

    /* ===== 2. Create Anjay Instance ===== */
    anjay = anjay_new(&config);
    if (!anjay) {
        elog_e("ANJAY", "Failed to create Anjay instance");
        goto cleanup;
    }

    elog_i("ANJAY", "Anjay instance created");

    /* ===== 3. Install Security Object ===== */
    security_obj = standalone_security_object_install(anjay);
    if (!security_obj) {
        elog_e("ANJAY", "Failed to install Security object");
        goto cleanup;
    }

    elog_i("ANJAY", "Security object installed");

    /* ===== 4. Install Server Object ===== */
    server_obj = standalone_server_object_install(anjay);
    if (!server_obj) {
        elog_e("ANJAY", "Failed to install Server object");
        goto cleanup;
    }

    elog_i("ANJAY", "Server object installed");

    /* ===== 4b. Install Device Object (/3) ===== */
    if (device_object_install(anjay)) {
        elog_e("ANJAY", "Failed to install Device Object (/3)");
        goto cleanup;
    }

    elog_i("ANJAY", "Device object (/3) installed");

    /* ===== 5. Configure Security ===== */
    elog_i("ANJAY", "Endpoint: %s", ANJAY_ENDPOINT_NAME);

#if ANJAY_USE_BOOTSTRAP
    standalone_security_instance_t bootstrap_security = {
        .ssid = ANJAY_SSID_BOOTSTRAP,
        .server_uri = BOOTSTRAP_SERVER_URI,
        .bootstrap_server = true,
        .security_mode = ANJAY_SECURITY_PSK,
        .client_holdoff_s = 0,
        .bootstrap_timeout_s = 0,
        .public_cert_or_psk_identity =
                (const uint8_t *)BOOTSTRAP_PSK_IDENTITY,
        .public_cert_or_psk_identity_size = strlen(BOOTSTRAP_PSK_IDENTITY),
        .private_cert_or_psk_key =
                (const uint8_t *)BOOTSTRAP_PSK_KEY,
        .private_cert_or_psk_key_size = strlen(BOOTSTRAP_PSK_KEY),
        .server_public_key = NULL,
        .server_public_key_size = 0,
    };

    anjay_iid_t bootstrap_iid = ANJAY_ID_INVALID;
    result = standalone_security_object_add_instance(security_obj,
                                                      &bootstrap_security,
                                                      &bootstrap_iid);
    if (result) {
        elog_e("ANJAY", "Failed to add Bootstrap Security instance: %d", result);
        goto cleanup;
    }

    elog_i("ANJAY", "Bootstrap Security instance added (IID=%d)", bootstrap_iid);
    elog_i("ANJAY", "Bootstrap URI: %s (PSK)", BOOTSTRAP_SERVER_URI);
#else /* Direct LwM2M Server connection (PSK) */
    /* Security instance for LwM2M Server (PSK DTLS) */
    standalone_security_instance_t server_security = {
        .ssid = 1,  /* Short Server ID */
        .server_uri = LWM2M_SERVER_URI,
        .bootstrap_server = false,
        .security_mode = ANJAY_SECURITY_PSK,
        .public_cert_or_psk_identity =
                (const uint8_t *)LWM2M_PSK_IDENTITY,
        .public_cert_or_psk_identity_size = strlen(LWM2M_PSK_IDENTITY),
        .private_cert_or_psk_key =
                (const uint8_t *)LWM2M_PSK_KEY,
        .private_cert_or_psk_key_size = strlen(LWM2M_PSK_KEY),
        .server_public_key = NULL,
        .server_public_key_size = 0,
    };

    anjay_iid_t server_sec_iid = ANJAY_ID_INVALID;
    result = standalone_security_object_add_instance(security_obj,
                                                      &server_security,
                                                      &server_sec_iid);
    if (result) {
        elog_e("ANJAY", "Failed to add Server Security instance: %d", result);
        goto cleanup;
    }

    /* Server object instance */
    standalone_server_instance_t server_instance = {
        .ssid = 1,
        .lifetime = 300,         /* Registration lifetime: 300 seconds */
        .default_min_period = 1, /* Min notification period */
        .default_max_period = 60,
        .disable_timeout = 86400,
        .binding = "U",          /* UDP binding */
    };

    anjay_iid_t server_iid = ANJAY_ID_INVALID;
    result = standalone_server_object_add_instance(server_obj,
                                                    &server_instance,
                                                    &server_iid);
    if (result) {
        elog_e("ANJAY", "Failed to add Server instance: %d", result);
        goto cleanup;
    }

    elog_i("ANJAY", "Direct LwM2M Server configured (IID=%d)", server_sec_iid);
    elog_i("ANJAY", "Server URI: %s (PSK)", LWM2M_SERVER_URI);
    elog_i("ANJAY", "PSK Identity: %s", LWM2M_PSK_IDENTITY);
#endif /* ANJAY_USE_BOOTSTRAP */

    /* ===== 6. Run Anjay Event Loop ===== */
    /* Uses standard anjay_event_loop_run(). The NetX Duo port provides:
     * - get_system_socket() → returns &sock->self (valid pointer for poll)
     * - netxduo_poll() → calls nx_udp_socket_receive(), buffers data
     * - receive() → returns poll-buffered data to avs_coap
     *
     * This follows the same pattern as the SIMCom reference implementation
     * (xcc_com_sockets_net_impl.c). */
    elog_i("ANJAY", "Entering event loop...");

    const avs_time_duration_t loop_timeout =
            avs_time_duration_from_scalar(1, AVS_TIME_S);

    while (1) {
        (void) anjay_event_loop_run(anjay, loop_timeout);
        device_object_update(anjay);
    }

cleanup:
    if (anjay) {
        anjay_delete(anjay);
    }
    elog_e("ANJAY", "Anjay thread exited");
}

/* ===== Public Functions ===== */

/**
 * @brief  Initialize Anjay LwM2M Client
 * @note   Call this after NetXDuo and PPP are initialized
 */
void MX_Anjay_Init(void) {
    UINT status;

    /* Allocate Anjay thread stack */
    status = tx_byte_allocate(byte_pool_ptr, &anjay_stack_ptr,
                               ANJAY_THREAD_STACK, TX_NO_WAIT);
    if (status != TX_SUCCESS) {
        elog_e("ANJAY", "Failed to allocate Anjay thread stack: 0x%02X", status);
        return;
    }

    /* Create Anjay thread */
    status = tx_thread_create(&anjay_thread, "Anjay", anjay_thread_entry, 0,
                               anjay_stack_ptr, ANJAY_THREAD_STACK,
                               ANJAY_THREAD_PRIO, ANJAY_THREAD_PRIO,
                               TX_NO_TIME_SLICE, TX_AUTO_START);
    if (status != TX_SUCCESS) {
        elog_e("ANJAY", "Failed to create Anjay thread: 0x%02X", status);
        tx_byte_release(anjay_stack_ptr);
        return;
    }

    elog_i("ANJAY", "Anjay thread created (priority=%d, stack=%d)",
          ANJAY_THREAD_PRIO, ANJAY_THREAD_STACK);
}

