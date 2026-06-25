/**
  ******************************************************************************
  * @file    app_filex.c
  * @brief   FileX application layer — NOR flash file system
  ******************************************************************************
  * @note
  *   Stack: FileX → fx_stm32_levelx_nor_driver → LevelX → bsp_spi_flash
  *
  *   Flash geometry is auto-detected via SFDP (JESD216) at runtime.
  *   Works with any SFDP-compliant SPI NOR flash without code changes.
  *
  *   Initialization sequence:
  *   1. bsp_spi_flash_init() — detect flash, prepare SPI
  *   2. lx_nor_custom_driver_sfdp_init() — read SFDP, get geometry
  *   3. fx_media_open() → FX_DRIVER_INIT → lx_nor_flash_open() — LevelX init
  *   4. If media open fails (blank flash), format then re-open
  ******************************************************************************
  */

#include "app_filex.h"
#include "fx_stm32_levelx_nor_driver.h"
#include "bsp_spi_flash.h"
#include "lx_stm32_nor_custom_driver.h"
#include "elog.h"
#include "tx_api.h"
#include <string.h>

#define TAG "FILEX"

/* ---------- Configuration --------------------------------------------------- */

/** Volume name for the NOR flash file system */
#define NOR_VOLUME_NAME         "NOR"

/** Hidden sectors (boot sector) */
#define NOR_HIDDEN_SECTORS      1

/** Root directory entries */
#define NOR_ROOT_DIR_ENTRIES    32

/* ---------- FileX objects --------------------------------------------------- */

static FX_MEDIA    nor_media;
static UCHAR       nor_media_memory[FX_MEDIA_MEMORY_SIZE];
static uint8_t     filex_initialized = 0;

/* ---------- Private helpers ------------------------------------------------- */

/**
 * @brief  Get FileX sector size — must match NOR_SECTOR_SIZE in LevelX driver
 *
 *  Uses nor_sector_size from SFDP if available, falls back to 512.
 *  For FAT compatibility, this is typically 512 bytes regardless of
 *  physical erase sector size.
 */
static uint32_t app_filex_sector_size(void)
{
    /* nor_sector_size is the LevelX logical sector size (512 for FAT).
     * nor_block_size is the physical erase sector size from SFDP.
     * FileX sector size must match LevelX NOR_SECTOR_SIZE. */
    return NOR_SECTOR_SIZE;  /* 512 — from lx_stm32_nor_custom_driver.h */
}

/**
 * @brief  Calculate total sectors available to FileX
 *
 *  LevelX reserves a fraction of flash for wear-leveling metadata.
 *  We subtract a conservative 1% margin.
 */
static uint32_t app_filex_total_sectors(void)
{
    uint32_t sector_size = app_filex_sector_size();
    uint32_t total = nor_flash_capacity / sector_size;

    /* Reserve 1% for LevelX overhead (mapping table + spare blocks) */
    if (total > 256)
        total -= (total / 100);

    return total;
}

/**
 * @brief  Format the NOR flash with a FAT file system
 *
 *  All geometry parameters are derived from SFDP-detected values.
 */
static UINT app_filex_format(void)
{
    UINT status;
    uint32_t sector_size = app_filex_sector_size();
    uint32_t total_sectors = app_filex_total_sectors();

    /* Cluster size = physical erase sector size (from SFDP).
     * This ensures one cluster = one erase block = optimal wear leveling. */
    uint32_t sectors_per_cluster = nor_block_size / sector_size;

    /* Clamp: must be power of 2, range [1..128] for FAT12/16 */
    if (sectors_per_cluster < 1)  sectors_per_cluster = 1;
    if (sectors_per_cluster > 128) sectors_per_cluster = 128;

    /* Chip erase before format.  LevelX needs clean (0xFF) blocks for its
     * internal mapping table.  Without this, writes to blocks with old data
     * will fail (NOR flash can only clear bits, not set them). */
    elog_i(TAG, "Chip erasing %luMB flash...",
           (unsigned long)(nor_flash_capacity / (1024 * 1024)));

    if (bsp_spi_flash_erase_chip() != 0)
    {
        elog_e(TAG, "Chip erase failed");
        return FX_IO_ERROR;
    }

    /* Verify erase completed — read back first 16 bytes, must be all 0xFF */
    {
        uint8_t verify[16];
        if (bsp_spi_flash_read(0, verify, sizeof(verify)) != 0)
        {
            elog_e(TAG, "Post-erase verify read failed");
            return FX_IO_ERROR;
        }
        for (int i = 0; i < 16; i++)
        {
            if (verify[i] != 0xFF)
            {
                elog_e(TAG, "Post-erase verify failed: byte[%d]=0x%02X (expected 0xFF)", i, verify[i]);
                return FX_IO_ERROR;
            }
        }
    }

    elog_i(TAG, "Erase complete + verified. Formatting (%lu sectors, %lu bytes/sector, "
           "%lu sectors/cluster)...",
           (unsigned long)total_sectors, (unsigned long)sector_size,
           (unsigned long)sectors_per_cluster);

    status = fx_media_format(&nor_media,
                             fx_stm32_levelx_nor_driver,            /* driver          */
                             (void *)(ULONG)NOR_CUSTOM_DRIVER_ID,   /* driver info     */
                             nor_media_memory,                      /* cache buffer    */
                             FX_MEDIA_MEMORY_SIZE,                  /* cache size      */
                             NOR_VOLUME_NAME,                       /* volume name     */
                             1,                                     /* number of FATs  */
                             NOR_ROOT_DIR_ENTRIES,                  /* root dir entries*/
                             NOR_HIDDEN_SECTORS,                    /* hidden sectors  */
                             total_sectors,                         /* total sectors   */
                             sector_size,                           /* bytes/sector    */
                             sectors_per_cluster,                   /* sectors/cluster */
                             1,                                     /* heads           */
                             1);                                    /* sectors/track   */

    if (status != FX_SUCCESS)
    {
        elog_e(TAG, "Format failed: 0x%02X", status);
        return status;
    }

    elog_i(TAG, "Format complete");
    return FX_SUCCESS;
}

/* ---------- Speed test ----------------------------------------------------- */

#define SPEED_TEST_SIZE         (1 * 1024 * 1024)   /* 1 MB */
#define SPEED_TEST_CHUNK        4096*8                /* write/read chunk size */
#define SPEED_TEST_FILE         "test.txt"

/**
 * @brief  FileX read/write speed test
 *
 *  Creates test.txt, writes 1MB, reads back 1MB, reports speed.
 *  Called after FileX media is ready.
 */
static void app_filex_speed_test(void)
{
    FX_FILE file;
    UINT status;
    static uint8_t buf[SPEED_TEST_CHUNK];
    ULONG bytes_written, bytes_read;
    ULONG total;
    ULONG tick_start, tick_end, ticks;
    uint32_t seed = 0x12345678;

    elog_i(TAG, "=== Speed test: %luKB write + read ===",
           (unsigned long)(SPEED_TEST_SIZE / 1024));

    /* Delete file if it exists */
    fx_file_delete(&nor_media, SPEED_TEST_FILE);

    /* ---- Write test ---- */
    status = fx_file_create(&nor_media, SPEED_TEST_FILE);
    if (status != FX_SUCCESS)
    {
        elog_e(TAG, "Speed: create file failed: 0x%02X", status);
        return;
    }

    status = fx_file_open(&nor_media, &file, SPEED_TEST_FILE, FX_OPEN_FOR_WRITE);
    if (status != FX_SUCCESS)
    {
        elog_e(TAG, "Speed: open file failed: 0x%02X", status);
        return;
    }

    /* Fill buffer with pseudo-random data */
    for (int i = 0; i < SPEED_TEST_CHUNK; i++)
    {
        seed = seed * 1103515245 + 12345;
        buf[i] = (uint8_t)(seed >> 16);
    }

    tick_start = tx_time_get();
    total = 0;

    while (total < SPEED_TEST_SIZE)
    {
        ULONG chunk = SPEED_TEST_CHUNK;
        if (total + chunk > SPEED_TEST_SIZE)
            chunk = SPEED_TEST_SIZE - total;

        bytes_written = 0;
        status = fx_file_write(&file, buf, chunk);
        if (status != FX_SUCCESS)
        {
            elog_e(TAG, "Speed: write failed at %luKB: 0x%02X",
                   (unsigned long)(total / 1024), status);
            break;
        }
        elog_d(TAG, "Speed: wrote %luKB", (unsigned long)(total / 1024));
        total += chunk;
    }

    tick_end = tx_time_get();
    ticks = tick_end - tick_start;
    if (ticks == 0) ticks = 1;

    fx_file_close(&file);

    {
        uint32_t elapsed_ms = ticks * (1000 / TX_TIMER_TICKS_PER_SECOND);
        uint32_t speed_kbs = (total / 1024) * 1000 / elapsed_ms;
        elog_i(TAG, "Speed: WRITE %luKB in %lums = %luKB/s",
               (unsigned long)(total / 1024), (unsigned long)elapsed_ms,
               (unsigned long)speed_kbs);
    }

    /* ---- Read + verify test ---- */
    status = fx_file_open(&nor_media, &file, SPEED_TEST_FILE, FX_OPEN_FOR_READ);
    if (status != FX_SUCCESS)
    {
        elog_e(TAG, "Speed: open for read failed: 0x%02X", status);
        return;
    }

    /* Regenerate expected pattern (same seed as write) */
    static uint8_t expected[SPEED_TEST_CHUNK];
    {
        uint32_t s = 0x12345678;
        for (int i = 0; i < SPEED_TEST_CHUNK; i++)
        {
            s = s * 1103515245 + 12345;
            expected[i] = (uint8_t)(s >> 16);
        }
    }

    tick_start = tx_time_get();
    total = 0;
    uint32_t mismatch_count = 0;
    uint32_t mismatch_offset = 0;

    while (1)
    {
        bytes_read = 0;
        status = fx_file_read(&file, buf, SPEED_TEST_CHUNK, &bytes_read);
        if (status != FX_SUCCESS || bytes_read == 0)
            break;

        /* Verify data */
        for (ULONG i = 0; i < bytes_read; i++)
        {
            if (buf[i] != expected[i])
            {
                if (mismatch_count == 0)
                    mismatch_offset = total + i;
                mismatch_count++;
            }
        }

        total += bytes_read;
    }

    tick_end = tx_time_get();
    ticks = tick_end - tick_start;
    if (ticks == 0) ticks = 1;

    fx_file_close(&file);

    {
        uint32_t elapsed_ms = ticks * (1000 / TX_TIMER_TICKS_PER_SECOND);
        uint32_t speed_kbs = (total / 1024) * 1000 / elapsed_ms;
        elog_i(TAG, "Speed: READ  %luKB in %lums = %luKB/s",
               (unsigned long)(total / 1024), (unsigned long)elapsed_ms,
               (unsigned long)speed_kbs);
    }

    /* Verification result */
    if (mismatch_count == 0)
        elog_i(TAG, "Speed: VERIFY OK — %luKB matched", (unsigned long)(total / 1024));
    else
        elog_e(TAG, "Speed: VERIFY FAILED — %lu mismatches, first at offset 0x%06lX",
               (unsigned long)mismatch_count, (unsigned long)mismatch_offset);

    elog_i(TAG, "=== Speed test done ===");
}

/* ---------- Public API ----------------------------------------------------- */

UINT app_filex_init(void)
{
    UINT status;

    if (filex_initialized)
        return FX_SUCCESS;

    /* Initialize FileX system (build options, timer, media list).
     * Must be called before any fx_media_open/fx_media_format. */
    fx_system_initialize();

    /* Step 1: Initialize the SPI flash hardware */
    if (bsp_spi_flash_init() != 0)
    {
        elog_e(TAG, "SPI flash init failed");
        return FX_NOT_FOUND;
    }

    /* Step 2: Auto-detect flash geometry via SFDP */
    if (lx_nor_custom_driver_sfdp_init() != 0)
    {
        elog_w(TAG, "SFDP detection failed — using defaults");
    }

    elog_i(TAG, "FileX + LevelX init (%luMB flash, %lu-byte blocks)...",
           (unsigned long)(nor_flash_capacity / (1024 * 1024)),
           (unsigned long)nor_block_size);

    /* Step 3: Try to open existing file system */
    status = fx_media_open(&nor_media, "NOR Flash",
                           fx_stm32_levelx_nor_driver,
                           (void *)(ULONG)NOR_CUSTOM_DRIVER_ID,
                           nor_media_memory, FX_MEDIA_MEMORY_SIZE);

    if (status != FX_SUCCESS)
    {
        elog_w(TAG, "Media open failed: 0x%02X — formatting...", status);

        /* Step 4: Flash is blank or corrupted — format and retry */
        status = app_filex_format();
        if (status != FX_SUCCESS)
            return status;

        status = fx_media_open(&nor_media, "NOR Flash",
                               fx_stm32_levelx_nor_driver,
                               (void *)(ULONG)NOR_CUSTOM_DRIVER_ID,
                               nor_media_memory, FX_MEDIA_MEMORY_SIZE);

        if (status != FX_SUCCESS)
        {
            elog_e(TAG, "Media open after format failed: 0x%02X", status);
            return status;
        }
    }

    elog_i(TAG, "FileX init complete — media ready");
    filex_initialized = 1;

    /* Run read/write speed test */
    //app_filex_speed_test();

    return FX_SUCCESS;
}

FX_MEDIA *app_filex_get_media(void)
{
    return filex_initialized ? &nor_media : NULL;
}
