/**
  ******************************************************************************
  * @file    app_filex.h
  * @brief   FileX application layer — NOR flash file system
  ******************************************************************************
  */

#ifndef APP_FILEX_H
#define APP_FILEX_H

#ifdef __cplusplus
extern "C" {
#endif

#include "fx_api.h"

/* ---------- Configuration -------------------------------------------------- */

/** FileX media memory size (sector cache).
 *  Larger cache = fewer flash accesses = faster I/O.
 *  Must be >= sector size (512) and a multiple of sector size.
 *  16KB caches 32 sectors — good balance of RAM usage vs write coalescing. */
#define FX_MEDIA_MEMORY_SIZE        (512 * 32)  /* 16KB — caches 32 sectors */

/** NOR flash sector size for FileX (must match LevelX sector size) */
#define FX_NOR_SECTOR_SIZE          512

/* ---------- Public API ----------------------------------------------------- */

/**
 * @brief  Initialize FileX system and open NOR flash media
 *         Call after bsp_spi_flash_init() and ThreadX start.
 * @return FX_SUCCESS on success
 */
UINT app_filex_init(void);

/**
 * @brief  Get pointer to the NOR flash media instance
 * @return Pointer to FX_MEDIA, or NULL if not initialized
 */
FX_MEDIA *app_filex_get_media(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_FILEX_H */
