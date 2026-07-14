# STM32 ThreadX NetX PPP A7683E

Cellular IoT project for **SIMCom A7683E** modem on **STM32L4R5ZIT6** (Cortex-M4 @ 120MHz). Establishes a PPP data link via Azure RTOS ThreadX + NetX Duo, performs NTP time synchronization over cellular, and runs an **Anjay LwM2M client** for device management.

## Features

- **PPP over cellular** — A7683E modem dial via AT commands, PPP negotiation with IPCP
- **Dual transport** — USART3 (direct) or USB (composite device) for AT/PPP
- **Multi-module support** — Auto-detect VID/PID (0x9011, 0x9028) with per-module AT port mapping
- **Anjay LwM2M client** — Bootstrap mode, DTLS PSK security, Device object (/3)
- **NTP time sync** — DNS resolution + SNTP client, RTC synchronization with configurable timezone
- **TCP/UDP iperf TX** — Optional throughput tests (compile-time guard `IPERF_ENABLE`)
- **EasyLogger** — Async colored logging via LPUART1 (DMA TX)

## Architecture

```text
A7683E Modem
  ├── USART3/DMA ──> bsp_uart3 (double-buffer, idle-ISR)
  └── USB Bulk  ──> bsp_usb   (lwrb, semaphore-driven RX)
                    | bytes
              bsp_serial_t (abstract serial interface)
                    |
              a7683e (AT commands for init/dial, IMEI read)
                    | PPP dial -> raw mode
              nx_ppp_byte_receive() <-- ppp_read_thread
                    | NetX PPP state machine
              NX_IP instance (IP negotiated via IPCP)
                    |
              ├── NX_DNS -> resolve hostnames
              ├── NX_SNTP_CLIENT -> get NTP time -> RTC sync
              ├── Anjay LwM2M Client
              │     ├── Bootstrap (coaps://, DTLS PSK)
              │     ├── Device Object (/3)
              │     └── Security/Server objects (standalone)
              └── TCP/UDP iperf TX (optional)
```

## Thread Overview

ThreadX priority: **lower number = higher priority**.

| Thread | Priority | Stack | Purpose |
| --- | --- | --- | --- |
| `PPP Read` | 5 | 1024B | Reads serial bytes, feeds to nx_ppp_byte_receive() |
| `Modem Init` | 10 | 2048B | AT command init, IMEI read, PPP dial, nx_ppp_start() |
| `elog async` | 11 | 1024B | Low-priority log output (drains async ring buffer) |
| `tx_app_thread` | 20 | 512B | EasyLogger init, USB/UART3 BSP init, LED heartbeat |
| `NTP Sync` | 31 | 3072B | Waits for PPP link-up, DNS resolve, SNTP sync, RTC set |
| `Anjay` | 30 | 8192B | LwM2M client: Bootstrap, DTLS, event loop |
| `TCP iperf TX` | 32 | 3072B | TCP TX throughput test (when `IPERF_ENABLE=1`) |
| `UDP iperf TX` | 33 | 3072B | UDP TX throughput test (when `IPERF_ENABLE=1`) |

**Execution order:** PPP link-up → NTP sync → Anjay start. When `IPERF_ENABLE=0` (default), iperf threads are not created.

## Hardware

| Peripheral | Function | Pins | Notes |
| --- | --- | --- | --- |
| LPUART1 | Debug UART | PG7 (TX), PG8 (RX) | 115200 baud, DMA, EasyLogger output |
| USART3 | Modem UART | PD8 (TX), PD9 (RX) | 115200 baud, DMA double-buffer |
| USB OTG FS | Modem USB | PA11 (DM), PA12 (DP) | USB Host, bulk transfers |
| GPIO | Modem control | PF13 (PWR_EN), PF14 (DTR), PE11 (RING) | Power, sleep, ring indicator |
| GPIO | LEDs | PB14 (LD3 red), PB7 (LD2 blue) | LD2 = heartbeat, LD3 = error |
| RTC | Real-time clock | — | Set from NTP time after sync |

## Interrupt Priority

```text
Preempt 0 (highest):  DMA1_Ch1-4 (USART3/LPUART1 RX/TX), OTG_FS (USB)
Preempt 1:            USART3, LPUART1, RNG
Preempt 4:            SysTick (ThreadX scheduler tick)
Preempt 15 (lowest):  TIM6 (HAL tick), PendSV, SVC
```

## Anjay LwM2M Client

Integrated using **Anjay** library with native **NetX Duo** socket transport (no BSD layer).

### Configuration

| Macro | Default | File | Purpose |
| --- | --- | --- | --- |
| `ANJAY_USE_BOOTSTRAP` | `1` | `NetXDuo/App/app_anjay.c` | Enable Bootstrap mode |
| `ANJAY_ENDPOINT_NAME` | `"XIOT_Client01"` | `NetXDuo/App/app_anjay.c` | LwM2M endpoint name |
| `BOOTSTRAP_SERVER_URI` | `"coaps://lwm2m-test.avsystem.io:5694"` | `NetXDuo/App/app_anjay.c` | Bootstrap server URI |
| `BOOTSTRAP_PSK_IDENTITY` | `"862095060018816"` | `NetXDuo/App/app_anjay.c` | DTLS PSK identity (IMEI) |
| `BOOTSTRAP_PSK_KEY` | `"1234567890abcdef1234567890abcdef"` | `NetXDuo/App/app_anjay.c` | DTLS PSK key (hex) |

### Device Object (/3)

| Resource | RID | Type | Description |
| --- | --- | --- | --- |
| Manufacturer | 0 | String R | "SIMCom" |
| Model Number | 1 | String R | "A7683E" |
| Serial Number | 2 | String R | IMEI (read via AT+GSN during init) |
| Firmware Version | 3 | String R | "1.0.0" |
| Reboot | 4 | Execute | Triggers `HAL_NVIC_SystemReset()` |
| Error Code | 11 | Integer RM | 0 = no error |
| Binding Mode | 16 | String R | "U" (UDP) |
| Software Version | 19 | String R | Anjay version string |

### mbedTLS Tuning

| Macro | Value | File | Purpose |
| --- | --- | --- | --- |
| `MBEDTLS_SSL_MAX_CONTENT_LEN` | 4096 | `mbedtls/config.h` | Reduced from 16KB to save heap |
| `MBEDTLS_SSL_DTLS_MAX_BUFFERING` | 8192 | `mbedtls/config.h` | Reduced from 32KB to save heap |
| Heap size | 96KB | `startup_stm32l4r5xx.s` | Enough for two DTLS sessions |

## Configuration Macros

### Modem

| Macro | Default | File | Purpose |
| --- | --- | --- | --- |
| `A7683E_APN` | `"cmnet"` | `Drivers/BSP/A7683E/a7683e.h` | Cellular APN |
| `A7683E_PDP_TYPE` | `"IP"` | `Drivers/BSP/A7683E/a7683e.h` | PDP context type |
| `A7683E_TRANSPORT` | `A7683E_TRANSPORT_USB` | `Drivers/BSP/A7683E/a7683e.h` | USB or USART3 |

### USB Multi-PID

| Macro | Default | File | Purpose |
| --- | --- | --- | --- |
| `UX_HOST_CLASS_MODEM_VID` | `0x1E0E` | `USBX/App/ux_host_class_modem.h` | USB Vendor ID |
| `UX_HOST_CLASS_MODEM_PID_LIST` | `{0x9011, 0x9028}` | `USBX/App/ux_host_class_modem.h` | Supported PID list |
| `A7683E_USB_AT_IFNUM` | `5` | `USBX/App/app_usbx_host.h` | Fallback AT port ifnum |

**PID → AT port mapping** (in `app_usbx_host.h`):

| PID | AT Port | Data Port |
| --- | --- | --- |
| `0x9011` | interface 5 | interface 4 |
| `0x9028` | interface 1 | — |

### NTP

| Macro | Default | File | Purpose |
| --- | --- | --- | --- |
| `NTP_SERVER_HOST` | `"ntp.aliyun.com"` | `NetXDuo/App/net_ntp.c` | NTP server hostname |
| `NTP_TZ_OFFSET_HOURS` | `8` | `NetXDuo/App/net_ntp.c` | UTC timezone offset (hours) |

### iperf (compile-time guarded)

Set `IPERF_ENABLE=1` to include iperf threads. Default is `0` (disabled) to save 6KB stack memory.

| Macro | Default | File | Purpose |
| --- | --- | --- | --- |
| `IPERF_ENABLE` | `0` | `NetXDuo/App/net_iperf.c` | Enable iperf threads |
| `IPERF_TCP_SERVER_HOST` | `"47.109.101.196"` | `NetXDuo/App/net_iperf.c` | TCP iperf server |
| `IPERF_TCP_SERVER_PORT` | `9010` | `NetXDuo/App/net_iperf.c` | TCP iperf port |
| `IPERF_UDP_SERVER_HOST` | `"47.109.101.196"` | `NetXDuo/App/net_iperf.c` | UDP iperf server |
| `IPERF_UDP_SERVER_PORT` | `9011` | `NetXDuo/App/net_iperf.c` | UDP iperf port |
| `IPERF_TEST_DURATION` | `10 * TX_TIMER_TICKS_PER_SECOND` | `NetXDuo/App/net_iperf.c` | Test duration (10s) |
| `IPERF_UDP_PACKET_SIZE` | `1470` | `NetXDuo/App/net_iperf.c` | UDP payload size |

### Logging

| Macro | Default | File | Purpose |
| --- | --- | --- | --- |
| `ELOG_OUTPUT_LVL` | `ELOG_LVL_VERBOSE` | `EasyLogger/inc/elog_cfg.h` | Max log level |
| `ELOG_ASYNC_OUTPUT_BUF_SIZE` | `65536` | `EasyLogger/inc/elog_cfg.h` | Async ring buffer size |
| `ELOG_COLOR_ENABLE` | `1` | `EasyLogger/inc/elog_cfg.h` | ANSI color output |

### UART3

| Macro | Default | File | Purpose |
| --- | --- | --- | --- |
| `BSP_UART3_RX_BUF_SIZE` | `512` | `Drivers/BSP/A7683E/bsp_uart3.h` | DMA receive buffer size |
| `BSP_UART3_RING_BUF_SIZE` | `4096` | `Drivers/BSP/A7683E/bsp_uart3.h` | Ring buffer size |
| `BSP_UART3_DEBUG` | `0` | `Drivers/BSP/A7683E/bsp_uart3.h` | UART3 hex dump logging |

## Building

Open `MDK-ARM/xiot.uvprojx` in **Keil uVision**. Build with **Project -> Build Target (F7)**.

**Source groups to add in Keil:**

- `BSP/A7683E` — `Drivers/BSP/A7683E/bsp_uart3.c`, `a7683e.c`, `bsp_usb.c`, `cmux.c`, `cmux_serial.c`, `cmux_port.c`
- `BSP/serial` — (header-only, no .c)
- `EasyLogger` — `EasyLogger/src/elog.c`, `elog_port.c`
- `Anjay` — `NetXDuo/App/app_anjay.c`, `app_anjay_device_object.c`, `net_ntp.c`, `net_dns.c`, `net_iperf.c`

**Include paths to add:**

- `../Drivers/BSP/A7683E`
- `../Drivers/BSP/serial`
- `../EasyLogger/inc`
- `../Middlewares/ST/Anjay/include_public`
- `../Middlewares/ST/Anjay/config`
- `../Middlewares/Third_Party/mbedtls/include`
- `../Core/Inc` (for BSD POSIX compat headers)

> **Note:** This project uses STM32CubeMX-generated code. Peripheral configuration is in `xiot.ioc`. Regeneration overwrites `Core/Src/` — keep custom code inside `USER CODE BEGIN/END` blocks.

## Project Structure

```text
Core/Src/, Core/Inc/          CubeMX-generated peripheral init + BSD POSIX headers
Drivers/BSP/A7683E/           Modem BSP: UART3, USB, AT commands, CMUX, IMEI
Drivers/BSP/serial/           Abstract serial interface (bsp_serial_t)
Drivers/BSP/lwrb/             Lightweight ring buffer library
Drivers/STM32L4xx_HAL_Driver/ STM32 HAL (vendor)
EasyLogger/                   Lightweight logging framework
NetXDuo/App/                  Anjay LwM2M + PPP + DNS + SNTP + NTP + iperf
USBX/App/                     USBX host: modem class, event handling
AZURE_RTOS/App/               ThreadX/NetX byte pool creation
Middlewares/ST/Anjay/         Anjay LwM2M client library (local source)
Middlewares/ST/               ThreadX + NetX Duo + USBX + PPP/DNS/SNTP
Middlewares/Third_Party/      mbedTLS + mbed-crypto
xiot.ioc                      CubeMX project file
```

## Serial Interface (bsp_serial_t)

All transport backends implement the same vtable:

```c
typedef struct bsp_serial {
    const char *name;
    void       *user_data;
    void (*init)(struct bsp_serial *self);
    uint16_t (*read)(struct bsp_serial *self, uint8_t *buf, uint16_t len, uint32_t timeout_ms);
    void (*write)(struct bsp_serial *self, const uint8_t *data, uint16_t len);
    uint16_t (*rx_available)(struct bsp_serial *self);
} bsp_serial_t;
```

**Backends:**

- `bsp_uart3` — USART3 DMA with double-buffer + idle-ISR, RX drop counter via `bsp_uart3_get_rx_drop_count()`
- `bsp_usb` — USB bulk via USBX, event-driven RX (semaphore, no busy-polling)
- `cmux_serial` — CMUX virtual channels over USART3

## License

MIT
