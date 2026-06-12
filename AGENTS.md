# AGENTS.md

## Build

**No CLI build system.** This project builds exclusively through Keil MDK-ARM.
- Open `MDK-ARM/xiot.uvprojx` in Keil uVision, press F7 (Build Target).
- Output: `MDK-ARM/xiot/`
- Compiler: ARMCLANG V6.24, target STM32L4R5ZIT6 (Cortex-M4 @ 120MHz)

No Makefile, CMakeLists.txt, or npm/pip tooling exists. Do not attempt `make`, `cmake`, or similar.

## CubeMX Code Generation

`xiot.ioc` is the source of truth for peripheral configuration. Regeneration overwrites `Core/Src/` and `Core/Inc/`.

- **Always** place custom code inside `/* USER CODE BEGIN */` / `/* USER CODE END */` blocks
- Code outside these markers **will be deleted** on regeneration
- CubeMX generates `usart.c`, `gpio.c`, `main.c`, `app_threadx.c`, `app_netxduo.c` — all have USER CODE blocks

## Adding New Source Files

If you add `.c` files, they must also be added in the Keil project:
- `MDK-ARM/xiot.uvprojx` → Manage Project Items → add to appropriate Source Group
- Also add include paths under Options → C/C++ → Include Paths

Current custom source groups:
| Group | Files |
|-------|-------|
| `BSP/A7683E` | `Drivers/BSP/A7683E/bsp_uart3.c`, `a7683e.c`, `cmux.c`, `cmux_port.c`, `cmux_serial.c` |
| `BSP/common` | `Drivers/BSP/common/ringbuf.c` |
| `EasyLogger` | `EasyLogger/src/elog.c`, `elog_port.c` |

## Key Architecture Gotchas

### Serial Abstraction (`bsp_serial_t`)

The modem serial port is abstracted via a vtable (`bsp_serial_t` in `Drivers/BSP/serial/bsp_serial.h`). Both the modem AT layer and NetX PPP use this interface, enabling seamless switching between:
- **Direct UART mode**: `bsp_serial_uart3` (UART3 DMA with double-buffer + idle-line ISR)
- **CMUX mode**: `cmux_serial_get(DLCI)` virtual serial ports per CMUX channel

`app_netxduo_set_serial()` and `a7683e_set_serial()` must be called before their respective subsystems start.

### CMUX Support

CMUX (GSM 07.10) is disabled by default (`CMUX_ENABLE=0` in `cmux.h`). When enabled:
- DLCI 0: control channel
- DLCI 1: AT commands
- DLCI 2: PPP data
- Uses UART3 RX hook (`bsp_uart3_set_rx_hook()`) to intercept raw bytes before ring buffer
- Fallback to direct UART mode if CMUX fails

### Thread Priorities

Higher number = higher priority in ThreadX:
| Priority | Thread | Stack |
|----------|--------|-------|
| 10 | `tx_app_thread` (heartbeat + init) | 512B |
| 12 | `Modem Init` (AT + PPP dial) | 2048B |
| 14 | `PPP Read` (feeds UART→NetX) | 1024B |
| 16 | `NTP Sync` (DNS + SNTP + RTC) | 2048B |
| 18 | `TCP Client` | 3072B |
| 19 | `UDP Client` | 3072B |
| 30 | `elog async` (drains log buffer) | 1024B |

ThreadX tick rate: 1000 Hz. Stack sizes defined in `app_threadx.c` and `app_netxduo.c`.

### PPP Lifecycle

1. `modem_thread_entry()` → `a7683e_init()` → `a7683e_pre_dial()` → `a7683e_ppp_dial()` (sends `ATD*99***1#`)
2. After CONNECT, calls `app_netxduo_create_ppp()` + `app_netxduo_start_ppp()`
3. `ppp_read_thread` reads from `ppp_serial->read()` → feeds `nx_ppp_byte_receive()`
4. PPP link-up callback sets `PPP_EVT_LINK_UP` event → NTP thread wakes

## Configuration Macros

| Macro | Default | File | Controls |
|-------|---------|------|----------|
| `A7683E_APN` | `"cmnet"` | `Drivers/BSP/A7683E/a7683e.h` | Cellular APN |
| `A7683E_PDP_TYPE` | `"IP"` | same | PDP type (IP/IPV6/IPV4V6) |
| `CMUX_ENABLE` | `0` | `Drivers/BSP/A7683E/cmux.h` | CMUX multiplexing |
| `NTP_SERVER_HOST` | `"ntp.aliyun.com"` | `NetXDuo/App/app_netxduo.c` | NTP server |
| `TCP_SERVER_HOST` | `"47.109.101.196"` | same | TCP server |
| `TCP_SERVER_PORT` | `9000` | same | TCP port |
| `UDP_SERVER_HOST` | `"47.109.101.196"` | same | UDP server |
| `UDP_SERVER_PORT` | `9003` | same | UDP port |
| `NX_APP_MEM_POOL_SIZE` | `65536` | `AZURE_RTOS/App/app_azure_rtos_config.h` | NetX memory pool |

## Do Not Edit

- `Drivers/STM32L4xx_HAL_Driver/` — vendor HAL, never modify
- `Middlewares/ST/` — Azure RTOS source, never modify
- `Core/Src/` and `Core/Inc/` outside of USER CODE blocks — will be overwritten

## Logging

Use EasyLogger macros: `LOG_E(tag, fmt, ...)`, `LOG_W(...)`, `LOG_I(...)`, `LOG_D(...)`, `LOG_V(...)`
- Tags: `"APP"`, `"MODEM"`, `"PPP"`, `"DNS"`, `"NTP"`, `"NET"`, `"ELOG"`, `"TCP"`, `"UDP"`
- Output: LPUART1 at 115200 baud (PG7/PG8), async mode
- Hex dump: `elog_hexdump(tag, data, len)`
