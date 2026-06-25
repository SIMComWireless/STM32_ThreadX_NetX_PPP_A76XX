/**
  ******************************************************************************
  * @file    lx_stm32_nor_custom_driver.h
  * @brief   LevelX custom NOR flash driver — SFDP-based auto-detection
  ******************************************************************************
  * @note
  *   This driver bridges LevelX NOR flash API to bsp_spi_flash.
  *   Flash geometry is auto-detected via SFDP (JESD216) at runtime,
  *   so any SFDP-compliant SPI NOR flash works without code changes.
  ******************************************************************************
  */

#ifndef LX_STM32_NOR_CUSTOM_DRIVER_H
#define LX_STM32_NOR_CUSTOM_DRIVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lx_api.h"
#include <stdint.h>

/* ---------- Configuration -------------------------------------------------- */

/** LevelX logical sector size in bytes (always 512 for FAT compatibility) */
#define NOR_SECTOR_SIZE                 512

/** Sector buffer size in ULONG words (512 / 4 = 128) */
#define NOR_SECTOR_BUFFER_SIZE          (NOR_SECTOR_SIZE / sizeof(ULONG))

/** Driver ID for fx_stm32_levelx_nor_driver lookup */
#define NOR_CUSTOM_DRIVER_ID            0xA7683E00

/** Driver name shown by LevelX */
#define NOR_CUSTOM_DRIVER_NAME          "SPI NOR (SFDP)"

/* ---------- Runtime geometry (filled by sfdp_init from SFDP data) ----------- */

extern uint32_t nor_block_size;         /* Erase block size in bytes (e.g. 4096)   */
extern uint32_t nor_total_blocks;       /* Total erase blocks (e.g. 4096)          */
extern uint32_t nor_flash_capacity;     /* Total flash capacity in bytes            */
extern uint32_t nor_page_size;          /* Page program size in bytes               */

/* ---------- Public API ------------------------------------------------------ */

/**
 * @brief  Initialize flash geometry from SFDP data
 *         Must be called before lx_stm32_nor_custom_driver_initialize().
 * @return 0 on success, negative on error
 */
int lx_nor_custom_driver_sfdp_init(void);

/**
 * @brief  LevelX NOR flash driver initialization
 *         Called by lx_nor_flash_open(). Sets up geometry and function pointers.
 */
UINT lx_stm32_nor_custom_driver_initialize(LX_NOR_FLASH *nor_flash);

#ifdef __cplusplus
}
#endif

#endif /* LX_STM32_NOR_CUSTOM_DRIVER_H */
