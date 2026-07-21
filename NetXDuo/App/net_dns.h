/**
  ******************************************************************************
  * @file    net_dns.h
  * @brief   DNS client — hostname resolution using native NetX Duo API
  ******************************************************************************
  */

#ifndef NET_DNS_H
#define NET_DNS_H

#include "nx_api.h"

/**
  * @brief  Early init — create DNS mutex. Call once from MX_NetXDuo_Init().
  */
void dns_early_init(void);

/**
  * @brief  Initialize DNS client — call after PPP link-up.
  *         Creates the global DNS client, adds carrier-assigned + fallback
  *         DNS servers. Idempotent (safe to call multiple times).
  * @return NX_SUCCESS on success
  */
UINT dns_client_init(void);

/**
  * @brief  Resolve hostname to IPv4 address.
  *         Fast path: parses dotted-decimal directly.
  *         Slow path: uses nx_dns_host_by_name_get().
  * @param  host        Hostname or dotted-decimal IP string
  * @param  ip_address  Output: resolved IP in host byte order
  * @return NX_SUCCESS on success
  */
UINT resolve_host(const char *host, ULONG *ip_address);

#endif /* NET_DNS_H */
