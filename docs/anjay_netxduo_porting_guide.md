# Anjay LwM2M Client 移植指南 (NetXDuo + ThreadX)

## 概述

本文档描述如何将 Anjay LwM2M Client 移植到 STM32L4 + ThreadX + NetXDuo PPP 平台，
实现通过蜂窝网络（A7683E modem）连接 LwM2M Bootstrap Server，使用 TLS 安全连接。

## 架构概览

```
┌─────────────────────────────────────────────────────────────┐
│                    Anjay LwM2M Client                        │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────────┐    │
│  │ Security │  │ Server  │  │ Device  │  │ Custom Obj  │    │
│  │  Object  │  │ Object  │  │ Object  │  │   ...       │    │
│  └─────────┘  └─────────┘  └─────────┘  └─────────────┘    │
├─────────────────────────────────────────────────────────────┤
│                    avs_commons (抽象层)                       │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐     │
│  │ avs_net     │  │ avs_coap    │  │ avs_crypto      │     │
│  │ (Socket抽象) │  │ (CoAP协议)  │  │ (TLS/mbedTLS)   │     │
│  └─────────────┘  └─────────────┘  └─────────────────┘     │
├─────────────────────────────────────────────────────────────┤
│              NetXDuo Socket 适配层 (需移植)                   │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  avs_net_socket_t vtable → NX_TCP_SOCKET 封装       │    │
│  └─────────────────────────────────────────────────────┘    │
├─────────────────────────────────────────────────────────────┤
│                    NetXDuo TCP/TLS                           │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────────┐    │
│  │ NX_IP   │  │ NX_TCP  │  │ NX_DNS  │  │ mbedTLS     │    │
│  └─────────┘  └─────────┘  └─────────┘  └─────────────┘    │
├─────────────────────────────────────────────────────────────┤
│                    ThreadX RTOS                              │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────────┐    │
│  │ Threads │  │ Mutex   │  │ Events  │  │ Byte Pool   │    │
│  └─────────┘  └─────────┘  └─────────┘  └─────────────┘    │
├─────────────────────────────────────────────────────────────┤
│  A7683E Modem ← USART3/DMA → PPP ← NetXDuo PPP Instance    │
└─────────────────────────────────────────────────────────────┘
```

## 移植步骤

### 步骤 1: 配置 Anjay 编译选项

创建 `anjay_config.h` 文件，配置 Anjay 功能开关：

```c
// Middlewares/ST/Anjay/config/anjay_config.h
#ifndef ANJAY_CONFIG_H
#define ANJAY_CONFIG_H

// ===== 基础配置 =====
// 启用 LwM2M 1.1 支持（Bootstrap 需要）
#define ANJAY_WITH_LWM2M11

// 启用 Bootstrap 支持
#define ANJAY_WITH_BOOTSTRAP

// 启用独立 Security/Server 对象（不依赖持久化存储）
#define ANJAY_WITH_MODULE_STANDALONE_OBJECTS

// ===== 网络配置 =====
// 使用 TCP 传输（而非 UDP/CoAP）
#define ANJAY_WITH_TCP

// 启用 TLS 支持（用于安全连接）
#define ANJAY_WITH_TLS

// ===== 安全配置 =====
// 启用 PSK 和 Certificate 两种安全模式
#define ANJAY_WITH_SECURITY_STRUCTURED

// ===== 内存优化 =====
// 禁用不需要的功能以节省 RAM
// #define ANJAY_WITH_LWM2M_GATEWAY     // 不需要网关功能
// #define ANJAY_WITH_SMS                // 不需要 SMS 绑定
// #define ANJAY_WITH_COAP_OSCORE       // 不需要 OSCORE

// ===== avs_commons 配置 =====
// 使用 mbedTLS 作为 TLS 后端
#define AVS_COMMONS_WITH_AVS_CRYPTO
#define AVS_COMMONS_WITH_MBEDTLS

// 启用 POSIX 兼容层（用于 socket 抽象）
#define AVS_COMMONS_POSIX_COMPAT_HEADER "avs_netxduo_posix_compat.h"

// ===== 调试配置 =====
// 启用日志（可选，生产环境可关闭）
#define ANJAY_WITH_LOGS
#define AVS_LOG_WITH_ANJAY_LOG

#endif // ANJAY_CONFIG_H
```

### 步骤 2: 实现 NetXDuo POSIX 兼容层

创建 `avs_netxduo_posix_compat.h`，为 avs_commons 提供 POSIX socket 抽象：

```c
// Middlewares/ST/Anjay/port/avs_netxduo_posix_compat.h
#ifndef AVS_NETXDUO_POSIX_COMPAT_H
#define AVS_NETXDUO_POSIX_COMPAT_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

// ===== Socket 类型定义 =====
typedef int sockfd_t;
typedef uint32_t socklen_t;

// ===== 地址族 =====
#define AF_INET     1
#define AF_INET6    2
#define AF_UNSPEC   0

// ===== Socket 类型 =====
#define SOCK_STREAM 1   // TCP
#define SOCK_DGRAM  2   // UDP

// ===== Socket 选项 =====
#define SOL_SOCKET  1
#define SO_RCVTIMEO 1
#define SO_SNDTIMEO 2
#define SO_REUSEADDR 3

#define IPPROTO_TCP  6
#define IPPROTO_UDP 17

// ===== 错误码 =====
#define EAGAIN      11
#define EWOULDBLOCK 11
#define EINPROGRESS 15
#define ETIMEDOUT   60
#define ECONNREFUSED 61
#define EHOSTUNREACH 65

// ===== 地址结构 =====
struct in_addr {
    uint32_t s_addr;
};

struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr {
    uint16_t sa_family;
    char sa_data[14];
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};

struct sockaddr_in6 {
    uint16_t sin6_family;
    uint16_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

struct sockaddr_storage {
    uint16_t ss_family;
    char __ss_padding[126];
};

// ===== 地址信息 =====
struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};

// ===== 时间结构 =====
struct timeval {
    long tv_sec;
    long tv_usec;
};

// ===== fd_set (select 使用) =====
#define FD_SETSIZE 16

typedef struct {
    uint32_t fds_bits[FD_SETSIZE / 32];
} fd_set;

#define FD_ZERO(set)    memset((set), 0, sizeof(fd_set))
#define FD_SET(fd, set) ((set)->fds_bits[(fd) / 32] |= (1U << ((fd) % 32)))
#define FD_CLR(fd, set) ((set)->fds_bits[(fd) / 32] &= ~(1U << ((fd) % 32)))
#define FD_ISSET(fd, set) ((set)->fds_bits[(fd) / 32] & (1U << ((fd) % 32)))

// ===== 字节序转换 =====
// STM32 是小端序，网络是大端序
#define htons(x) __REV16(x)
#define ntohs(x) __REV16(x)
#define htonl(x) __REV(x)
#define ntohl(x) __REV(x)

// ===== inet 地址转换 =====
static inline int inet_pton(int af, const char *src, void *dst) {
    // 简化实现，仅支持 IPv4 点分十进制
    if (af == AF_INET) {
        uint32_t addr = 0;
        int parts[4] = {0};
        int part = 0;
        const char *p = src;

        while (*p && part < 4) {
            if (*p == '.') {
                part++;
                p++;
            } else {
                parts[part] = parts[part] * 10 + (*p - '0');
                p++;
            }
        }

        addr = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
        *(uint32_t *)dst = htonl(addr);
        return 1;
    }
    return 0; // IPv6 暂不支持
}

static inline const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    if (af == AF_INET) {
        uint32_t addr = ntohl(*(const uint32_t *)src);
        snprintf(dst, size, "%lu.%lu.%lu.%lu",
                 (addr >> 24) & 0xFF, (addr >> 16) & 0xFF,
                 (addr >> 8) & 0xFF, addr & 0xFF);
        return dst;
    }
    return NULL;
}

// ===== errno 定义 =====
#ifndef errno
extern int errno;
#endif

#define AVS_EAGAIN      EAGAIN
#define AVS_ETIMEDOUT   ETIMEDOUT

#endif // AVS_NETXDUO_POSIX_COMPAT_H
```

### 步骤 3: 实现 NetXDuo Socket 适配层

创建 `avs_net_impl_netxduo.c`，实现 avs_net_socket_t 的 vtable：

```c
// Middlewares/ST/Anjay/port/avs_net_impl_netxduo.c

#include <avsystem/commons/avs_socket_v_table.h>
#include <avsystem/commons/avs_net.h>
#include <avsystem/commons/avs_memory.h>
#include <avsystem/commons/avs_errno.h>

#include "nx_api.h"
#include "nxd_dns.h"
#include "nxd_tcp.h"

#include "elog.h"
#define TAG "NETX_AVS"

// ===== 外部引用 =====
extern NX_IP nx_ip;           // NetX IP 实例（在 app_netxduo.c 中定义）
extern NX_DNS nx_dns_client;  // DNS 客户端

// ===== Socket 内部结构 =====
struct avs_net_socket_struct {
    const avs_net_socket_v_table_t *vtable;
    avs_net_socket_type_t type;
    avs_net_socket_state_t state;

    // NetXDuo TCP Socket
    NX_TCP_SOCKET tcp_socket;

    // 连接信息
    char remote_host[256];
    char remote_port[8];
    char local_host[48];
    char local_port[8];

    // 超时配置
    avs_time_duration_t recv_timeout;

    // TLS 配置（如果需要）
    void *tls_context;

    // 内存池（用于 socket 内部分配）
    NX_PACKET_POOL *pool;
};

// ===== 前向声明 =====
static const avs_net_socket_v_table_t netxduo_tcp_vtable;
static const avs_net_socket_v_table_t netxduo_ssl_vtable;

// ===== 辅助函数: DNS 解析 =====
static avs_error_t resolve_hostname(const char *host, ULONG *ip_addr) {
    UINT status;

    // 尝试直接解析 IP 地址
    status = nx_dns_host_by_name_get(&nx_dns_client, (CHAR *)host, ip_addr, 5000);
    if (status != NX_SUCCESS) {
        LOG_E(TAG, "DNS resolve failed for %s: %d", host, status);
        return avs_errno(AVS_EADDRNOTAVAIL);
    }

    return AVS_OK;
}

// ===== Socket 创建 =====
avs_error_t avs_net_tcp_socket_create(avs_net_socket_t **socket,
                                       const avs_net_socket_configuration_t *config) {
    if (!socket) {
        return avs_errno(AVS_EINVAL);
    }

    // 清理已有 socket
    if (*socket) {
        avs_net_socket_cleanup(socket);
    }

    // 分配 socket 结构
    avs_net_socket_t *sock = (avs_net_socket_t *)avs_calloc(1, sizeof(avs_net_socket_t));
    if (!sock) {
        return avs_errno(AVS_ENOMEM);
    }

    sock->vtable = &netxduo_tcp_vtable;
    sock->type = AVS_NET_TCP_SOCKET;
    sock->state = AVS_NET_SOCKET_STATE_CLOSED;
    sock->recv_timeout = AVS_NET_SOCKET_DEFAULT_RECV_TIMEOUT;

    *socket = sock;
    return AVS_OK;
}

#ifdef AVS_COMMONS_WITH_AVS_CRYPTO
avs_error_t avs_net_ssl_socket_create(avs_net_socket_t **socket,
                                       const avs_net_ssl_configuration_t *config) {
    if (!socket) {
        return avs_errno(AVS_EINVAL);
    }

    // 清理已有 socket
    if (*socket) {
        avs_net_socket_cleanup(socket);
    }

    // 分配 socket 结构
    avs_net_socket_t *sock = (avs_net_socket_t *)avs_calloc(1, sizeof(avs_net_socket_t));
    if (!sock) {
        return avs_errno(AVS_ENOMEM);
    }

    sock->vtable = &netxduo_ssl_vtable;
    sock->type = AVS_NET_SSL_SOCKET;
    sock->state = AVS_NET_SOCKET_STATE_CLOSED;
    sock->recv_timeout = AVS_NET_SOCKET_DEFAULT_RECV_TIMEOUT;

    // 保存 TLS 配置（后续 connect 时使用）
    // TODO: 深拷贝 config 中的证书/PSK 信息

    *socket = sock;
    return AVS_OK;
}
#endif

// ===== TCP Socket 连接 =====
static avs_error_t netxduo_tcp_connect(avs_net_socket_t *socket,
                                        const char *host,
                                        const char *port) {
    UINT status;
    ULONG server_ip;
    UINT server_port;

    if (!socket || socket->type != AVS_NET_TCP_SOCKET) {
        return avs_errno(AVS_EINVAL);
    }

    // DNS 解析
    avs_error_t err = resolve_hostname(host, &server_ip);
    if (avs_is_err(err)) {
        return err;
    }

    server_port = (UINT)atoi(port);

    // 创建 TCP socket
    status = nx_tcp_socket_create(
        &nx_ip,
        &socket->tcp_socket,
        "Anjay TCP",
        NX_IP_NORMAL,   // TOS
        NX_FRAGMENT_OK,  // DF bit
        128,             // TTL
        8192,            // 窗口大小
        NX_NULL,         // 不使用 urgent pointer 回调
        NX_NULL          // 不使用 disconnect 回调
    );

    if (status != NX_SUCCESS) {
        LOG_E(TAG, "TCP socket create failed: %d", status);
        return avs_errno(AVS_UNKNOWN_ERROR);
    }

    // 绑定到任意本地端口
    status = nx_tcp_client_socket_bind(&socket->tcp_socket, NX_ANY_PORT, 1000);
    if (status != NX_SUCCESS) {
        LOG_E(TAG, "TCP bind failed: %d", status);
        nx_tcp_socket_delete(&socket->tcp_socket);
        return avs_errno(AVS_UNKNOWN_ERROR);
    }

    // 连接到服务器
    // 注意：NetXDuo 的 connect 可能需要先建立底层连接（如 PPP）
    // 这里假设 PPP 已经建立
    status = nx_tcp_client_socket_connect(
        &socket->tcp_socket,
        server_ip,
        server_port,
        10000  // 10秒超时
    );

    if (status != NX_SUCCESS) {
        LOG_E(TAG, "TCP connect to %s:%s failed: %d", host, port, status);
        nx_tcp_client_socket_unbind(&socket->tcp_socket);
        nx_tcp_socket_delete(&socket->tcp_socket);
        return avs_errno(AVS_ETIMEDOUT);
    }

    // 保存连接信息
    strncpy(socket->remote_host, host, sizeof(socket->remote_host) - 1);
    strncpy(socket->remote_port, port, sizeof(socket->remote_port) - 1);
    socket->state = AVS_NET_SOCKET_STATE_CONNECTED;

    LOG_I(TAG, "TCP connected to %s:%s", host, port);
    return AVS_OK;
}

// ===== TLS Socket 连接 =====
#ifdef AVS_COMMONS_WITH_AVS_CRYPTO
static avs_error_t netxduo_ssl_connect(avs_net_socket_t *socket,
                                        const char *host,
                                        const char *port) {
    // TODO: 实现 TLS 握手
    // 1. 先建立 TCP 连接
    // 2. 创建 mbedTLS SSL context
    // 3. 配置证书/PSK
    // 4. 执行 TLS 握手

    // 临时实现：先建立普通 TCP 连接
    avs_error_t err = netxduo_tcp_connect(socket, host, port);
    if (avs_is_err(err)) {
        return err;
    }

    LOG_W(TAG, "TLS not implemented yet, using plain TCP");
    return AVS_OK;
}
#endif

// ===== 数据发送 =====
static avs_error_t netxduo_tcp_send(avs_net_socket_t *socket,
                                     const void *buffer,
                                     size_t buffer_length) {
    NX_PACKET *packet;
    UINT status;

    if (!socket || socket->state != AVS_NET_SOCKET_STATE_CONNECTED) {
        return avs_errno(AVS_EBADF);
    }

    // 分配数据包
    status = nx_packet_allocate(&pool_0, &packet, NX_TCP_PACKET,
                                socket->recv_timeout.duration.seconds > 0 ?
                                socket->recv_timeout.duration.seconds * 100 : 1000);
    if (status != NX_SUCCESS) {
        LOG_E(TAG, "Packet allocate failed: %d", status);
        return avs_errno(AVS_ENOMEM);
    }

    // 将数据拷贝到数据包
    status = nx_packet_data_append(packet, (void *)buffer, buffer_length,
                                    &pool_0, 1000);
    if (status != NX_SUCCESS) {
        nx_packet_release(packet);
        LOG_E(TAG, "Packet data append failed: %d", status);
        return avs_errno(AVS_UNKNOWN_ERROR);
    }

    // 发送数据
    status = nx_tcp_socket_send(&socket->tcp_socket, packet, 10000);
    if (status != NX_SUCCESS) {
        nx_packet_release(packet);
        LOG_E(TAG, "TCP send failed: %d", status);
        return avs_errno(AVS_UNKNOWN_ERROR);
    }

    return AVS_OK;
}

// ===== 数据接收 =====
static avs_error_t netxduo_tcp_receive(avs_net_socket_t *socket,
                                        size_t *out_bytes_received,
                                        void *buffer,
                                        size_t buffer_length) {
    NX_PACKET *packet;
    UINT status;
    ULONG timeout_ticks;

    if (!socket || socket->state != AVS_NET_SOCKET_STATE_CONNECTED) {
        return avs_errno(AVS_EBADF);
    }

    // 计算超时 ticks
    if (socket->recv_timeout.duration.seconds > 0) {
        timeout_ticks = socket->recv_timeout.duration.seconds * TX_TIMER_TICKS_PER_SECOND;
    } else {
        timeout_ticks = NX_WAIT_FOREVER;
    }

    // 接收数据包
    status = nx_tcp_socket_receive(&socket->tcp_socket, &packet, timeout_ticks);
    if (status != NX_SUCCESS) {
        if (status == NX_NO_PACKET) {
            *out_bytes_received = 0;
            return avs_errno(AVS_ETIMEDOUT);
        }
        LOG_E(TAG, "TCP receive failed: %d", status);
        return avs_errno(AVS_UNKNOWN_ERROR);
    }

    // 拷贝数据到用户缓冲区
    ULONG data_len = packet->nx_packet_length;
    if (data_len > buffer_length) {
        data_len = buffer_length;
    }

    // 从数据包拷贝数据
    NX_PACKET *current = packet;
    size_t bytes_copied = 0;
    uint8_t *dst = (uint8_t *)buffer;

    while (current && bytes_copied < data_len) {
        ULONG chunk_len = current->nx_packet_append_ptr - current->nx_packet_prepend_ptr;
        if (chunk_len > data_len - bytes_copied) {
            chunk_len = data_len - bytes_copied;
        }
        memcpy(dst + bytes_copied, current->nx_packet_prepend_ptr, chunk_len);
        bytes_copied += chunk_len;
        current = current->nx_packet_next;
    }

    // 释放数据包
    nx_packet_release(packet);

    *out_bytes_received = bytes_copied;
    return AVS_OK;
}

// ===== Socket 关闭 =====
static avs_error_t netxduo_tcp_close(avs_net_socket_t *socket) {
    if (!socket) {
        return avs_errno(AVS_EINVAL);
    }

    if (socket->state == AVS_NET_SOCKET_STATE_CONNECTED) {
        nx_tcp_socket_disconnect(&socket->tcp_socket, 1000);
        nx_tcp_client_socket_unbind(&socket->tcp_socket);
        nx_tcp_socket_delete(&socket->tcp_socket);
    }

    socket->state = AVS_NET_SOCKET_STATE_CLOSED;
    return AVS_OK;
}

// ===== Socket 清理 =====
avs_error_t avs_net_socket_cleanup(avs_net_socket_t **socket) {
    if (!socket || !*socket) {
        return AVS_OK;
    }

    // 关闭连接
    netxduo_tcp_close(*socket);

    // 释放内存
    avs_free(*socket);
    *socket = NULL;

    return AVS_OK;
}

// ===== VTable 定义 =====
static const avs_net_socket_v_table_t netxduo_tcp_vtable = {
    .connect = netxduo_tcp_connect,
    .send = netxduo_tcp_send,
    .receive = netxduo_tcp_receive,
    .close = netxduo_tcp_close,
    .cleanup = avs_net_socket_cleanup,
    // 其他函数可以暂时为空或返回错误
    .send_to = NULL,
    .receive_from = NULL,
    .bind = NULL,
    .accept = NULL,
    .shutdown = netxduo_tcp_close,
    .get_remote_host = NULL,
    .get_remote_hostname = NULL,
    .get_remote_port = NULL,
    .get_local_host = NULL,
    .get_local_port = NULL,
    .get_opt = NULL,
    .set_opt = NULL,
    .get_system = NULL,
    .interface_name = NULL,
    .decorate = NULL
};

#ifdef AVS_COMMONS_WITH_AVS_CRYPTO
static const avs_net_socket_v_table_t netxduo_ssl_vtable = {
    .connect = netxduo_ssl_connect,
    .send = netxduo_tcp_send,        // TLS 后 send/recv 需要加解密
    .receive = netxduo_tcp_receive,
    .close = netxduo_tcp_close,
    .cleanup = avs_net_socket_cleanup,
    .send_to = NULL,
    .receive_from = NULL,
    .bind = NULL,
    .accept = NULL,
    .shutdown = netxduo_tcp_close,
    .get_remote_host = NULL,
    .get_remote_hostname = NULL,
    .get_remote_port = NULL,
    .get_local_host = NULL,
    .get_local_port = NULL,
    .get_opt = NULL,
    .set_opt = NULL,
    .get_system = NULL,
    .interface_name = NULL,
    .decorate = NULL
};
#endif

// ===== 需要实现的 POSIX 兼容函数 =====
// 这些函数供 avs_commons 内部使用

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    ULONG ip_addr;
    avs_error_t err = resolve_hostname(node, &ip_addr);
    if (avs_is_err(err)) {
        return -1;
    }

    // 分配 addrinfo 结构
    struct addrinfo *ai = (struct addrinfo *)avs_calloc(1, sizeof(struct addrinfo));
    struct sockaddr_in *sa = (struct sockaddr_in *)avs_calloc(1, sizeof(struct sockaddr_in));
    if (!ai || !sa) {
        avs_free(ai);
        avs_free(sa);
        return -1;
    }

    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = ip_addr;
    sa->sin_port = htons(atoi(service));

    ai->ai_family = AF_INET;
    ai->ai_socktype = SOCK_STREAM;
    ai->ai_protocol = IPPROTO_TCP;
    ai->ai_addrlen = sizeof(struct sockaddr_in);
    ai->ai_addr = (struct sockaddr *)sa;
    ai->ai_next = NULL;

    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res) {
    if (res) {
        avs_free(res->ai_addr);
        avs_free(res);
    }
}
```

### 步骤 4: 集成到 ThreadX 应用

在 `app_netxduo.c` 中添加 Anjay 初始化代码：

```c
// ===== Anjay 线程配置 =====
#define ANJAY_THREAD_PRIO       20
#define ANJAY_THREAD_STACK      (1024 * 4)

static TX_THREAD anjay_thread;
static VOID *anjay_stack_ptr;

// ===== Bootstrap 服务器配置 =====
// 根据你的 LwM2M 平台修改这些参数
#define BOOTSTRAP_SERVER_URI    "coaps://your-bootstrap-server.com:5684"
#define BOOTSTRAP_SERVER_IP     "0.0.0.0"  // 将由 Bootstrap 配置

// ===== Anjay 线程入口 =====
static void anjay_thread_entry(ULONG input) {
    anjay_t *anjay = NULL;
    const anjay_dm_object_def_t **security_obj = NULL;
    const anjay_dm_object_def_t **server_obj = NULL;

    LOG_I(TAG, "Waiting for PPP link-up...");

    // 等待 PPP 链路建立
    ULONG ppp_flags;
    tx_event_flags_get(&ppp_events, PPP_EVT_LINK_UP, TX_AND_CLEAR,
                       &ppp_flags, TX_WAIT_FOREVER);

    LOG_I(TAG, "PPP link-up, starting Anjay...");

    // ===== 1. 配置 Anjay =====
    anjay_configuration_t config = anjay_get_default_config();

    // 设置 endpoint name（设备名称，Bootstrap 需要）
    config.endpoint_name = "STM32L4_A7683E";

    // 配置 UDP/CoAP 传输参数（使用 TCP 时需调整）
    // 如果使用 TCP，需要在 anjay_config.h 中定义 ANJAY_WITH_TCP

    // 配置 PRNG（随机数生成器）
    // TODO: 使用硬件随机数生成器
    config.prng_ctx = NULL;

    // ===== 2. 创建 Anjay 实例 =====
    anjay = anjay_new(&config);
    if (!anjay) {
        LOG_E(TAG, "Failed to create Anjay instance");
        return;
    }

    // ===== 3. 安装 Security 对象 =====
    security_obj = standalone_security_object_install(anjay);
    if (!security_obj) {
        LOG_E(TAG, "Failed to install Security object");
        goto cleanup;
    }

    // ===== 4. 配置 Bootstrap 服务器 =====
    // 注意：Bootstrap 模式下，Security 和 Server 对象由 Bootstrap 服务器配置
    // 这里只需要配置连接 Bootstrap 服务器所需的信息

    standalone_security_instance_t bootstrap_security = {
        .ssid = ANJAY_SSID_BOOTSTRAP,
        .server_uri = BOOTSTRAP_SERVER_URI,
        .bootstrap_server = true,
        .security_mode = ANJAY_SECURITY_CERTIFICATE,  // 或 ANJAY_SECURITY_PSK

        // TLS 证书配置（根据你的服务器修改）
        .public_cert_or_psk_identity = NULL,  // 客户端证书或 PSK Identity
        .public_cert_or_psk_identity_size = 0,
        .private_cert_or_psk_key = NULL,      // 客户端私钥或 PSK Key
        .private_cert_or_psk_key_size = 0,
        .server_public_key = NULL,             // 服务器 CA 证书
        .server_public_key_size = 0,
    };

    anjay_iid_t bootstrap_iid = ANJAY_ID_INVALID;
    if (standalone_security_object_add_instance(security_obj,
                                                 &bootstrap_security,
                                                 &bootstrap_iid)) {
        LOG_E(TAG, "Failed to add Bootstrap Security instance");
        goto cleanup;
    }

    LOG_I(TAG, "Bootstrap Security instance added (IID=%d)", bootstrap_iid);

    // ===== 5. 运行 Anjay 主循环 =====
    LOG_I(TAG, "Entering Anjay event loop...");

    while (1) {
        // anjay_sched_run 处理所有待处理的事件
        // 返回值是下次需要调用的最长时间
        anjay_sched_run(anjay);

        // 让出 CPU 给其他线程
        tx_thread_sleep(10);  // 10ms
    }

cleanup:
    if (anjay) {
        anjay_delete(anjay);
    }
    LOG_E(TAG, "Anjay thread exited");
}

// ===== 初始化函数 =====
void MX_Anjay_Init(void) {
    UINT status;

    // 分配 Anjay 线程栈
    status = tx_byte_allocate(&byte_pool, &anjay_stack_ptr,
                               ANJAY_THREAD_STACK, TX_NO_WAIT);
    if (status != TX_SUCCESS) {
        LOG_E(TAG, "Failed to allocate Anjay thread stack");
        return;
    }

    // 创建 Anjay 线程
    status = tx_thread_create(&anjay_thread, "Anjay", anjay_thread_entry, 0,
                               anjay_stack_ptr, ANJAY_THREAD_STACK,
                               ANJAY_THREAD_PRIO, ANJAY_THREAD_PRIO,
                               TX_NO_TIME_SLICE, TX_AUTO_START);
    if (status != TX_SUCCESS) {
        LOG_E(TAG, "Failed to create Anjay thread");
        return;
    }

    LOG_I(TAG, "Anjay thread created");
}
```

### 步骤 5: 配置 mbedTLS 集成

如果需要 TLS 安全连接，需要集成 mbedTLS：

```c
// Middlewares/ST/Anjay/port/avs_mbedtls_netxduo.c

#include "mbedtls/ssl.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/certs.h"

// ===== mbedTLS NetXDuo 网络回调 =====
// mbedTLS 需要自定义的网络发送/接收回调，底层使用 NetXDuo

static int mbedtls_netxduo_send(void *ctx, const unsigned char *buf, size_t len) {
    avs_net_socket_t *socket = (avs_net_socket_t *)ctx;

    // 调用我们实现的 send 函数
    avs_error_t err = netxduo_tcp_send(socket, buf, len);
    if (avs_is_err(err)) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }

    return (int)len;
}

static int mbedtls_netxduo_recv(void *ctx, unsigned char *buf, size_t len) {
    avs_net_socket_t *socket = (avs_net_socket_t *)ctx;
    size_t bytes_received = 0;

    // 调用我们实现的 receive 函数
    avs_error_t err = netxduo_tcp_receive(socket, &bytes_received, buf, len);
    if (avs_is_err(err)) {
        if (avs_errno(err) == AVS_ETIMEDOUT) {
            return MBEDTLS_ERR_SSL_TIMEOUT;
        }
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }

    return (int)bytes_received;
}

// ===== TLS 握手函数 =====
avs_error_t netxduo_tls_handshake(avs_net_socket_t *socket,
                                    const avs_net_ssl_configuration_t *config) {
    int ret;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_x509_crt ca_cert;
    mbedtls_x509_crt client_cert;
    mbedtls_pk_context client_key;

    // 初始化 mbedTLS 结构
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_x509_crt_init(&ca_cert);
    mbedtls_x509_crt_init(&client_cert);
    mbedtls_pk_init(&client_key);

    // 配置随机数生成器
    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                 (const unsigned char *)"anjay", 5);
    if (ret != 0) {
        LOG_E(TAG, "mbedtls_ctr_drbg_seed failed: -0x%04X", -ret);
        goto cleanup;
    }

    // 配置 SSL
    ret = mbedtls_ssl_config_defaults(&conf,
                                       MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        LOG_E(TAG, "mbedtls_ssl_config_defaults failed: -0x%04X", -ret);
        goto cleanup;
    }

    // 设置随机数生成器
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

    // 加载 CA 证书（服务器证书验证）
    if (config->data.cert.server_cert_validation &&
        config->data.cert.trusted_certs.array_lengths) {
        // 从 avs_crypto 结构加载证书
        // TODO: 实现证书加载
    }

    // 加载客户端证书（双向认证）
    if (config->data.cert.client_cert.array_lengths) {
        // TODO: 实现客户端证书加载
    }

    // 设置网络回调
    mbedtls_ssl_set_bio(&ssl, socket,
                          mbedtls_netxduo_send,
                          mbedtls_netxduo_recv,
                          NULL);  // 不使用 recv_timeout 回调

    // 设置 SNI（Server Name Indication）
    if (config->server_name_indication) {
        mbedtls_ssl_set_hostname(&ssl, config->server_name_indication);
    }

    // 执行 TLS 握手
    LOG_I(TAG, "Starting TLS handshake...");
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            LOG_E(TAG, "TLS handshake failed: -0x%04X", -ret);
            goto cleanup;
        }
    }

    LOG_I(TAG, "TLS handshake successful");

    // 保存 TLS context 到 socket
    // TODO: 保存 ssl context 以便后续加解密

cleanup:
    // 注意：不能在这里释放 ssl context，需要在 socket 关闭时释放
    // mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_x509_crt_free(&ca_cert);
    mbedtls_x509_crt_free(&client_cert);
    mbedtls_pk_free(&client_key);

    if (ret != 0) {
        return avs_errno(AVS_UNKNOWN_ERROR);
    }
    return AVS_OK;
}
```

### 步骤 6: Keil 工程配置

在 Keil 工程中添加以下文件和路径：

**源文件组：**
```
Anjay/
├── src/core/*.c                    (Anjay 核心)
├── src/modules/*.c                 (Anjay 模块)
├── src/standalone/security/*.c     (Security 对象)
├── src/standalone/server/*.c       (Server 对象)
├── deps/avs_commons/src/net/*.c    (网络抽象)
├── deps/avs_commons/src/crypto/mbedtls/*.c  (TLS)
├── deps/avs_coap/src/*.c           (CoAP 协议)
└── port/avs_net_impl_netxduo.c     (NetXDuo 适配层)
```

**头文件路径：**
```
../Middlewares/ST/Anjay/include_public
../Middlewares/ST/Anjay/deps/avs_commons/include_public
../Middlewares/ST/Anjay/deps/avs_coap/include_public
../Middlewares/ST/Anjay/port
../Middlewares/ST/Anjay/config
```

**预定义宏：**
```
ANJAY_CONFIG_FILE="anjay_config.h"
```

## Bootstrap 流程说明

1. **设备上电** → 初始化硬件、PPP 拨号
2. **PPP 链路建立** → Anjay 线程启动
3. **连接 Bootstrap Server** → 使用预配置的 Security 实例
4. **Bootstrap 握手** → 服务器下发 Security 和 Server 对象配置
5. **Bootstrap 完成** → 设备使用新配置连接正式 LwM2M Server
6. **注册成功** → 设备可以上报数据、接收下发命令

## 调试建议

1. **先不使用 TLS**：用 CoAP (UDP) 测试基本功能
2. **使用本地 LwM2M Server**：如 Leshan，方便抓包调试
3. **启用 Anjay 日志**：在 anjay_config.h 中定义 ANJAY_WITH_LOGS
4. **检查内存使用**：ThreadX byte pool 大小要足够（建议 128KB+）
5. **检查 PPP MTU**：确保 TCP 窗口大小与 PPP MTU 匹配

## 常见问题

**Q: DNS 解析失败？**
A: 检查 PPP 链路是否已建立，DNS 服务器是否正确配置（可通过 AT 命令设置）。

**Q: TCP 连接超时？**
A: 检查防火墙设置，确认服务器端口是否开放，检查 PPP 路由是否正确。

**Q: TLS 握手失败？**
A: 检查证书是否正确，时钟是否已同步（TLS 验证证书有效期）。

**Q: 内存不足？**
A: 增加 ThreadX byte pool 大小，或减少 Anjay 缓冲区配置。

## 参考资料

- [Anjay 文档](https://avsystem.github.io/Anjay-doc/)
- [NetXDuo API 参考](https://github.com/eclipse-threadx/rtos-docs)
- [mbedTLS 文档](https://tls.mbed.org/)
