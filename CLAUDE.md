# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

STM32L4R5ZIT6 (Cortex-M4) embedded project for interfacing with a SIMCom A7683E cellular modem via PPP (Point-to-Point Protocol). Uses Azure RTOS ThreadX + NetXDuo for networking, and STM32CubeMX-generated HAL code. Built with Keil MDK-ARM (ARMCLANG V6.24).

**Goal:** Establish a cellular data link via PPP, resolve an NTP server via DNS, and synchronize the MCU RTC.

## Build

Open `MDK-ARM/xiot.uvprojx` in Keil uVision. Build with Project → Build Target (F7). Output is generated in `MDK-ARM/xiot/`.

There is no Makefile or CLI build system — all building is done through the Keil IDE.

**New source groups to add in Keil:**
- `BSP/A7683E` — `Drivers/BSP/A7683E/bsp_uart3.c`, `a7683e.c`
- `EasyLogger` — `EasyLogger/src/elog.c`, `elog_port.c`

**New include paths to add in Keil:**
- `../Drivers/BSP/A7683E`
- `../EasyLogger/inc`

## Architecture

### Data Flow

```
A7683E Modem ←USART3/DMA→ bsp_uart3 (double-buffer, idle-ISR)
                    ↓ bytes
              a7683e (AT commands for init/dial)
                    ↓ PPP dial → raw mode
              nx_ppp_byte_receive() ← ppp_read_thread
                    ↓ NetX PPP state machine
              NX_IP instance (IP negotiated via IPCP)
                    ↓
              NX_DNS → resolve ntp.aliyun.com
                    ↓
              NX_SNTP_CLIENT → get NTP time
                    ↓
              RTC set from Unix timestamp
```

### ThreadX Threads

ThreadX priority: **lower number = higher priority**.

| Thread | Priority | Stack | Purpose |
|--------|----------|-------|---------|
| `PPP Read` | 5 | 1024B | Reads UART3 bytes, feeds to nx_ppp_byte_receive() |
| `Modem Init` | 10 | 2048B | AT command init, PPP dial, nx_ppp_start() |
| `elog async` | 11 | 1024B | Low-priority log output (drains async ring buffer) |
| `tx_app_thread` | 20 | 512B | EasyLogger init, UART3 BSP init, LED heartbeat |
| `NTP Sync` | 31 | 3072B | Waits for PPP link-up, DNS resolve, SNTP sync |
| `TCP iperf TX` | 32 | 3072B | TCP TX throughput test (conditional, `IPERF_ENABLE`) |
| `UDP iperf TX` | 33 | 3072B | UDP TX throughput test (conditional, `IPERF_ENABLE`) |

Tick rate: 1000 Hz (`TX_TIMER_TICKS_PER_SECOND`).

**Execution order:** PPP link-up → TCP iperf → UDP iperf → NTP sync (iperf skipped when `IPERF_ENABLE=0`).

### Code Organization

- `Core/Src/` and `Core/Inc/` — CubeMX-generated peripheral init (do not edit outside USER CODE blocks)
- `Drivers/BSP/A7683E/` — Modem BSP: UART3 DMA driver + AT command layer
- `Drivers/STM32L4xx_HAL_Driver/` — STM32 HAL (vendor, do not edit)
- `EasyLogger/` — Lightweight logging framework (elog)
- `NetXDuo/App/` — NetX PPP + DNS + SNTP application logic
- `AZURE_RTOS/App/` — ThreadX/NetX byte pool creation
- `Middlewares/ST/` — ThreadX kernel + NetXDuo stack + PPP/DNS/SNTP addons
- `xiot.ioc` — CubeMX project file (source of truth for peripheral config)

### CubeMX USER CODE Blocks

All CubeMX-generated source files use `/* USER CODE BEGIN ... */` / `/* USER CODE END ... */` markers. **Always place custom code inside these blocks** — anything outside them gets overwritten when the `.ioc` file is regenerated.

### Peripheral Layout

| Peripheral | Function | Pins | Notes |
|-----------|----------|------|-------|
| LPUART1 | Debug UART | PG7 (TX), PG8 (RX) | 115200 baud, DMA, used by EasyLogger |
| USART3 | Modem UART | PD8 (TX), PD9 (RX) | 115200 baud, DMA double-buffer, HIGH priority |
| GPIO | Modem control | PF13 (PWR_EN), PF14 (DTR), PE11 (RING) | Power enable, sleep/wake, ring indicator |
| GPIO | LEDs | PB14 (LD3 red), PB7 (LD2 blue) | LD2 = heartbeat, LD3 = error indicator |
| RTC | Real-time clock | — | Set from NTP time after sync |

### Modem Interface (A7683E)

- BSP layer: `Drivers/BSP/A7683E/bsp_uart3.{c,h}` — UART3 DMA double-buffer with idle-line detection
- AT layer: `Drivers/BSP/A7683E/a7683e.{c,h}` — AT command send/receive, power control, PPP dial
- Configurable APN: `A7683E_APN` macro (default "cmnet", change in a7683e.h)

### Logging (EasyLogger)

- Output: LPUART1 at 115200 baud (PG7/PG8)
- Async mode: log messages buffered in ring buffer, output from dedicated low-priority thread
- Color: ANSI escape codes for colored terminal output
- Levels: ASSERT (red), ERROR (red), WARN (yellow), INFO (green), DEBUG (cyan), VERBOSE (white)
- Macros: `LOG_E(tag, fmt, ...)`, `LOG_W(...)`, `LOG_I(...)`, `LOG_D(...)`, `LOG_V(...)`
- Tags: "APP", "MODEM", "PPP", "DNS", "NTP", "NET", "ELOG"
- Hex dump: `elog_hexdump(tag, data, len)`

### NetX PPP + DNS + SNTP

- PPP: `nx_ppp_create()` → `nx_ppp_start()` → link-up callback → DNS/SNTP
- DNS: global `dns_client`, created on-demand by `dns_client_init()` on PPP link-up, uses native `nx_dns_host_by_name_get()` API (no BSD socket layer)
- SNTP: `nx_sntp_client_create()` → `nx_sntp_client_run_unicast()` → `nx_sntp_client_request_unicast_time()`
- NTP server: `NTP_SERVER_HOST` macro (default "ntp.aliyun.com", change in app_netxduo.c)
- NTP epoch: NTP seconds since 1900 → Unix seconds = `ntp_seconds - 2208988800`
- RTC sync: HAL_RTC_SetTime/SetDate after NTP response

### Key Event Flow

1. `tx_app_thread` initializes EasyLogger and UART3 BSP, creates `Modem Init` thread
2. `Modem Init` powers on modem, runs AT init sequence, dials PPP (`ATD*99***1#`)
3. After CONNECT, `Modem Init` calls `nx_ppp_start()` — PPP negotiation begins
4. `PPP Read` thread feeds UART3 bytes to `nx_ppp_byte_receive()`
5. On PPP link-up, link-up callback sets event flag → `NTP Sync` thread wakes
6. `NTP Sync` creates DNS client, resolves `ntp.aliyun.com`, runs SNTP, sets RTC

## Key Files

| File | Purpose |
|------|---------|
| `Core/Src/main.c` | System clock config (HSI → PLL → 120MHz), peripheral init sequence |
| `Core/Src/app_threadx.c` | App init, modem thread, LED heartbeat |
| `Core/Src/usart.c` | UART init and DMA MspInit for LPUART1 and USART3 |
| `Core/Src/stm32l4xx_it.c` | ISR handlers including USART3 idle-line |
| `Core/Inc/main.h` | GPIO pin definitions and port mappings |
| `Drivers/BSP/A7683E/bsp_uart3.c` | UART3 DMA double-buffer driver |
| `Drivers/BSP/A7683E/a7683e.c` | A7683E AT command layer |
| `EasyLogger/src/elog.c` | Log engine (color, async, tags) |
| `EasyLogger/src/elog_port.c` | LPUART1 output port |
| `NetXDuo/App/app_netxduo.c` | PPP + DNS + SNTP + TCP/UDP iperf (native NetX Duo API) |
| `Middlewares/ST/Anjay/port/avs_net_impl_netxduo.c` | Anjay socket impl using native nx_udp_socket_* API |
| `AZURE_RTOS/App/app_azure_rtos.c` | TX/NX byte pool creation |
| `xiot.ioc` | CubeMX project (regenerates Core/Src/) |

## Configuration Macros

| Macro | Default | Location | Purpose |
|-------|---------|----------|---------|
| `A7683E_APN` | "cmnet" | `Drivers/BSP/A7683E/a7683e.h` | Cellular APN |
| `A7683E_PDP_TYPE` | "IP" | `Drivers/BSP/A7683E/a7683e.h` | PDP context type |
| `A7683E_TRANSPORT` | `A7683E_TRANSPORT_USB` | `Drivers/BSP/A7683E/a7683e.h` | UART/CMUX/USB transport |
| `NTP_SERVER_HOST` | "ntp.aliyun.com" | `NetXDuo/App/app_netxduo.c` | NTP server hostname |
| `NTP_TZ_OFFSET_HOURS` | 8 | `NetXDuo/App/app_netxduo.c` | UTC timezone offset (hours) |
| `IPERF_ENABLE` | 0 | `NetXDuo/App/app_netxduo.c` | Enable iperf threads (0=off) |
| `BSP_UART3_DEBUG` | 0 | `Drivers/BSP/A7683E/bsp_uart3.h` | UART3 hex dump logging |
| `ELOG_OUTPUT_LVL` | `ELOG_LVL_VERBOSE` | `EasyLogger/inc/elog_cfg.h` | Max log level |
| `ELOG_ASYNC_OUTPUT_BUF_SIZE` | 16384 | `EasyLogger/inc/elog_cfg.h` | Async ring buffer size |
| `ELOG_COLOR_ENABLE` | 1 | `EasyLogger/inc/elog_cfg.h` | ANSI color output |
| `NX_APP_MEM_POOL_SIZE` | 65536 | `AZURE_RTOS/App/app_azure_rtos_config.h` | NetX memory pool |
