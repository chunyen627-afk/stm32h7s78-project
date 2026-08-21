/**
 * FatFs 磁碟介面，接到 BSP_SD。
 *
 * 為什麼不用韌體包裡的 sd_diskio.c：它綁的是 CubeMX 產生的 hsd1 handle 和
 * MX_SDMMC1_SD_Init()，而我們走 BSP_SD（自帶 MspInit、插卡偵測、卡片資訊），
 * 兩邊的初始化路徑對不上。自己寫這一層只有幾十行，而且可以把整個裝置標成
 * 唯讀 —— 相簿不需要寫卡，從 FatFs 這層就擋掉，使用者的照片不可能被動到。
 *
 * 用的是 BSP_SD_ReadBlocks（輪詢版，內部是 HAL_SD_ReadBlocks 帶 timeout），
 * 不是 DMA 版，所以不需要處理快取一致性。
 */
#include "ff_gen_drv.h"
#include "stm32h7s78_discovery_sd.h"

#include <string.h>

#define SD_INSTANCE     0U
#define SD_WAIT_MS      2000U

static volatile DSTATUS g_stat = STA_NOINIT;

/* 開機卡住時用 SWD 讀，看停在哪一步。 */
volatile uint32_t g_sd_stage;
volatile int32_t  g_sd_init_res;
volatile uint32_t g_sd_reads;

/* HAL_SD_ReadBlocks 要求 32 位元對齊的目的地，但 FatFs 交下來的緩衝區
 * 不保證對齊（f_read 可以直接讀進使用者的任意指標）。不對齊時走這塊中繼。 */
static uint32_t g_bounce[512 / 4];

static DSTATUS sd_wait_ready(void)
{
    uint32_t t0 = HAL_GetTick();
    while (BSP_SD_GetCardState(SD_INSTANCE) != SD_TRANSFER_OK) {
        if (HAL_GetTick() - t0 > SD_WAIT_MS) {
            return STA_NOINIT;
        }
    }
    return 0;
}

static DSTATUS sdbsp_initialize(BYTE lun)
{
    (void)lun;

    g_stat = STA_NOINIT;

    /* 正常路徑直接 Init，失敗才 DeInit 重試。
     *
     * 不要一開始就無條件 DeInit：它沒有解決原本想解決的問題（卡片停在資料
     * 傳輸狀態導致 BSP_SD_Init 卡死）—— 實測卡死時 g_sd_stage 已經是 2，
     * 代表 DeInit 跑完了 Init 照樣出不來。那個情況是靠看門狗兜住的。
     *
     * 但 DeInit 拿來當「重試前先清乾淨」有意義：它會把 SDMMC 斷電、把偵測腳
     * HAL_GPIO_DeInit 掉，讓下一次初始化從乾淨的狀態開始。 */
    g_sd_stage = 1;
    for (uint32_t attempt = 0; attempt < 4u; attempt++) {
        if (attempt > 0u) {
            (void)BSP_SD_DeInit(SD_INSTANCE);
            HAL_Delay(200);             /* 讓偵測腳穩定下來再重設 */
        }
        g_sd_stage = 2;
        g_sd_init_res = BSP_SD_Init(SD_INSTANCE);
        g_sd_stage = 3;
        if (g_sd_init_res == BSP_ERROR_NONE) {
            break;
        }
        HAL_Delay(200);
    }
    if (g_sd_init_res != BSP_ERROR_NONE) {
        return g_stat;
    }

    if (BSP_SD_IsDetected(SD_INSTANCE) != SD_PRESENT) {
        return g_stat;
    }
    g_sd_stage = 4;

    /* STA_PROTECT：已初始化但唯讀。FatFs 之後任何寫入都會直接被擋下。 */
    g_stat = STA_PROTECT;
    return g_stat;
}

static DSTATUS sdbsp_status(BYTE lun)
{
    (void)lun;
    if (BSP_SD_IsDetected(SD_INSTANCE) != SD_PRESENT) {
        g_stat = STA_NOINIT;
    }
    return g_stat;
}

static DRESULT sdbsp_read(BYTE lun, BYTE *buff, DWORD sector, UINT count)
{
    (void)lun;

    if (g_stat & STA_NOINIT) {
        return RES_NOTRDY;
    }

    g_sd_reads++;
    if (((uint32_t)(uintptr_t)buff & 3U) == 0U) {
        if (BSP_SD_ReadBlocks(SD_INSTANCE, (uint32_t *)(uintptr_t)buff,
                              (uint32_t)sector, (uint32_t)count)
            != BSP_ERROR_NONE) {
            return RES_ERROR;
        }
        return sd_wait_ready() ? RES_ERROR : RES_OK;
    }

    /* 不對齊：一次一個磁區搬過去。慢，但幾乎不會走到這條路。 */
    for (UINT i = 0; i < count; i++) {
        if (BSP_SD_ReadBlocks(SD_INSTANCE, g_bounce,
                              (uint32_t)sector + i, 1U) != BSP_ERROR_NONE) {
            return RES_ERROR;
        }
        if (sd_wait_ready()) {
            return RES_ERROR;
        }
        memcpy(buff + i * 512U, g_bounce, 512U);
    }
    return RES_OK;
}

static DRESULT sdbsp_write(BYTE lun, const BYTE *buff, DWORD sector, UINT count)
{
    (void)lun; (void)buff; (void)sector; (void)count;
    /* 相簿只讀。真的有人呼叫到這裡，代表某處邏輯錯了，直接回報寫入保護。 */
    return RES_WRPRT;
}

static DRESULT sdbsp_ioctl(BYTE lun, BYTE cmd, void *buff)
{
    (void)lun;
    BSP_SD_CardInfo info;

    if (g_stat & STA_NOINIT) {
        return RES_NOTRDY;
    }

    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;

    case GET_SECTOR_COUNT:
        if (BSP_SD_GetCardInfo(SD_INSTANCE, &info) != BSP_ERROR_NONE) {
            return RES_ERROR;
        }
        *(DWORD *)buff = info.LogBlockNbr;
        return RES_OK;

    case GET_SECTOR_SIZE:
        if (BSP_SD_GetCardInfo(SD_INSTANCE, &info) != BSP_ERROR_NONE) {
            return RES_ERROR;
        }
        *(WORD *)buff = (WORD)info.LogBlockSize;
        return RES_OK;

    case GET_BLOCK_SIZE:
        if (BSP_SD_GetCardInfo(SD_INSTANCE, &info) != BSP_ERROR_NONE) {
            return RES_ERROR;
        }
        *(DWORD *)buff = info.LogBlockSize / 512U;
        return RES_OK;

    default:
        return RES_PARERR;
    }
}

const Diskio_drvTypeDef SD_BSP_Driver = {
    sdbsp_initialize,
    sdbsp_status,
    sdbsp_read,
    sdbsp_write,
    sdbsp_ioctl,
};
