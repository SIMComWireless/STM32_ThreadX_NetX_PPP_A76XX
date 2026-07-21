/**
  ******************************************************************************
  * @file    net_dns.c
  * @brief   DNS client — hostname resolution using native NetX Duo API
  ******************************************************************************
  * @note
  *   Provides dns_client_init() and resolve_host() for all modules that
  *   need DNS resolution (NTP, TCP iperf, UDP iperf, Anjay).
  *
  *   The global dns_client is created on PPP link-up and deleted on
  *   PPP link-down (by app_netxduo_destroy_ppp).
  ******************************************************************************
  */

#include "net_dns.h"
#include "nx_api.h"
#include "nxd_dns.h"
#include "nx_ppp.h"
#include "elog.h"

#include <stdio.h>

#define TAG_DNS "DNS"

/* ============================================================================
 * NetX objects — defined in app_netxduo.c
 * ============================================================================ */

extern NX_IP             ip_0;
extern NX_PACKET_POOL    pool_0;
extern NX_PPP            ppp_0;

/* ============================================================================
 * Global DNS client — shared with Anjay port (avs_net_impl_netxduo.c,
 * avs_port_netxduo.c) via extern declarations in app_netxduo.h
 * ============================================================================ */

NX_DNS       dns_client;
volatile UINT dns_client_initialized = 0;
static TX_MUTEX dns_init_mutex;

void dns_early_init(void)
{
    tx_mutex_create(&dns_init_mutex, "DNS Init Mutex", TX_INHERIT);
}

/* ============================================================================
 * DNS client initialization
 * ============================================================================ */

UINT dns_client_init(void)
{
    if (dns_client_initialized) {
        return NX_SUCCESS;
    }

    /* Serialize initialization — may be called from link-up callback and other threads */
    tx_mutex_get(&dns_init_mutex, TX_WAIT_FOREVER);

    /* Double-check after acquiring lock */
    if (dns_client_initialized) {
        tx_mutex_put(&dns_init_mutex);
        return NX_SUCCESS;
    }

    UINT status;

    status = nx_dns_create(&dns_client, &ip_0, (UCHAR *)"DNS Client");
    if (status != NX_SUCCESS) {
        elog_e(TAG_DNS, "DNS client create failed: 0x%02X", status);
        tx_mutex_put(&dns_init_mutex);
        return status;
    }

    /* NX_DNS_CLIENT_USER_CREATE_PACKET_POOL is defined — must provide a
     * packet pool manually so DNS queries can allocate packets. */
    status = nx_dns_packet_pool_set(&dns_client, &pool_0);
    if (status != NX_SUCCESS) {
        elog_e(TAG_DNS, "DNS packet pool set failed: 0x%02X", status);
        tx_mutex_put(&dns_init_mutex);
        return status;
    }

    /* Add DNS servers — try carrier-assigned from PPP IPCP, plus fallbacks */
    ULONG dns1 = 0, dns2 = 0;
    UINT dns1_rc = nx_ppp_dns_address_get(&ppp_0, &dns1);
    UINT dns2_rc = nx_ppp_secondary_dns_address_get(&ppp_0, &dns2);

    if (dns1_rc == NX_SUCCESS) nx_dns_server_add(&dns_client, dns1);
    if (dns2_rc == NX_SUCCESS) nx_dns_server_add(&dns_client, dns2);

    /* Add well-known public DNS as fallback */
    nx_dns_server_add(&dns_client, IP_ADDRESS(223, 5, 5, 5));   /* Alibaba DNS */
    nx_dns_server_add(&dns_client, IP_ADDRESS(8, 8, 8, 8));     /* Google DNS */

    dns_client_initialized = 1;
    tx_mutex_put(&dns_init_mutex);
    elog_i(TAG_DNS, "DNS client initialized (carrier: %s/%s)",
           dns1_rc == NX_SUCCESS ? "OK" : "none",
           dns2_rc == NX_SUCCESS ? "OK" : "none");

    return NX_SUCCESS;
}

/* ============================================================================
 * Hostname resolution
 * ============================================================================ */

UINT resolve_host(const char *host, ULONG *ip_address)
{
    /* Fast path: try as dotted-decimal IP */
    UINT dots = 0;
    UINT all_digits = 1;
    for (const char *p = host; *p; p++) {
        if (*p == '.') dots++;
        else if (*p < '0' || *p > '9') all_digits = 0;
    }
    if (dots == 3 && all_digits) {
        UINT a, b, c, d;
        if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
            a <= 255 && b <= 255 && c <= 255 && d <= 255) {
            *ip_address = IP_ADDRESS(a, b, c, d);
            return NX_SUCCESS;
        }
    }

    /* DNS path: use native nx_dns_host_by_name_get() */
    if (!dns_client_initialized) {
        elog_e(TAG_DNS, "DNS client not initialized");
        return NX_NOT_ENABLED;
    }

    ULONG resolved_ip;
    UINT status = nx_dns_host_by_name_get(&dns_client, (UCHAR *)host,
                                           &resolved_ip,
                                           5 * NX_IP_PERIODIC_RATE);
    if (status != NX_SUCCESS) {
        elog_e(TAG_DNS, "nx_dns_host_by_name_get(%s) failed: 0x%02X", host, status);
        return status;
    }

    *ip_address = resolved_ip;

    elog_d(TAG_DNS, "Resolved %s -> %lu.%lu.%lu.%lu", host,
           (*ip_address >> 24) & 0xFF, (*ip_address >> 16) & 0xFF,
           (*ip_address >> 8) & 0xFF, *ip_address & 0xFF);

    return NX_SUCCESS;
}
