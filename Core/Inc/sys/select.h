/**
 * @file select.h
 * @brief Minimal POSIX stub for embedded platform (Anjay event loop support)
 */

#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H

#include <stdint.h>
#include <stddef.h>

/* time_t definition */
#ifndef _TIME_T_DEFINED
#define _TIME_T_DEFINED
typedef long time_t;
#endif

/* struct timeval */
#ifndef _STRUCT_TIMEVAL_DEFINED
#define _STRUCT_TIMEVAL_DEFINED
struct timeval {
    time_t tv_sec;
    long   tv_usec;
};
#endif

/* fd_set definitions */
#ifndef FD_SETSIZE
#define FD_SETSIZE 64
#endif

typedef struct {
    uint32_t fds_bits[(FD_SETSIZE + 31) / 32];
} fd_set;

#define FD_ZERO(set)    memset((set), 0, sizeof(fd_set))
#define FD_SET(fd, set) ((set)->fds_bits[(fd) / 32] |= (1U << ((fd) % 32)))
#define FD_CLR(fd, set) ((set)->fds_bits[(fd) / 32] &= ~(1U << ((fd) % 32)))
#define FD_ISSET(fd, set) ((set)->fds_bits[(fd) / 32] & (1U << ((fd) % 32)))

/* Invalid socket value */
#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif

/* Stub select() function - always returns 0 (no sockets ready) */
static inline int select(int nfds, fd_set *readfds, fd_set *writefds,
                         fd_set *exceptfds, struct timeval *timeout) {
    (void)nfds;
    (void)readfds;
    (void)writefds;
    (void)exceptfds;
    (void)timeout;
    return 0; /* No sockets ready */
}

#endif /* _SYS_SELECT_H */
