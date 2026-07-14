/**
 ******************************************************************************
 * @file    nx_bsd_posix_compat.h
 * @brief   POSIX compatibility layer for NetX Duo BSD socket API
 ******************************************************************************
 * @note
 *   This header is included by avs_commons via AVS_COMMONS_POSIX_COMPAT_HEADER.
 *   NetX Duo's nxd_bsd.h already defines all POSIX socket types and functions.
 *   We only supplement with missing types that avs_commons expects.
 ******************************************************************************
 */

#ifndef NX_BSD_POSIX_COMPAT_H
#define NX_BSD_POSIX_COMPAT_H

/* ===== Standard C headers ===== */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ===== NetX Duo BSD socket API ===== */
#include "nxd_bsd.h"

/* ===== POSIX types not provided by nxd_bsd.h ===== */

/* Socket file descriptor type — avs_commons uses this throughout */
#ifndef sockfd_t
typedef int sockfd_t;
#endif

/* Socket address family type */
#ifndef sa_family_t
typedef uint16_t sa_family_t;
#endif

/* Signed size type */
#ifndef ssize_t
typedef int ssize_t;
#endif

/* Note: socklen_t is already defined by nxd_bsd.h as ULONG.
 * The warnings about socklen_t* vs INT* are harmless and can be ignored. */

/* ===== POSIX error codes ===== */

#ifndef EBADF
#define EBADF           9
#endif

#ifndef EAGAIN
#define EAGAIN          11
#endif

#ifndef ENOMEM
#define ENOMEM          12
#endif

#ifndef EACCES
#define EACCES          13
#endif

#ifndef EFAULT
#define EFAULT          14
#endif

#ifndef EINVAL
#define EINVAL          22
#endif

#ifndef ENOSPC
#define ENOSPC          28
#endif

#ifndef EPIPE
#define EPIPE           32
#endif

#ifndef ERANGE
#define ERANGE          34
#endif

#ifndef ENOSYS
#define ENOSYS          38
#endif

#ifndef ENOTSUP
#define ENOTSUP         45
#endif

#ifndef EMSGSIZE
#define EMSGSIZE        90
#endif

#ifndef ETIMEDOUT
#define ETIMEDOUT       110
#endif

#ifndef ECONNREFUSED
#define ECONNREFUSED    111
#endif

#ifndef ECONNRESET
#define ECONNRESET      104
#endif

#ifndef EINPROGRESS
#define EINPROGRESS     115
#endif

#ifndef EISCONN
#define EISCONN         113
#endif

#ifndef ENOTCONN
#define ENOTCONN        107
#endif

/* ===== Additional POSIX defines ===== */

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT    0x40
#endif

#ifndef MSG_WAITALL
#define MSG_WAITALL     0x100
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL    0
#endif

/* Shutdown modes — nxd_bsd.h doesn't define these */
#ifndef SHUT_RD
#define SHUT_RD         0
#endif

#ifndef SHUT_WR
#define SHUT_WR         1
#endif

#ifndef SHUT_RDWR
#define SHUT_RDWR       2
#endif

/* nxd_bsd.h uses soc_close(), not close(). Map close() to soc_close(). */
#define close(fd)       soc_close(fd)

/* NetX Duo BSD does not provide shutdown(). Map to a no-op. */
static inline int shutdown(int fd, int how) {
    (void)fd;
    (void)how;
    return 0;
}

/* Note: errno is already defined by nxd_bsd.h as:
 *   #define errno (tx_thread_identify() -> bsd_errno)
 * No need to redefine it here. */

#endif /* NX_BSD_POSIX_COMPAT_H */
