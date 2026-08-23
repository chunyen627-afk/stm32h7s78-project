/**
 * FatFs 磁碟介面，接到 BSP_SD。
 *
 * 為什麼不用韌體包裡的 sd_diskio.c：它綁的是 CubeMX 產生的 hsd1 handle 和
 * MX_SDMMC1_SD_Init()，而我們走 BSP_SD（自帶 MspInit、插卡偵測、卡片資訊），
 * 兩邊的初始化路徑對不上。自己寫這一層只有幾十行，而且可以把整個裝置標成
 * 唯讀 —— 相簿不需要寫卡，從 FatFs 這層就擋掉，使用者的照片不可能被動到。
 *
 * 用的是 BSP_SD_ReadBlocks / WriteBlocks（輪詢版，內部是 HAL_SD_* 帶 timeout），
 * 不是 DMA 版，所以不需要處理快取一致性。
 *
 * ── 寫入：預設鎖住，只為了「我的最愛」而開 ──────────────────
 * 相簿的播放路徑完全不需要寫卡，所以預設仍然掛成 STA_PROTECT ——
 * FatFs 在這一層就擋掉一切寫入，播放模式下就算程式有 bug 也動不到照片。
 *
 * 只有 favorites.c 會呼叫 sd_bsp_unlock_write()，而且用完立刻
 * sd_bsp_lock_write() 收回去（做法見 favorites.c 的檔頭）。
 *
 * 關鍵限制：**g_stat 是在 disk_initialize 當下決定的**，所以解鎖必須發生在
 * f_mount 之前，掛載完再解鎖沒有用。
 *
 * 寫入實作是從 projects/video/core/sd_bsp_diskio.c 移植過來的，那份已經
 * 靠 SD 燒錄模式實測過 122MB 覆寫零錯誤，重試與 Abort 救援都在裡面。
 */
#include "ff_gen_drv.h"
#include "stm32h7s78_discovery_sd.h"

#include <stdbool.h>
#include <string.h>

#define SD_INSTANCE     0U
#define SD_WAIT_MS      2000U

/* 寫入之後要等的時間比讀取長很多。
 *
 * 便宜的 SD 卡在內部做垃圾回收／抹除區塊搬移時，單次寫入的完成時間偶爾會
 * 到好幾秒。2 秒就放棄的話，我們會在卡片還忙著的時候送下一個指令 ——
 * 實測就是這樣一路壞下去：一次寫入逾時之後，連讀取都跟著失敗，
 * 要整個板子重置才回得來。 */
#define SD_WRITE_WAIT_MS 8000U

/* 解鎖用的 magic。用特定值而不是 0/1，避免未初始化的記憶體剛好等於 1。 */
#define SD_WRITE_MAGIC  0x57524954u     /* 'WRIT' */

static volatile DSTATUS  g_stat = STA_NOINIT;
static volatile uint32_t g_allow_write;

/* 開機卡住時用 SWD 讀，看停在哪一步。 */
volatile uint32_t g_sd_stage;
volatile int32_t  g_sd_init_res;
volatile uint32_t g_sd_reads;
volatile uint32_t g_sd_writes;

/* 寫入失敗時的現場。FatFs 把所有磁碟錯誤都壓成 FR_DISK_ERR(1)，
 * 光看那個數字完全不知道是哪一次、哪個磁區、什麼原因。 */
volatile int32_t  g_sd_werr;        /* 最後一次失敗的 BSP 回傳值 */
volatile uint32_t g_sd_wsector;     /* 失敗的起始磁區 */
volatile uint32_t g_sd_wcount;      /* 那次要寫幾個磁區 */
volatile uint32_t g_sd_wtimeout;    /* sd_wait_ready 逾時的次數 */
volatile uint32_t g_sd_halerr;      /* HAL_SD_GetError */
volatile uint32_t g_sd_cardstate;   /* 失敗當下的卡片狀態 */
volatile uint32_t g_sd_halstate;    /* hsd.State：1=READY 才收得下新指令 */
volatile uint32_t g_sd_aborts;      /* 用 Abort 把狀態機救回來的次數 */

/* 解鎖／上鎖**不需要重新掛載**。
 *
 * FatFs 的 mount_volume() 在 volume 已掛載時仍然每次都呼叫 disk_status()
 * 並重新檢查 STA_PROTECT（ff.c:3416-3419），所以 sdbsp_status() 把保護位元
 * 重算一次就會立刻生效。
 *
 * 第一版是照影片專案那樣「卸載 -> 解鎖 -> 掛載」，結果寫入失敗時連帶
 * 掛不回去，整個檔案系統就沒了 —— 相簿之後每次 f_open 都失敗。
 * 不卸載就沒有這個失敗模式。 */
void sd_bsp_unlock_write(void)
{
    g_allow_write = SD_WRITE_MAGIC;
}

void sd_bsp_lock_write(void)
{
    g_allow_write = 0u;
}

/* 等待卡片的迴圈可能長達數秒，而相簿有 16 秒的獨立看門狗。
 * 沒有人在這裡餵狗的話，一次寫入逾時就會把板子重開 —— 實測就是這樣：
 * 卡片不接受寫入 -> sd_write_one 重試 -> 超過 16 秒 -> IWDG reset ->
 * 開機偵測到 IWDGRST 停在「請拔出記憶卡再重新插入」。 */
static void (*g_keepalive)(void);

void sd_bsp_set_keepalive(void (*fn)(void))
{
    g_keepalive = fn;
}

static inline void keepalive(void)
{
    if (g_keepalive != NULL) {
        g_keepalive();
    }
}

/* HAL_SD_ReadBlocks 要求 32 位元對齊的目的地，但 FatFs 交下來的緩衝區
 * 不保證對齊（f_read 可以直接讀進使用者的任意指標）。不對齊時走這塊中繼。 */
static uint32_t g_bounce[512 / 4];

/* 等卡片回到 TRANSFER_OK。
 *
 * 兩件事以前做錯了：
 *
 * 1. **輪詢之間沒有延遲。** BSP_SD_GetCardState 每次都送一個 CMD13
 *    （SEND_STATUS），全速空轉等於在卡片做內部寫入時瘋狂灌指令。
 *    加 1ms 間隔是標準做法，也大幅減少匯流排上的雜訊。
 * 2. **讀寫用同一個逾時。** 寫入的完成時間可以比讀取長一個數量級，
 *    見 SD_WRITE_WAIT_MS 的說明。 */
static DSTATUS sd_wait_ready_ms(uint32_t limit_ms)
{
    uint32_t t0 = HAL_GetTick();

    while (BSP_SD_GetCardState(SD_INSTANCE) != SD_TRANSFER_OK) {
        keepalive();                /* 這裡不餵狗，寫入逾時就會變成重開機 */
        if (HAL_GetTick() - t0 > limit_ms) {
            return STA_NOINIT;
        }
        HAL_Delay(1);               /* 不要全速灌 CMD13 */
    }
    return 0;
}

static DSTATUS sd_wait_ready(void)
{
    return sd_wait_ready_ms(SD_WAIT_MS);
}

/* 這裡曾經有一個 g_card_up 捷徑，讓卡片還在線時跳過 BSP_SD_Init。
 *
 * 它是為了「解鎖寫入要重新掛載」而加的 —— 但那套機制後來確認根本不需要
 * （FatFs 每次寫入操作都會重新檢查 STA_PROTECT，見上面），捷徑就變成純粹
 * 的遺留品，而且會出錯：拔卡期間 sdbsp_status() 不保證會被呼叫到
 * （相簿的 sd_present() 直接走 BSP，不經過 FatFs），旗標就停在「還在線」，
 * 插回來時跳過完整初始化，f_mount 直接回 FR_DISK_ERR。
 *
 * 現在 disk_initialize 只在真正需要的時候被呼叫（開機、熱插拔），
 * 每次都做完整初始化才是對的。 */
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

    /* 沒解鎖就標成 STA_PROTECT：已初始化但唯讀，FatFs 之後任何寫入都會被擋。
     * 解鎖只發生在 favorites.c 的寫入視窗內，而且掛載完就立刻收回。 */
    g_stat = (g_allow_write == SD_WRITE_MAGIC) ? 0 : STA_PROTECT;
    return g_stat;
}

static DSTATUS sdbsp_status(BYTE lun)
{
    (void)lun;
    if (BSP_SD_IsDetected(SD_INSTANCE) != SD_PRESENT) {
        g_stat = STA_NOINIT;
        return g_stat;
    }
    /* 保護位元每次都重算。FatFs 在每個需要寫入的操作前都會走到這裡
     * （ff.c:3417），所以 favorites.c 解鎖之後不必重新掛載就會生效。 */
    if ((g_stat & STA_NOINIT) == 0) {
        g_stat = (g_allow_write == SD_WRITE_MAGIC) ? 0 : STA_PROTECT;
    }
    return g_stat;
}

static DRESULT sdbsp_read(BYTE lun, BYTE *buff, DWORD sector, UINT count)
{
    (void)lun;

    if (g_stat & STA_NOINIT) {
        return RES_NOTRDY;
    }

    /* 讀取的**失敗**路徑以前沒有餵狗：BSP_SD_ReadBlocks 一失敗就直接 return，
     * 完全不會走到 sd_wait_ready()。卡片進 BUSY 之後每一次讀取都是這條路，
     * 上層又會把整份清單重試一遍 —— 16 秒的看門狗就把板子打掉了，
     * 開機看到 IWDGRST 還會停在「請拔出記憶卡再重新插入」。
     * 有餵狗，上層的 card_sick 偵測才有機會先跑到。 */
    keepalive();
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
 * 實測到的失敗長相（影片專案那邊撈出來的）：`BSP_SD_WriteBlocks` 回 -4
 * （PERIPH_FAILURE），但 `HAL_SD_GetError` 是 **0** —— 沒有任何硬體錯誤。
 * 這個組合只有一個解釋：HAL 在 `hsd->State != READY` 時直接回 HAL_BUSY，
 * 連暫存器都沒碰。也就是**卡住的是軟體狀態機，不是卡片**。
 *
 * 跟 board-notes 16.8（JPEG 週邊累積狀態要 DeInit+Init）與 16.10
 * （DMA2D 的 State 停在 BUSY）同一個家族：State 沒回到 READY，之後每次
 * 呼叫都被擋在門口，而且回傳值裡沒有錯誤碼。
 *
 * 解法由輕到重：先 HAL_SD_Abort() 把 State 收回 READY，還不行就等卡片自己好。
 */
/* 重試次數從影片專案的 4 次減成 2 次。
 *
 * 那邊是專用的燒錄模式、可以慢慢磨；相簿是使用者按下愛心之後在等的互動，
 * 而每次嘗試最壞是「前等 2 秒 + 寫 + 後等 2 秒」。4 次要 16 秒，正好是
 * 看門狗的長度 —— 就算現在有餵狗，讓使用者盯著畫面等 16 秒也不對。
 * 卡片真的不接受寫入時，早點回報失敗比多試兩次有用。 */
#define SD_WRITE_TRIES  2u

/* 每次寫入完成之後的最小間隔（board-notes 17.9）。 */
#define SD_WRITE_GAP_MS 2u

static DRESULT sd_write_one(const uint32_t *data, uint32_t sector, uint32_t count)
{
    for (uint32_t attempt = 0; attempt < SD_WRITE_TRIES; attempt++) {
        int32_t rc;

        keepalive();

        /* 前一次操作沒結束就送下一個指令，卡片會直接拒絕。
         * 這裡用寫入的長逾時 —— 上一次可能也是寫入，還在收尾。 */
        if (sd_wait_ready_ms(SD_WRITE_WAIT_MS) != 0) {
            g_sd_wtimeout++;
        }

        rc = BSP_SD_WriteBlocks(SD_INSTANCE, (uint32_t *)(uintptr_t)data,
                                sector, count);
        if (rc == BSP_ERROR_NONE) {
            if (sd_wait_ready_ms(SD_WRITE_WAIT_MS) == 0) {
                /* board-notes 17.9：這張卡在**密集單磁區寫入**下大約
                 * 一百多次之後就不再回應，而讀取與大塊循序寫入都沒問題。
                 * 那條記錄建議的第一步就是「等到 SD_TRANSFER_OK 之後
                 * 再加一個最小間隔」—— 只等狀態不夠。
                 *
                 * 實測就是這樣壞的：f_mkdir 逐磁區清空目錄簇，第 33 次
                 * 寫入時卡片進 BUSY 就再也沒回來（FR_DISK_ERR）。
                 *
                 * 大塊寫入不受影響：複製檔案時 FatFs 一次交出上百個
                 * 連續磁區，每次呼叫只多這 2ms。 */
                HAL_Delay(SD_WRITE_GAP_MS);
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
    /* 雙重保險：沒解鎖時 FatFs 看 STA_PROTECT 就不會走到這裡，這裡再擋一次。
     * 播放模式下這個回傳值等於「卡片是唯讀的」，不會有任何寫入發生。 */
    if (g_allow_write != SD_WRITE_MAGIC) {
        return RES_WRPRT;
    }

    g_sd_writes++;
    if (((uint32_t)(uintptr_t)buff & 3U) == 0U) {
        return sd_write_one((const uint32_t *)(uintptr_t)buff,
                            (uint32_t)sector, (uint32_t)count);
    }

    /* 不對齊：一次一個磁區搬過去。 */
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
