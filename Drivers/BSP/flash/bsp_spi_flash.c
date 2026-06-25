/**
  ******************************************************************************
  * @file    bsp_spi_flash.c
  * @brief   W25Q128 SPI NOR Flash driver — DMA + RTOS
  ******************************************************************************
  */

#include "bsp_spi_flash.h"
#include "spi.h"
#include "main.h"
#include "tx_api.h"
#include "elog.h"
#include <string.h>

#define TAG "FLASH"

/* ---------- W25Q128 commands ----------------------------------------------- */

#define CMD_WRITE_ENABLE            0x06
#define CMD_WRITE_DISABLE           0x04
#define CMD_READ_STATUS_REG1        0x05
#define CMD_READ_DATA               0x03
#define CMD_PAGE_PROGRAM            0x02
#define CMD_SECTOR_ERASE_4K         0x20
#define CMD_BLOCK_ERASE_32K         0x52
#define CMD_BLOCK_ERASE_64K         0xD8
#define CMD_CHIP_ERASE              0xC7
#define CMD_JEDEC_ID                0x9F
#define CMD_ENABLE_RESET            0x66
#define CMD_RESET                   0x99

/* ---------- RTOS synchronization ------------------------------------------- */

static TX_SEMAPHORE flash_sem;          /* bus lock (mutual exclusion)         */
static TX_SEMAPHORE flash_dma_done;     /* DMA completion signal               */
static uint8_t flash_initialized = 0;

/* ---------- CS control ----------------------------------------------------- */

static inline void cs_low(void)  { HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_RESET); }
static inline void cs_high(void) { HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_SET);   }

/* ---------- DMA completion callbacks --------------------------------------- */

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI1)
        tx_semaphore_put(&flash_dma_done);
}

void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI1)
        tx_semaphore_put(&flash_dma_done);
}

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI1)
        tx_semaphore_put(&flash_dma_done);
}

/**
 * @brief  SPI error callback — reset SPI state to prevent permanent HAL_BUSY
 */
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI1)
    {
        elog_e(TAG, "SPI DMA error: 0x%08lX", (unsigned long)hspi->ErrorCode);
        HAL_SPI_Abort_IT(hspi);
        tx_semaphore_put(&flash_dma_done);
    }
}

/* ---------- Low-level SPI helpers ------------------------------------------ */

/**
 * @brief  Send command then receive data via DMA (BUG FIX #2: loop for >64KB)
 */
static int flash_cmd_read(uint8_t cmd, uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint8_t hdr[4];

    cs_low();

    /* Send command + 3-byte address */
    hdr[0] = cmd;
    hdr[1] = (addr >> 16) & 0xFF;
    hdr[2] = (addr >> 8)  & 0xFF;
    hdr[3] = addr & 0xFF;

    tx_semaphore_get(&flash_dma_done, TX_NO_WAIT);
    if (HAL_SPI_Transmit_DMA(&hspi1, hdr, 4) != HAL_OK) { cs_high(); return -1; }
    if (tx_semaphore_get(&flash_dma_done, 5 * TX_TIMER_TICKS_PER_SECOND) != TX_SUCCESS) { cs_high(); return -2; }

    /* Receive data in chunks (HAL DMA size is uint16_t, max 65535) */
    while (len > 0)
    {
        uint16_t chunk = (len > 65535) ? 65535 : (uint16_t)len;

        tx_semaphore_get(&flash_dma_done, TX_NO_WAIT);
        if (HAL_SPI_Receive_DMA(&hspi1, buf, chunk) != HAL_OK) { cs_high(); return -3; }
        if (tx_semaphore_get(&flash_dma_done, 5 * TX_TIMER_TICKS_PER_SECOND) != TX_SUCCESS) { cs_high(); return -4; }

        buf += chunk;
        len -= chunk;
    }

    cs_high();
    return 0;
}

/**
 * @brief  Send Write Enable command (with error checking)
 * @return 0 on success, negative on error
 */
static int flash_write_enable(void)
{
    uint8_t cmd = CMD_WRITE_ENABLE;

    cs_low();
    tx_semaphore_get(&flash_dma_done, TX_NO_WAIT);
    if (HAL_SPI_Transmit_DMA(&hspi1, &cmd, 1) != HAL_OK) { cs_high(); return -1; }
    if (tx_semaphore_get(&flash_dma_done, TX_TIMER_TICKS_PER_SECOND) != TX_SUCCESS) { cs_high(); return -2; }
    cs_high();
    return 0;
}

/* ---------- Manufacturer ID lookup ----------------------------------------- */

/**
 * @brief  Get manufacturer name from JEDEC ID byte 0
 * @param  mfr  Manufacturer ID (first byte of JEDEC ID)
 * @return Human-readable manufacturer name string
 */
static const char *flash_mfr_name(uint8_t mfr)
{
    switch (mfr)
    {
        case 0xEF: return "Winbond";
        case 0xC8: return "GigaDevice";
        case 0x20: return "Micron/XMC";
        case 0xC2: return "Macronix";
        case 0x1C: return "EON/ESMT";
        case 0x85: return "Puya";
        case 0x01: return "Spansion";
        case 0xBF: return "SST";
        case 0x9D: return "ISSI";
        case 0x0B: return "XTX";
        case 0x68: return "Boya";
        case 0xA1: return "FudanMicro";
        case 0xEB: return "TsingTeng";
        case 0x1F: return "Atmel";
        default:   return "Unknown";
    }
}

/* ---------- Public API ----------------------------------------------------- */

int bsp_spi_flash_init(void)
{
    uint8_t id[3];

    if (flash_initialized) return 0;

    /* Create semaphores */
    tx_semaphore_create(&flash_sem, "flash_sem", 1);
    tx_semaphore_create(&flash_dma_done, "flash_dma", 0);

    /* GPIO pins (CS/WP/HOLD) are initialized in MX_GPIO_Init() */

    /* Reset flash chip */
    {
        uint8_t reset_cmd[2] = { CMD_ENABLE_RESET, CMD_RESET };
        cs_low();
        HAL_SPI_Transmit(&hspi1, reset_cmd, 2, 100);
        cs_high();
        tx_thread_sleep(5);  /* tRST = 30us max */
    }

    /* Read JEDEC ID */
    if (bsp_spi_flash_read_id(id) != 0)
    {
        elog_e(TAG, "Failed to read JEDEC ID");
        return -1;
    }

    elog_i(TAG, "JEDEC ID: %02X %02X %02X — %s",
           id[0], id[1], id[2], flash_mfr_name(id[0]));

    /* Log capacity from ID byte 2.
     * JEDEC encoding: capacity = 2^(id[2]) bits.
     * e.g. 0x18 = 24 → 2^24 = 16,777,216 bits = 128 Mbit = 16 MB.
     * Note: SFDP is authoritative; this is informational only. */
    {
        uint32_t cap_mbit = (1UL << (id[2] - 17));   /* 2^(n-3) Mbit = 2^n bits / 1024 / 1024 */
        elog_i(TAG, "Capacity from ID: %luMbit (%luMB)",
               (unsigned long)cap_mbit,
               (unsigned long)(cap_mbit / 8));
    }

    flash_initialized = 1;
    return 0;
}

int bsp_spi_flash_read_id(uint8_t id[3])
{
    uint8_t cmd = CMD_JEDEC_ID;

    /* BUG FIX #5: use flash_sem for thread safety */
    tx_semaphore_get(&flash_sem, TX_WAIT_FOREVER);

    cs_low();
    tx_semaphore_get(&flash_dma_done, TX_NO_WAIT);
    if (HAL_SPI_Transmit_DMA(&hspi1, &cmd, 1) != HAL_OK) { cs_high(); tx_semaphore_put(&flash_sem); return -1; }
    if (tx_semaphore_get(&flash_dma_done, TX_TIMER_TICKS_PER_SECOND) != TX_SUCCESS) { cs_high(); tx_semaphore_put(&flash_sem); return -2; }

    tx_semaphore_get(&flash_dma_done, TX_NO_WAIT);
    if (HAL_SPI_Receive_DMA(&hspi1, id, 3) != HAL_OK) { cs_high(); tx_semaphore_put(&flash_sem); return -3; }
    if (tx_semaphore_get(&flash_dma_done, TX_TIMER_TICKS_PER_SECOND) != TX_SUCCESS) { cs_high(); tx_semaphore_put(&flash_sem); return -4; }

    cs_high();
    tx_semaphore_put(&flash_sem);
    return 0;
}

int bsp_spi_flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    int rc;
    tx_semaphore_get(&flash_sem, TX_WAIT_FOREVER);
    rc = flash_cmd_read(CMD_READ_DATA, addr, buf, len);
    tx_semaphore_put(&flash_sem);
    return rc;
}

int bsp_spi_flash_write_page(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint8_t hdr[4];
    int rc = 0;

    if (len == 0 || len > W25Q128_PAGE_SIZE) return -1;

    /* BUG FIX #6: enforce page boundary */
    if ((addr % W25Q128_PAGE_SIZE) + len > W25Q128_PAGE_SIZE) return -2;

    tx_semaphore_get(&flash_sem, TX_WAIT_FOREVER);

    /* BUG FIX #4: check Write Enable return value */
    rc = flash_write_enable();
    if (rc != 0) goto out;

    /* Page Program: cmd + 3-byte addr + data */
    cs_low();
    hdr[0] = CMD_PAGE_PROGRAM;
    hdr[1] = (addr >> 16) & 0xFF;
    hdr[2] = (addr >> 8)  & 0xFF;
    hdr[3] = addr & 0xFF;

    tx_semaphore_get(&flash_dma_done, TX_NO_WAIT);
    if (HAL_SPI_Transmit_DMA(&hspi1, hdr, 4) != HAL_OK) { cs_high(); rc = -3; goto out; }
    if (tx_semaphore_get(&flash_dma_done, 5 * TX_TIMER_TICKS_PER_SECOND) != TX_SUCCESS) { cs_high(); rc = -4; goto out; }

    tx_semaphore_get(&flash_dma_done, TX_NO_WAIT);
    if (HAL_SPI_Transmit_DMA(&hspi1, (uint8_t *)buf, len) != HAL_OK) { cs_high(); rc = -5; goto out; }
    if (tx_semaphore_get(&flash_dma_done, 5 * TX_TIMER_TICKS_PER_SECOND) != TX_SUCCESS) { cs_high(); rc = -6; goto out; }

    cs_high();

    /* Wait for write to complete (tPP = 3ms typical, 5ms max per datasheet,
     * but some chips (e.g. Macronix) may take longer in practice) */
    rc = bsp_spi_flash_wait_ready(50);

out:
    tx_semaphore_put(&flash_sem);
    return rc;
}

int bsp_spi_flash_write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    int rc = 0;

    while (len > 0)
    {
        /* Bytes remaining in current page */
        uint32_t page_remain = W25Q128_PAGE_SIZE - (addr % W25Q128_PAGE_SIZE);
        uint32_t chunk = (len < page_remain) ? len : page_remain;

        rc = bsp_spi_flash_write_page(addr, buf, chunk);
        if (rc != 0) return rc;

        addr += chunk;
        buf  += chunk;
        len  -= chunk;
    }
    return 0;
}

int bsp_spi_flash_erase_sector(uint32_t addr)
{
    uint8_t hdr[4];
    int rc = 0;

    tx_semaphore_get(&flash_sem, TX_WAIT_FOREVER);

    /* BUG FIX #4: check Write Enable return value */
    rc = flash_write_enable();
    if (rc != 0) goto out;

    /* Sector Erase: cmd + 3-byte addr */
    cs_low();
    hdr[0] = CMD_SECTOR_ERASE_4K;
    hdr[1] = (addr >> 16) & 0xFF;
    hdr[2] = (addr >> 8)  & 0xFF;
    hdr[3] = addr & 0xFF;

    tx_semaphore_get(&flash_dma_done, TX_NO_WAIT);
    if (HAL_SPI_Transmit_DMA(&hspi1, hdr, 4) != HAL_OK) { cs_high(); rc = -1; goto out; }
    if (tx_semaphore_get(&flash_dma_done, 5 * TX_TIMER_TICKS_PER_SECOND) != TX_SUCCESS) { cs_high(); rc = -2; goto out; }

    cs_high();

    /* Wait for erase to complete (tSE = 50ms typical, 400ms max) */
    rc = bsp_spi_flash_wait_ready(1000);

out:
    tx_semaphore_put(&flash_sem);
    return rc;
}

int bsp_spi_flash_erase_block64k(uint32_t addr)
{
    uint8_t hdr[4];
    int rc = 0;

    tx_semaphore_get(&flash_sem, TX_WAIT_FOREVER);

    /* BUG FIX #4: check Write Enable return value */
    rc = flash_write_enable();
    if (rc != 0) goto out;

    /* 64KB Block Erase */
    cs_low();
    hdr[0] = CMD_BLOCK_ERASE_64K;
    hdr[1] = (addr >> 16) & 0xFF;
    hdr[2] = (addr >> 8)  & 0xFF;
    hdr[3] = addr & 0xFF;

    tx_semaphore_get(&flash_dma_done, TX_NO_WAIT);
    if (HAL_SPI_Transmit_DMA(&hspi1, hdr, 4) != HAL_OK) { cs_high(); rc = -1; goto out; }
    if (tx_semaphore_get(&flash_dma_done, 5 * TX_TIMER_TICKS_PER_SECOND) != TX_SUCCESS) { cs_high(); rc = -2; goto out; }

    cs_high();

    /* Wait for erase to complete (tBE = 150ms typical, 2s max) */
    rc = bsp_spi_flash_wait_ready(3000);

out:
    tx_semaphore_put(&flash_sem);
    return rc;
}

int bsp_spi_flash_erase_chip(void)
{
    int rc = 0;

    tx_semaphore_get(&flash_sem, TX_WAIT_FOREVER);

    /* BUG FIX #4: check Write Enable return value */
    rc = flash_write_enable();
    if (rc != 0) goto out;

    /* Chip Erase */
    cs_low();
    {
        uint8_t cmd = CMD_CHIP_ERASE;
        tx_semaphore_get(&flash_dma_done, TX_NO_WAIT);
        if (HAL_SPI_Transmit_DMA(&hspi1, &cmd, 1) != HAL_OK) { cs_high(); rc = -1; goto out; }
        if (tx_semaphore_get(&flash_dma_done, TX_TIMER_TICKS_PER_SECOND) != TX_SUCCESS) { cs_high(); rc = -2; goto out; }
    }
    cs_high();

    /* Wait for erase to complete (tCE = 25s typical, 100s max) */
    rc = bsp_spi_flash_wait_ready(120000);

out:
    tx_semaphore_put(&flash_sem);
    return rc;
}

int bsp_spi_flash_wait_ready(uint32_t timeout_ms)
{
    /* BUG FIX #3: use tx_time_get() for correct timing */
    ULONG start = tx_time_get();
    ULONG timeout_ticks = timeout_ms * TX_TIMER_TICKS_PER_SECOND / 1000;
    if (timeout_ticks == 0) timeout_ticks = 1;

    while ((tx_time_get() - start) < timeout_ticks)
    {
        uint8_t cmd = CMD_READ_STATUS_REG1;
        uint8_t sr = 0;

        cs_low();
        tx_semaphore_get(&flash_dma_done, TX_NO_WAIT);
        HAL_SPI_Transmit_DMA(&hspi1, &cmd, 1);
        tx_semaphore_get(&flash_dma_done, TX_TIMER_TICKS_PER_SECOND);

        tx_semaphore_get(&flash_dma_done, TX_NO_WAIT);
        HAL_SPI_Receive_DMA(&hspi1, &sr, 1);
        tx_semaphore_get(&flash_dma_done, TX_TIMER_TICKS_PER_SECOND);
        cs_high();

        if (!(sr & W25Q128_SR_BUSY))
            return 0;

        tx_thread_sleep(5);
    }

    elog_e(TAG, "Flash busy timeout (%lums)", (unsigned long)timeout_ms);
    return -1;
}

/* ---------- SFDP (JESD216) ------------------------------------------------- */

#define CMD_READ_SFDP               0x5A

#define SFDP_SIGNATURE              0x50444653  /* "SFDP" */

/**
 * @brief  Read raw SFDP data from flash
 * @param  addr  Byte address within SFDP space (0-based)
 * @param  buf   Destination buffer
 * @param  len   Number of bytes to read
 * @return 0 on success
 */
static int sfdp_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint8_t hdr[5];

    if (len == 0) return 0;

    cs_low();

    /* Command + 3-byte address + 1 dummy byte — sent as one DMA burst.
     * Splitting cmd+addr and dummy into two DMA transfers causes
     * the GD25Q128 to return stale data on subsequent reads. */
    hdr[0] = CMD_READ_SFDP;
    hdr[1] = (addr >> 16) & 0xFF;
    hdr[2] = (addr >> 8)  & 0xFF;
    hdr[3] = addr & 0xFF;
    hdr[4] = 0xFF;  /* 8 dummy clocks */

    tx_semaphore_get(&flash_dma_done, TX_NO_WAIT);
    if (HAL_SPI_Transmit_DMA(&hspi1, hdr, 5) != HAL_OK) { cs_high(); return -1; }
    if (tx_semaphore_get(&flash_dma_done, 5 * TX_TIMER_TICKS_PER_SECOND) != TX_SUCCESS) { cs_high(); return -2; }

    /* Receive SFDP data in chunks */
    while (len > 0)
    {
        uint16_t chunk = (len > 65535) ? 65535 : (uint16_t)len;

        tx_semaphore_get(&flash_dma_done, TX_NO_WAIT);
        if (HAL_SPI_Receive_DMA(&hspi1, buf, chunk) != HAL_OK) { cs_high(); return -5; }
        if (tx_semaphore_get(&flash_dma_done, 5 * TX_TIMER_TICKS_PER_SECOND) != TX_SUCCESS) { cs_high(); return -6; }

        buf += chunk;
        len -= chunk;
    }

    cs_high();
    return 0;
}


/**
 * @brief  Parse erase types from SFDP basic parameter table (SFUD approach)
 *
 *  Per JESD216F §6.4.18, each erase type is 2 bytes:
 *    byte 0 = erase size exponent (size = 2^N bytes, 0 = not supported)
 *    byte 1 = erase command opcode
 *
 *  SFUD: table[28+2*i] is size exponent, table[28+2*i+1] is erase command.
 *  We collect up to 4 erase types, sort by size, and fill geo fields.
 */
#define SFDP_ERASE_TYPE_MAX     4

static void sfdp_parse_erase_types(const uint8_t *table, bsp_flash_geometry_t *geo)
{
    uint32_t sizes[SFDP_ERASE_TYPE_MAX] = {0};
    uint8_t  cmds[SFDP_ERASE_TYPE_MAX]  = {0};
    int count = 0;

    /* Collect valid erase types (size exponent != 0) */
    for (int i = 0; i < SFDP_ERASE_TYPE_MAX && count < SFDP_ERASE_TYPE_MAX; i++)
    {
        uint8_t size_exp = table[28 + 2 * i];
        if (size_exp != 0)
        {
            sizes[count] = 1UL << size_exp;
            cmds[count]  = table[28 + 2 * i + 1];
            elog_d(TAG, "SFDP: erase type %d: %luKB cmd=0x%02X",
                   count + 1, (unsigned long)(sizes[count] / 1024), cmds[count]);
            count++;
        }
    }

    /* Sort by size (bubble sort, max 4 entries) */
    for (int i = 0; i < count - 1; i++)
    {
        for (int j = i + 1; j < count; j++)
        {
            if (sizes[j] < sizes[i])
            {
                uint32_t ts = sizes[i]; sizes[i] = sizes[j]; sizes[j] = ts;
                uint8_t  tc = cmds[i];  cmds[i]  = cmds[j];  cmds[j]  = tc;
            }
        }
    }

    /* Smallest erase type → sector_size */
    if (count > 0)
        geo->sector_size = sizes[0];

    /* Map remaining erase types to block_size_32k / block_size_64k */
    for (int i = 1; i < count; i++)
    {
        if (sizes[i] == 32 * 1024)
            geo->block_size_32k = sizes[i];
        else if (sizes[i] == 64 * 1024)
            geo->block_size_64k = sizes[i];
    }
}

int bsp_spi_flash_read_sfdp(bsp_flash_geometry_t *geo)
{
    /*
     * SFDP parsing based on SFUD (Serial Flash Universal Driver) approach.
     * Reads header, parameter header, and basic table as separate SPI
     * transactions — each with its own CS cycle — matching how SFUD's
     * read_sfdp_data() works.  The PTP from the parameter header is used
     * directly as the byte address for the basic table.
     */
    uint8_t hdr[8];         /* SFDP header (1 DWORD) */
    uint8_t param[8];       /* first parameter header (2 DWORDs) */
    uint8_t table[9 * 4];   /* basic flash parameter table (9 DWORDs) */
    uint32_t ptp;
    int rc = 0;

    memset(geo, 0, sizeof(*geo));

    tx_semaphore_get(&flash_sem, TX_WAIT_FOREVER);

    /* ---- Step 1: Read SFDP header (address 0, 8 bytes) ---- */
    if (sfdp_read(0, hdr, sizeof(hdr)) != 0)
    {
        elog_e(TAG, "SFDP: read header failed");
        rc = -1;
        goto out;
    }

    elog_hexdump(TAG, 16, hdr, sizeof(hdr));

    if (!(hdr[0] == 'S' && hdr[1] == 'F' && hdr[2] == 'D' && hdr[3] == 'P'))
    {
        elog_e(TAG, "SFDP: bad signature %02X%02X%02X%02X (expected SFDP)",
               hdr[3], hdr[2], hdr[1], hdr[0]);
        rc = -2;
        goto out;
    }

    {
        uint8_t major = hdr[5];
        uint8_t minor = hdr[4];
        elog_i(TAG, "SFDP: v%d.%d, %d param header(s)", major, minor, hdr[6] + 1);

        if (major > 1)
        {
            elog_e(TAG, "SFDP: v%d.%d not supported (max v1.x)", major, minor);
            rc = -3;
            goto out;
        }
    }

    /* ---- Step 2: Read first parameter header (address 8, 8 bytes) ---- */
    if (sfdp_read(8, param, sizeof(param)) != 0)
    {
        elog_e(TAG, "SFDP: read param header failed");
        rc = -4;
        goto out;
    }

    elog_hexdump(TAG, 16, param, sizeof(param));

    /*
     * JESD216F §6.2: parameter header layout
     *   byte 0 = Parameter ID LSB
     *   byte 1 = Parameter minor revision
     *   byte 2 = Parameter major revision
     *   byte 3 = Parameter table length (in DWORDs)
     *   byte 4 = PTP[7:0]
     *   byte 5 = PTP[15:8]
     *   byte 6 = PTP[23:16]
     *   byte 7 = reserved
     */
    {
        uint8_t param_major = param[2];
        uint8_t param_len   = param[3];  /* table length in DWORDs */

        /* JESD216: PTP is 3-byte little-endian */
        ptp = (uint32_t)param[4]
            | ((uint32_t)param[5] << 8)
            | ((uint32_t)param[6] << 16);

        elog_i(TAG, "SFDP: PTP=0x%06lX, len=%d DWORDs, v%d.%d",
               (unsigned long)ptp, param_len, param_major, param[1]);

        if (param_major > 1)
        {
            elog_e(TAG, "SFDP: param header v%d.%d not supported", param_major, param[1]);
            rc = -5;
            goto out;
        }

        if (param_len < 9)
        {
            elog_e(TAG, "SFDP: basic table too short (%d DWORDs, need >= 9)", param_len);
            rc = -6;
            goto out;
        }
    }

    /* ---- Step 3: Read basic parameter table at PTP (9 DWORDs = 36 bytes) ---- */
    if (sfdp_read(ptp, table, sizeof(table)) != 0)
    {
        elog_e(TAG, "SFDP: read basic table at 0x%06lX failed", (unsigned long)ptp);
        rc = -7;
        goto out;
    }

    elog_hexdump(TAG, 16, table, sizeof(table));

    /* Check if basic table data is valid (not all 0xFF).
     * Some chips have SFDP headers but the basic table area is erased. */
    if (table[0] == 0xFF && table[1] == 0xFF &&
        table[2] == 0xFF && table[3] == 0xFF)
    {
        uint8_t id[3];
        elog_w(TAG, "SFDP: basic table at 0x%06lX is all 0xFF, using JEDEC ID",
               (unsigned long)ptp);

        if (bsp_spi_flash_read_id(id) == 0 && id[2] >= 0x14)
        {
            geo->capacity_bytes = 1UL << id[2];
            geo->sector_size    = 4096;
            geo->page_size      = 256;
            geo->sector_count   = geo->capacity_bytes / geo->sector_size;
            geo->sfdp_valid     = 1;
            elog_i(TAG, "SFDP: JEDEC fallback: %luMB, sector=%lu, page=%lu",
                   (unsigned long)(geo->capacity_bytes / (1024 * 1024)),
                   (unsigned long)geo->sector_size,
                   (unsigned long)geo->page_size);
        }
        rc = geo->sfdp_valid ? 0 : -7;
        goto out;
    }

    /* ---- Step 4: Parse basic parameter table (SFUD approach) ---- */

    /* DWORD 1 (bytes 0..3): erase_4k cmd + support, write granularity */
    {
        uint8_t erase_4k_support = table[0] & 0x03;
        if (erase_4k_support == 1)
            elog_d(TAG, "SFDP: 4KB erase supported, cmd=0x%02X", table[1]);
        else if (erase_4k_support == 3)
            elog_d(TAG, "SFDP: 4KB erase not supported");
        else
            elog_w(TAG, "SFDP: 4KB erase field=0x%02X (unexpected)", erase_4k_support);
    }

    /* DWORD 2 (bytes 4..7): flash density */
    {
        uint32_t dword2 = (uint32_t)table[4]
                        | ((uint32_t)table[5] << 8)
                        | ((uint32_t)table[6] << 16)
                        | ((uint32_t)table[7] << 24);
        if (dword2 & 0x80000000)
        {
            uint32_t density_bits = dword2 & 0x7FFFFFFF;
            if (density_bits > 32)
            {
                elog_e(TAG, "SFDP: density %lu bits > 32, unsupported", (unsigned long)density_bits);
                rc = -8;
                goto out;
            }
            geo->capacity_bytes = 1UL << (density_bits - 3);
        }
        else
        {
            geo->capacity_bytes = (dword2 + 1) / 8;
        }

        if (geo->capacity_bytes == 0)
        {
            elog_e(TAG, "SFDP: capacity is 0");
            rc = -9;
            goto out;
        }

        elog_i(TAG, "SFDP: capacity=%luMB", (unsigned long)(geo->capacity_bytes / (1024 * 1024)));
    }

    /* DWORD 7 (bytes 24..27): page size & sector size exponents (JESD216F) */
    {
        uint32_t dword7 = (uint32_t)table[24]
                        | ((uint32_t)table[25] << 8)
                        | ((uint32_t)table[26] << 16)
                        | ((uint32_t)table[27] << 24);
        uint32_t page_exp   = (dword7 >> 8) & 0x1F;
        uint32_t sector_exp = dword7 & 0x1F;

        /* Sanity: valid page size exponents are 8..12 (256B..4KB).
         * exp=0 means not reported; exp>16 is corrupted data (e.g. 0xFF→31). */
        geo->page_size   = (page_exp >= 8 && page_exp <= 12) ? (1UL << page_exp) : 256;
        geo->sector_size = sector_exp ? (1UL << sector_exp) : 0;
    }

    /* DWORD 8-11 (bytes 28..35): erase types 1-4 (SFUD-style parsing) */
    sfdp_parse_erase_types(table, geo);

    /* If no valid erase types were found, the SFDP table data is unreliable.
     * Fall back to JEDEC ID for geometry (capacity + default sector/page). */
    if (geo->sector_size == 0)
    {
        uint8_t id[3];
        elog_w(TAG, "SFDP: no valid erase types, table data unreliable — using JEDEC ID");

        if (bsp_spi_flash_read_id(id) == 0 && id[2] >= 0x14)
        {
            geo->capacity_bytes = 1UL << id[2];
            geo->sector_size    = 4096;
            geo->page_size      = 256;
            geo->sector_count   = geo->capacity_bytes / geo->sector_size;
        }
        else
        {
            /* Last resort: use the capacity from SFDP, default sector */
            geo->sector_size  = 4096;
            geo->page_size    = 256;
            geo->sector_count = geo->capacity_bytes / geo->sector_size;
        }
    }

    geo->sector_count = geo->capacity_bytes / geo->sector_size;
    geo->sfdp_valid   = 1;

    elog_i(TAG, "SFDP: capacity=%luMB, sector=%lu, page=%lu, 32k=%lu, 64k=%lu",
           (unsigned long)(geo->capacity_bytes / (1024 * 1024)),
           (unsigned long)geo->sector_size,
           (unsigned long)geo->page_size,
           (unsigned long)geo->block_size_32k,
           (unsigned long)geo->block_size_64k);

out:
    tx_semaphore_put(&flash_sem);
    return rc;
}
