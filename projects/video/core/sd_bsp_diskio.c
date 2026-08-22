/**
 * FatFs 磁碟介面，接到 BSP_SD。影片專案版本。
 *
 * 跟相簿那份（projects/album/core/sd_bsp_diskio.c）幾乎相同，差別只有一個：
 * **這份支援寫入，但預設仍然是鎖住的**。
 *
 * 為什麼要能寫：電腦上沒有讀卡機，影片檔要靠 ST-LINK 經 SWD 送進板子、
 * 再由韌體寫進 SD 卡。
 *
 * 為什麼預設鎖住：使用者的 SD 卡裡有相簿的照片。播放路徑完全不需要寫入，
 * 所以預設掛成 STA_PROTECT —— FatFs 在這一層就會擋掉任何寫入，
 * 播放模式下就算程式有 bug 也動不到卡片內容。
 * 只有 SD 燒錄模式會呼叫 sd_bsp_unlock_write()，而那個模式必須由 SWD 寫入
 * 一個 magic 才會進去（board-notes 16.3：實驗的副作用要由外部旗標觸發）。
 *
 * 為什麼不直接共用相簿那份：相簿是正在用的正式路徑，為了影片去改它的
 * 磁碟介面等於拿使用者的相簿做實驗。複製一份一百行的檔案便宜得多。
 *
 * 用的是 BSP_SD_ReadBlocks / WriteBlocks（輪詢版，內部是 HAL_SD_* 帶 timeout），
 * 不是 DMA 版，所以不需要處理快取一致性。
 */
#include "ff_gen_drv.h"
#include "stm32h7s78_discovery_sd.h"

#include <string.h>

#define SD_INSTANCE     0U
#define SD_WAIT_MS      2000U

/* 解鎖用的 magic。用特定值而不是 0/1，避免未初始化的記憶體剛好等於 1。 */
#define SD_WRITE_MAGIC  0x57524954u     /* 'WRIT' */

static volatile DSTATUS g_stat = STA_NOINIT;
static volatile uint32_t g_allow_write;

/* 開機卡住時用 SWD 讀，看停在哪一步。 */
volatile uint32_t g_sd_stage;
volatile int32_t  g_sd_init_res;
volatile uint32_t g_sd_reads;
volatile uint32_t g_sd_writes;

/* 寫入失敗時的現場。FatFs 把所有磁碟錯誤都壓成 FR_DISK_ERR(1)，
 * 光看那個數字完全不知道是哪一次、哪個磁區、什麼原因 —— 這幾個變數
 * 就是把「1」還原成可以行動的資訊。 */
volatile int32_t  g_sd_werr;        /* 最後一次失敗的 BSP 回傳值 */
volatile uint32_t g_sd_wsector;     /* 失敗的起始磁區 */
volatile uint32_t g_sd_wcount;      /* 那次要寫幾個磁區 */
volatile uint32_t g_sd_wtimeout;    /* sd_wait_ready 逾時的次數 */
volatile uint32_t g_sd_halerr;      /* HAL_SD_GetError */
volatile uint32_t g_sd_cardstate;   /* 失敗當下的卡片狀態 */
volatile uint32_t g_sd_halstate;    /* hsd.State：1=READY 才收得下新指令 */
volatile uint32_t g_sd_aborts;      /* 用 Abort 把狀態機救回來的次數 */

/* HAL_SD_*Blocks 要求 32 位元對齊的緩衝區，但 FatFs 交下來的不保證對齊
 * （f_read/f_write 可以直接用使用者的任意指標）。不對齊時走這塊中繼。 */
static uint32_t g_bounce[512 / 4];

/* 必須在 f_mount 之前呼叫，否則掛載時就已經標成唯讀了。 */
void sd_bsp_unlock_write(void)
{
    g_allow_write = SD_WRITE_MAGIC;
}

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

    /* 正常路徑直接 Init，失敗才 DeInit 重試。DeInit 會把 SDMMC 斷電、
     * 把偵測腳 HAL_GPIO_DeInit 掉，讓下一次從乾淨的狀態開始。 */
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

    /* 沒解鎖就標成 STA_PROTECT：已初始化但唯讀，FatFs 之後任何寫入都會被擋。 */
    g_stat = (g_allow_write == SD_WRITE_MAGIC) ? 0 : STA_PROTECT;
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

/* 寫一批磁區，失敗時記下現場並嘗試把週邊救回來。
 *
 * 實測到的失敗長相：`BSP_SD_WriteBlocks` 回 -4（PERIPH_FAILURE），
 * 但 `HAL_SD_GetError` 是 **0** —— 沒有任何硬體錯誤被記錄。
 * 這個組合只有一個解釋：HAL 在 `hsd->State != READY` 時直接回傳 HAL_BUSY，
 * 連暫存器都沒碰。也就是**卡住的是軟體狀態機，不是卡片**。
 *
 * 跟 board-notes 16.8（JPEG 週邊累積狀態、Abort 清不掉要 DeInit+Init）
 * 與 16.10（DMA2D 的 State 停在 BUSY）是同一個家族的坑：HAL 的狀態欄位
 * 沒有回到 READY，之後每一次呼叫都被擋在門口，而且**回傳值裡沒有錯誤碼**。
 *
 * 解法分兩段，由輕到重：
 *   1. HAL_SD_Abort()：送 CMD12 結束傳輸並把 State 設回 READY
 *   2. 還是不行就等卡片自己好，再試
 */
static DRESULT sd_write_one(const uint32_t *data, uint32_t sector, uint32_t count)
{
    for (uint32_t attempt = 0; attempt < 4u; attempt++) {
        int32_t rc;

        /* 前一次操作沒結束就送下一個指令，卡片會直接拒絕。 */
        if (sd_wait_ready() != 0) {
            g_sd_wtimeout++;
        }

        rc = BSP_SD_WriteBlocks(SD_INSTANCE, (uint32_t *)(uintptr_t)data,
                                sector, count);
        if (rc == BSP_ERROR_NONE) {
            if (sd_wait_ready() == 0) {
                return RES_OK;
            }
            g_sd_wtimeout++;
            continue;
        }

        g_sd_werr      = rc;
        g_sd_wsector   = sector;
        g_sd_wcount    = count;
        g_sd_halerr    = HAL_SD_GetError(&hsd_sdmmc[SD_INSTANCE]);
        g_sd_halstate  = (uint32_t)hsd_sdmmc[SD_INSTANCE].State;
        g_sd_cardstate = (uint32_t)BSP_SD_GetCardState(SD_INSTANCE);

        /* State 不是 READY 就是被卡住了，Abort 是把它收回來的正規手段。 */
        if (hsd_sdmmc[SD_INSTANCE].State != HAL_SD_STATE_READY) {
            g_sd_aborts++;
            (void)HAL_SD_Abort(&hsd_sdmmc[SD_INSTANCE]);
        }
        HAL_Delay(5);
    }
    return RES_ERROR;
}

static DRESULT sdbsp_write(BYTE lun, const BYTE *buff, DWORD sector, UINT count)
{
    (void)lun;

    if (g_stat & STA_NOINIT) {
        return RES_NOTRDY;
    }
    /* 雙重保險：FatFs 看 STA_PROTECT 就不會走到這裡，這裡再擋一次。
     * 播放模式下這個回傳值等於「卡片是唯讀的」，不會有任何寫入發生。 */
    if (g_allow_write != SD_WRITE_MAGIC) {
        return RES_WRPRT;
    }

    g_sd_writes++;
    if (((uint32_t)(uintptr_t)buff & 3U) == 0U) {
        return sd_write_one((const uint32_t *)(uintptr_t)buff,
                            (uint32_t)sector, (uint32_t)count);
    }

    for (UINT i = 0; i < count; i++) {
        memcpy(g_bounce, buff + i * 512U, 512U);
        if (sd_write_one(g_bounce, (uint32_t)sector + i, 1U) != RES_OK) {
            return RES_ERROR;
        }
    }
    return RES_OK;
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
