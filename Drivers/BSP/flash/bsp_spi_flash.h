/**
  ******************************************************************************
  * @file    bsp_spi_flash.h
  * @brief   W25Q128 SPI NOR Flash driver for STM32L4 + ThreadX
  ******************************************************************************
  * @note
  *   Hardware: W25Q128CSIG (16MB SPI NOR Flash)
  *   SPI: SPI1 (PA5=SCK, PA6=MISO, PA7=MOSI)
  *   Control: PD14=CS, PD15=WP, PF12=HOLD
  *   Transfer: DMA + RTOS semaphore (non-blocking)
  ******************************************************************************
  */

#ifndef BSP_SPI_FLASH_H
#define BSP_SPI_FLASH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ---------- Flash geometry (W25Q128) --------------------------------------- */

#define W25Q128_PAGE_SIZE           256         /* bytes per page              */
#define W25Q128_SECTOR_SIZE         4096        /* bytes per sector (4KB)      */
#define W25Q128_BLOCK_SIZE_32K      (32*1024)   /* 32KB block                  */
#define W25Q128_BLOCK_SIZE_64K      (64*1024)   /* 64KB block                  */
#define W25Q128_FLASH_SIZE          (16*1024*1024) /* 16MB total               */
#define W25Q128_SECTOR_COUNT        (W25Q128_FLASH_SIZE / W25Q128_SECTOR_SIZE)

/* ---------- Status register bits ------------------------------------------- */

#define W25Q128_SR_BUSY             (1 << 0)    /* Write in progress           */
#define W25Q128_SR_WEL              (1 << 1)    /* Write enable latch          */

/* ---------- SFDP (JESD216) geometry result ---------------------------------- */

typedef struct {
    uint32_t capacity_bytes;       /* Flash total capacity in bytes            */
    uint32_t sector_size;          /* Minimum erase granularity (bytes)        */
    uint32_t page_size;            /* Page program size (bytes)                */
    uint32_t block_size_32k;       /* 32KB erase block size, 0 if unsupported  */
    uint32_t block_size_64k;       /* 64KB erase block size, 0 if unsupported  */
    uint32_t sector_count;         /* Total sector_count (sector_size units)   */
    uint8_t  sfdp_valid;           /* 1 if SFDP was successfully parsed        */
} bsp_flash_geometry_t;

/* ---------- Public API ----------------------------------------------------- */

/**
 * @brief  Initialize flash (SPI, GPIO, read JEDEC ID)
 * @return 0 on success, negative on error
 */
int bsp_spi_flash_init(void);

/**
 * @brief  Read JEDEC manufacturer + device ID
 * @param  id  Output: 3-byte JEDEC ID (manufacturer, memory_type, capacity)
 * @return 0 on success
 */
int bsp_spi_flash_read_id(uint8_t id[3]);

/**
 * @brief  Read data from flash
 * @param  addr  Start address (0-based)
 * @param  buf   Destination buffer
 * @param  len   Number of bytes to read
 * @return 0 on success
 */
int bsp_spi_flash_read(uint32_t addr, uint8_t *buf, uint32_t len);

/**
 * @brief  Program a page (up to 256 bytes, must not cross page boundary)
 * @param  addr  Start address (must be page-aligned for best performance)
 * @param  buf   Source data
 * @param  len   Number of bytes (1..256)
 * @return 0 on success
 */
int bsp_spi_flash_write_page(uint32_t addr, const uint8_t *buf, uint32_t len);

/**
 * @brief  Write arbitrary data (handles page-boundary crossing internally)
 * @param  addr  Start address
 * @param  buf   Source data
 * @param  len   Number of bytes
 * @return 0 on success
 */
int bsp_spi_flash_write(uint32_t addr, const uint8_t *buf, uint32_t len);

/**
 * @brief  Erase a 4KB sector
 * @param  addr  Any address within the sector
 * @return 0 on success
 */
int bsp_spi_flash_erase_sector(uint32_t addr);

/**
 * @brief  Erase a 64KB block
 * @param  addr  Any address within the block
 * @return 0 on success
 */
int bsp_spi_flash_erase_block64k(uint32_t addr);

/**
 * @brief  Erase the entire chip (takes several seconds)
 * @return 0 on success
 */
int bsp_spi_flash_erase_chip(void);

/**
 * @brief  Wait until all pending operations complete
 * @return 0 on success, negative on timeout
 */
int bsp_spi_flash_wait_ready(uint32_t timeout_ms);

/**
 * @brief  Read and parse SFDP (Serial Flash Discoverable Parameters, JESD216)
 * @param  geo  Output: flash geometry filled from SFDP data
 * @return 0 on success, negative on error (no SFDP or invalid signature)
 */
int bsp_spi_flash_read_sfdp(bsp_flash_geometry_t *geo);

#ifdef __cplusplus
}
#endif

#endif /* BSP_SPI_FLASH_H */
