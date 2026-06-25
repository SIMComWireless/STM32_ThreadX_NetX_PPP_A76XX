/**
  ******************************************************************************
  * @file    lx_stm32_nor_custom_driver.c
  * @brief   LevelX custom NOR flash driver — bridges LevelX to bsp_spi_flash
  ******************************************************************************
  * @note
  *   Flash geometry is auto-detected via SFDP (JESD216).
  *   LevelX calls these driver functions for all physical flash I/O.
  *
  *   Address mapping:
  *     LevelX tracks flash as ULONG* pointers: base_address + word_offset.
  *     C pointer arithmetic already accounts for sizeof(ULONG), so the
  *     byte offset is simply: (ULONG)flash_address - base_address.
  *     We set base_address = 0x10000000 (pseudo-address, never dereferenced).
  ******************************************************************************
  */

#include "lx_stm32_nor_custom_driver.h"
#include "bsp_spi_flash.h"
#include "elog.h"
#include <string.h>

#define TAG "LX_NOR"

/* Set to 1 to enable verbose erase/write/read logging */
#define LX_NOR_DEBUG    0

#if LX_NOR_DEBUG
#define LX_LOG_D(...)   elog_i(TAG, __VA_ARGS__)
#else
#define LX_LOG_D(...)   ((void)0)
#endif

/* ---------- Pseudo base address --------------------------------------------- */

/*
 * LevelX tracks flash positions as pointers: base_address + word_offset.
 * Since SPI flash is not memory-mapped, we use a non-zero pseudo base
 * so pointer arithmetic yields the correct word offset.
 */
#define NOR_FLASH_BASE_ADDRESS          0x10000000UL

/* ---------- Runtime geometry (filled by sfdp_init) -------------------------- */

uint32_t nor_block_size     = 4096;     /* default: 4KB (overwritten by SFDP)   */
uint32_t nor_total_blocks   = 0;        /* filled by sfdp_init                   */
uint32_t nor_flash_capacity = 0;        /* filled by sfdp_init                   */
uint32_t nor_page_size      = 256;      /* default: 256 bytes (overwritten)      */

/* ---------- Sector buffer (required when LX_DIRECT_READ is not defined) ----- */

#ifndef LX_DIRECT_READ
static ULONG nor_sector_buffer[NOR_SECTOR_BUFFER_SIZE];
#endif

/* ---------- SFDP initialization --------------------------------------------- */

int lx_nor_custom_driver_sfdp_init(void)
{
    bsp_flash_geometry_t geo;

    if (bsp_spi_flash_read_sfdp(&geo) != 0)
    {
        elog_e(TAG, "SFDP read failed — using W25Q128 defaults");
        /* Fallback to W25Q128 defaults */
        nor_block_size     = 4096;
        nor_flash_capacity = 16 * 1024 * 1024;
        nor_page_size      = 256;
        nor_total_blocks   = nor_flash_capacity / nor_block_size;
        return -1;
    }

    /* Use sector_size (minimum erase granularity) as LevelX block size */
    nor_block_size     = geo.sector_size;
    nor_flash_capacity = geo.capacity_bytes;
    nor_page_size      = geo.page_size ? geo.page_size : 256;
    nor_total_blocks   = geo.sector_count;

    elog_i(TAG, "SFDP OK: %luMB, block=%lu, page=%lu, blocks=%lu",
           (unsigned long)(nor_flash_capacity / (1024 * 1024)),
           (unsigned long)nor_block_size,
           (unsigned long)nor_page_size,
           (unsigned long)nor_total_blocks);

    return 0;
}

/* ---------- Driver functions ------------------------------------------------ */

/**
 * @brief  Read words from NOR flash
 * @param  flash_address  Pointer into LevelX address space (pseudo-address)
 * @param  destination    Destination buffer (ULONG-aligned)
 * @param  words          Number of ULONG words to read
 */
static UINT lx_nor_driver_read(ULONG *flash_address, ULONG *destination, ULONG words)
{
    /*
     * LevelX tracks flash as ULONG* pointers.  Pointer arithmetic already
     * accounts for sizeof(ULONG): (flash_address - base) yields the word
     * offset.  The byte offset is simply the pointer difference.
     */
    ULONG byte_offset = (ULONG)flash_address - NOR_FLASH_BASE_ADDRESS;
    ULONG byte_len    = words * sizeof(ULONG);

    LX_LOG_D("Read 0x%06lX %lu w", (unsigned long)byte_offset, (unsigned long)words);

    if (bsp_spi_flash_read(byte_offset, (uint8_t *)destination, byte_len) != 0)
    {
        elog_e(TAG, "Read failed at 0x%08lX (%lu bytes)", (unsigned long)byte_offset, (unsigned long)byte_len);
        return LX_ERROR;
    }

    return LX_SUCCESS;
}

/**
 * @brief  Write words to NOR flash
 * @param  flash_address  Pointer into LevelX address space (pseudo-address)
 * @param  source         Source buffer (ULONG-aligned)
 * @param  words          Number of ULONG words to write
 */
static UINT lx_nor_driver_write(ULONG *flash_address, ULONG *source, ULONG words)
{
    ULONG byte_offset = (ULONG)flash_address - NOR_FLASH_BASE_ADDRESS;
    ULONG byte_len    = words * sizeof(ULONG);

    LX_LOG_D("Write 0x%06lX %lu w", (unsigned long)byte_offset, (unsigned long)words);

    if (bsp_spi_flash_write(byte_offset, (const uint8_t *)source, byte_len) != 0)
    {
        elog_e(TAG, "Write failed at 0x%08lX (%lu bytes)", (unsigned long)byte_offset, (unsigned long)byte_len);
        return LX_ERROR;
    }

    return LX_SUCCESS;
}

/**
 * @brief  Erase a physical block
 * @param  block        Block number (0 .. nor_total_blocks-1)
 * @param  erase_count  Erase count for wear-leveling tracking
 */
static UINT lx_nor_driver_block_erase(ULONG block, ULONG erase_count)
{
    ULONG byte_addr = block * nor_block_size;

    LX_PARAMETER_NOT_USED(erase_count);

    LX_LOG_D("Erase blk %lu → 0x%06lX", (unsigned long)block, (unsigned long)byte_addr);

    if (bsp_spi_flash_erase_sector(byte_addr) != 0)
    {
        elog_e(TAG, "Erase failed: blk=%lu addr=0x%06lX", (unsigned long)block, (unsigned long)byte_addr);
        return LX_ERROR;
    }

    return LX_SUCCESS;
}

/**
 * @brief  Verify that a block is fully erased (all 0xFF)
 * @param  block  Block number to verify
 */
static UINT lx_nor_driver_block_erased_verify(ULONG block)
{
    ULONG byte_addr = block * nor_block_size;

    LX_LOG_D("Verify blk %lu → 0x%06lX", (unsigned long)block, (unsigned long)byte_addr);
    ULONG words     = nor_block_size / sizeof(ULONG);
    ULONG buf[32];  /* Read in small chunks to save stack */
    ULONG offset;

    for (offset = 0; offset < words; offset += 8)
    {
        ULONG chunk = ((words - offset) < 8) ? (words - offset) : 8;
        ULONG chunk_bytes = chunk * sizeof(ULONG);

        if (bsp_spi_flash_read(byte_addr + offset * sizeof(ULONG),
                               (uint8_t *)buf, chunk_bytes) != 0)
        {
            return LX_ERROR;
        }

        /* Check all words in chunk are 0xFFFFFFFF */
        for (ULONG i = 0; i < chunk; i++)
        {
            if (buf[i] != 0xFFFFFFFF)
                return LX_ERROR;
        }
    }

    return LX_SUCCESS;
}

/* ---------- LevelX initialization ------------------------------------------ */

UINT lx_stm32_nor_custom_driver_initialize(LX_NOR_FLASH *nor_flash)
{
    elog_i(TAG, "LevelX NOR driver init — %lu blocks x %lu bytes",
           (unsigned long)nor_total_blocks, (unsigned long)nor_block_size);

    /* Set flash base address (pseudo, for pointer arithmetic) */
    nor_flash->lx_nor_flash_base_address = (ULONG *)NOR_FLASH_BASE_ADDRESS;

    /* Flash geometry (from SFDP) */
    nor_flash->lx_nor_flash_total_blocks    = nor_total_blocks;
    nor_flash->lx_nor_flash_words_per_block = nor_block_size / sizeof(ULONG);

    /* Driver function pointers */
    nor_flash->lx_nor_flash_driver_read              = lx_nor_driver_read;
    nor_flash->lx_nor_flash_driver_write              = lx_nor_driver_write;
    nor_flash->lx_nor_flash_driver_block_erase        = lx_nor_driver_block_erase;
    nor_flash->lx_nor_flash_driver_block_erased_verify = lx_nor_driver_block_erased_verify;

#ifndef LX_DIRECT_READ
    /* Sector buffer for LevelX internal use */
    nor_flash->lx_nor_flash_sector_buffer = nor_sector_buffer;
#endif

    elog_i(TAG, "LevelX NOR driver ready (%lu logical sectors)",
           (unsigned long)(nor_total_blocks * (nor_block_size / NOR_SECTOR_SIZE)));

    return LX_SUCCESS;
}
