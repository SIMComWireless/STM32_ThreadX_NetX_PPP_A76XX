/**
 ******************************************************************************
 * @file    avs_port_netxduo.c
 * @brief   avs_commons porting layer for ThreadX + NetXDuo (BSD socket mode)
 ******************************************************************************
 * @note
 *   Provides platform-specific implementations required by avs_commons:
 *   - Threading: mutex (ThreadX)
 *   - Time: monotonic and real clocks (ThreadX ticks)
 *   - Init once: thread-safe one-time initialization
 *   - mbedTLS timing: delay context for DTLS retransmission
 *   - Entropy: PRNG seed from ThreadX tick counter
 *   - Standard C: time(), assert stub
 *
 *   Socket operations are provided by native NetX Duo UDP API
 *   (see avs_net_impl_netxduo.c).
 ******************************************************************************
 */

/* ===== avs_commons headers ===== */
#include <avsystem/commons/avs_commons_config.h>
#include <avsystem/commons/avs_time.h>
#include <avsystem/commons/avs_mutex.h>
#include <avsystem/commons/avs_init_once.h>
#include <avsystem/commons/avs_memory.h>
#include <avsystem/commons/avs_errno.h>
#include <avsystem/commons/avs_net.h>
#include <avsystem/commons/avs_addrinfo.h>

/* ===== RTOS headers ===== */
#include "tx_api.h"

/* ===== Standard C headers ===== */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>

/* ===== Application headers ===== */
#include "elog.h"
#include "rtc.h"

/* ============================================================================
 * Section 1: Mutex implementation (ThreadX)
 * ============================================================================
 *
 * Anjay uses avs_mutex_t for thread-safe access to internal state.
 * We implement it using ThreadX mutexes with priority inheritance.
 * ============================================================================ */

struct avs_mutex {
    TX_MUTEX mutex;
    char     name[16];
};

int avs_mutex_create(avs_mutex_t **out_mutex) {
    if (!out_mutex) {
        return -1;
    }

    *out_mutex = avs_calloc(1, sizeof(avs_mutex_t));
    if (!*out_mutex) {
        return -1;
    }

    static uint32_t mutex_count = 0;
    snprintf((*out_mutex)->name, sizeof((*out_mutex)->name),
             "avs%u", (unsigned)mutex_count++);

    UINT status = tx_mutex_create(&(*out_mutex)->mutex,
                                  (*out_mutex)->name,
                                  TX_NO_INHERIT);
    if (status != TX_SUCCESS) {
        avs_free(*out_mutex);
        *out_mutex = NULL;
        return -1;
    }

    return 0;
}

int avs_mutex_lock(avs_mutex_t *mutex) {
    if (!mutex) {
        return -1;
    }
    UINT status = tx_mutex_get(&mutex->mutex, TX_WAIT_FOREVER);
    return (status == TX_SUCCESS) ? 0 : -1;
}

int avs_mutex_unlock(avs_mutex_t *mutex) {
    if (!mutex) {
        return -1;
    }
    UINT status = tx_mutex_put(&mutex->mutex);
    return (status == TX_SUCCESS) ? 0 : -1;
}

void avs_mutex_cleanup(avs_mutex_t **mutex) {
    if (mutex && *mutex) {
        tx_mutex_delete(&(*mutex)->mutex);
        avs_free(*mutex);
        *mutex = NULL;
    }
}

/* ============================================================================
 * Section 2: Init Once implementation
 * ============================================================================
 *
 * Thread-safe one-time initialization using double-checked locking.
 * The handle is set to a non-NULL sentinel after successful init.
 * ============================================================================ */

static TX_MUTEX init_once_mutex;
static volatile int init_once_mutex_created = 0;

static void ensure_init_once_mutex(void) {
    if (!init_once_mutex_created) {
        tx_mutex_create(&init_once_mutex, "init_once", TX_NO_INHERIT);
        init_once_mutex_created = 1;
    }
}

int avs_init_once(volatile avs_init_once_handle_t *handle,
                  avs_init_once_func_t *func,
                  void *func_arg) {
    if (!handle || !func) {
        return -1;
    }

    /* Fast path: already initialized */
    if (*handle != NULL) {
        return 0;
    }

    /* Slow path: serialize with mutex */
    ensure_init_once_mutex();
    tx_mutex_get(&init_once_mutex, TX_WAIT_FOREVER);

    /* Double-check after acquiring mutex */
    if (*handle != NULL) {
        tx_mutex_put(&init_once_mutex);
        return 0;
    }

    int result = func(func_arg);
    if (result == 0) {
        /* Set handle to non-NULL sentinel */
        *handle = (avs_init_once_handle_t)(uintptr_t)1;
    }

    tx_mutex_put(&init_once_mutex);
    return result;
}

/* ============================================================================
 * Section 3: Time implementation
 * ============================================================================
 *
 * Anjay uses avs_time_real_t and avs_time_monotonic_t for:
 * - CoAP token generation and deduplication
 * - Timeout calculations
 * - Scheduling
 *
 * We use ThreadX tick counter (tx_time_get()) as the time source.
 * The absolute value doesn't matter — only monotonicity and uniqueness.
 * ============================================================================ */

avs_time_real_t avs_time_real_now(void) {
    /* Use RTC for real wall-clock time (synced via NTP).
     * rtc_time_cache is updated every second by RTC IRQ. */
    volatile rtc_cached_time_t *rtc = &rtc_time_cache;

    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_sec   = rtc->seconds;
    t.tm_min   = rtc->minutes;
    t.tm_hour  = rtc->hours;
    t.tm_mday  = rtc->date;
    t.tm_mon   = rtc->month - 1;   /* RTC: 1-12, struct tm: 0-11 */
    t.tm_year  = rtc->year + 100;   /* RTC: year-2000, struct tm: year-1900 */
    t.tm_isdst = -1;

    time_t unix_time = mktime(&t);

    avs_time_real_t result;
    result.since_real_epoch.seconds     = (int64_t) unix_time;
    result.since_real_epoch.nanoseconds = 0;  /* RTC has no sub-second precision */
    return result;
}

avs_time_monotonic_t avs_time_monotonic_now(void) {
    ULONG ticks = tx_time_get();
    ULONG tps   = TX_TIMER_TICKS_PER_SECOND;

    avs_time_monotonic_t result;
    result.since_monotonic_epoch.seconds     = (int64_t)(ticks / tps);
    result.since_monotonic_epoch.nanoseconds =
            (int32_t)((ticks % tps) * (1000000000UL / tps));
    return result;
}

/* ============================================================================
 * Section 4: mbedTLS timing
 * ============================================================================
 *
 * Used by mbedTLS for DTLS retransmission timing.
 * ============================================================================ */

typedef struct {
    unsigned long snap_ms;
    unsigned long int_ms;
    unsigned long fin_ms;
} mbedtls_timing_delay_context;

void mbedtls_timing_set_delay(void *data, uint32_t int_ms, uint32_t fin_ms) {
    mbedtls_timing_delay_context *ctx = (mbedtls_timing_delay_context *)data;
    if (!ctx) return;

    ctx->int_ms  = int_ms;
    ctx->fin_ms  = fin_ms;
    ctx->snap_ms = tx_time_get() * 1000 / TX_TIMER_TICKS_PER_SECOND;
}

int mbedtls_timing_get_delay(void *data) {
    mbedtls_timing_delay_context *ctx = (mbedtls_timing_delay_context *)data;
    if (!ctx) return -1;

    unsigned long now_ms     = tx_time_get() * 1000 / TX_TIMER_TICKS_PER_SECOND;
    unsigned long elapsed_ms = now_ms - ctx->snap_ms;

    if (ctx->fin_ms == 0) {
        return -1;
    }

    if (elapsed_ms >= ctx->fin_ms) {
        return 2;  /* Final delay expired */
    }

    if (elapsed_ms >= ctx->int_ms) {
        return 1;  /* Intermediate delay expired */
    }

    return 0;  /* No delay expired */
}

/* ============================================================================
 * Section 5: mbedTLS entropy
 * ============================================================================
 *
 * Provides pseudo-random bytes for mbedTLS. Not cryptographically secure,
 * but sufficient for CoAP/DTLS operation on embedded platforms.
 * ============================================================================ */

int mbedtls_platform_entropy_poll(void *data, unsigned char *output,
                                  size_t len, size_t *olen) {
    (void)data;

    uint32_t seed = tx_time_get();

    for (size_t i = 0; i < len; i++) {
        seed = seed * 1103515245 + 12345;  /* LCG */
        output[i] = (unsigned char)(seed >> 16);
    }

    *olen = len;
    return 0;
}

int mbedtls_hardclock_poll(void *data, unsigned char *output,
                           size_t len, size_t *olen) {
    return mbedtls_platform_entropy_poll(data, output, len, olen);
}

/* ============================================================================
 * Section 6: Standard C: time()
 * ============================================================================ */

time_t time(time_t *timer) {
    ULONG ticks = tx_time_get();
    ULONG tps   = TX_TIMER_TICKS_PER_SECOND;
    time_t t    = (time_t)(ticks / tps);

    if (timer) {
        *timer = t;
    }

    return t;
}

/* ============================================================================
 * Section 7: ARM assert stub
 * ============================================================================ */

void __aeabi_assert(const char *expr, const char *file, int line) {
    elog_e("ASSERT", "Assertion failed: %s, file %s, line %d",
           expr, file, line);
    while (1) {
        /* Halt in debug */
    }
}

/* ============================================================================
 * Section 8: errno support for NetX Duo BSD
 * ============================================================================
 *
 * NetX Duo BSD stores errno per-thread via bsd_errno field in TX_THREAD.
 * We provide __errno() that returns a pointer to the current thread's
 * bsd_errno field.
 * ============================================================================ */

int *__errno(void) {
    TX_THREAD *current = tx_thread_identify();
    if (current) {
        return &current->bsd_errno;
    }
    /* Fallback: should not happen in normal operation */
    static int fallback_errno = 0;
    return &fallback_errno;
}

/* ============================================================================
 * Section 9: Network global state
 * ============================================================================
 *
 * Required by avs_net_global.c. Called during avs_commons init/cleanup.
 * ============================================================================ */

avs_error_t _avs_net_initialize_global_compat_state(void) {
    return AVS_OK;
}

void _avs_net_cleanup_global_compat_state(void) {
    /* Nothing to cleanup */
}

/* ============================================================================
 * Section 10: DNS addrinfo (IPv4 only, using native NetX Duo DNS)
 * ============================================================================
 *
 * Provides avs_net_addrinfo_* functions for Anjay's DNS resolution.
 * Uses native nx_dns_host_by_name_get() for hostname resolution.
 * ============================================================================ */

#include "nxd_dns.h"

/* Access the global DNS client from app_netxduo.c */
extern NX_DNS       dns_client;
extern volatile UINT dns_client_initialized;

/* Endpoint format: struct sockaddr_in (16 bytes) for Anjay compatibility */
struct sockaddr_in_min {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    char     sin_zero[8];
};

#define ENDPOINT_SIZE sizeof(struct sockaddr_in_min)

static uint16_t avs_port_htons(uint16_t val) {
    return (uint16_t) ((val >> 8) | (val << 8));
}

static int parse_ip_str(const char *str, ULONG *out_ip) {
    ULONG parts[4] = {0};
    int part = 0;
    const char *p = str;

    while (part < 4) {
        ULONG val = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (ULONG) (*p - '0');
            p++;
            digits++;
        }
        if (digits == 0 || val > 255) return -1;
        parts[part++] = val;
        if (*p == '.') p++;
        else if (*p == '\0' && part == 4) break;
        else return -1;
    }
    if (part != 4 || *p != '\0') return -1;
    *out_ip = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 0;
}

struct avs_net_addrinfo_struct {
    avs_net_resolved_endpoint_t results[4];
    int count;
    int index;
};

avs_net_addrinfo_t *avs_net_addrinfo_resolve_ex(
        avs_net_socket_type_t socket_type,
        avs_net_af_t family,
        const char *host,
        const char *port_str,
        int flags,
        const avs_net_resolved_endpoint_t *preferred_endpoint) {
    (void) socket_type;
    (void) flags;
    (void) preferred_endpoint;

    if (!host || !*host) return NULL;
    if (family != AVS_NET_AF_INET4 && family != AVS_NET_AF_UNSPEC) return NULL;

    uint16_t port = 0;
    if (port_str && *port_str) {
        port = (uint16_t) atoi(port_str);
    }

    avs_net_addrinfo_t *ctx =
            (avs_net_addrinfo_t *) avs_calloc(1, sizeof(avs_net_addrinfo_t));
    if (!ctx) return NULL;

    ULONG ip_addr;
    if (parse_ip_str(host, &ip_addr) != 0) {
        /* DNS resolve via native nx_dns_host_by_name_get() */
        if (!dns_client_initialized) {
            avs_free(ctx);
            return NULL;
        }
        ULONG resolved;
        UINT status = nx_dns_host_by_name_get(&dns_client, (UCHAR *)host,
                                               &resolved,
                                               5 * NX_IP_PERIODIC_RATE);
        if (status != NX_SUCCESS) {
            avs_free(ctx);
            return NULL;
        }
        ip_addr = resolved;  /* nx_dns_host_by_name_get returns host byte order */
    }

    /* Store as struct sockaddr_in */
    struct sockaddr_in_min *addr =
            (struct sockaddr_in_min *) &ctx->results[0].data.buf;
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = 2; /* AF_INET */
    addr->sin_port   = avs_port_htons(port);
    addr->sin_addr   = ip_addr;
    ctx->results[0].size = (uint8_t) sizeof(*addr);
    ctx->count = 1;
    ctx->index = 0;

    return ctx;
}

int avs_net_addrinfo_next(avs_net_addrinfo_t *ctx,
                          avs_net_resolved_endpoint_t *out) {
    if (!ctx || ctx->index >= ctx->count) return AVS_NET_ADDRINFO_END;
    *out = ctx->results[ctx->index++];
    return 0;
}

void avs_net_addrinfo_rewind(avs_net_addrinfo_t *ctx) {
    if (ctx) ctx->index = 0;
}

void avs_net_addrinfo_delete(avs_net_addrinfo_t **ctx) {
    if (ctx && *ctx) {
        avs_free(*ctx);
        *ctx = NULL;
    }
}

avs_error_t avs_net_resolved_endpoint_get_host_port(
        const avs_net_resolved_endpoint_t *endpoint,
        char *host, size_t host_size,
        char *port, size_t port_size) {
    if (!endpoint || endpoint->size < ENDPOINT_SIZE) {
        return avs_errno(AVS_EINVAL);
    }
    const struct sockaddr_in_min *addr =
            (const struct sockaddr_in_min *) &endpoint->data.buf;
    if (host && host_size > 0) {
        ULONG ip = addr->sin_addr;
        snprintf(host, host_size, "%lu.%lu.%lu.%lu",
                 (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                 (ip >> 8) & 0xFF, ip & 0xFF);
    }
    if (port && port_size > 0) {
        snprintf(port, port_size, "%u", (unsigned) avs_port_htons(addr->sin_port));
    }
    return AVS_OK;
}
