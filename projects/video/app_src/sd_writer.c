/**
 * SD 燒錄模式：透過 ST-LINK 把影片檔寫進 SD 卡。
 *
 * 為什麼需要這個：電腦上沒有讀卡機，而影片檔有一百多 MB，放不進外部 Flash
 * 的 128MB（還要扣掉程式本身）。唯一的路是讓板子當中介 —— SWD 把資料塞進
 * PSRAM，韌體收一塊寫一塊到 SD 卡。
 *
 * 實測 SWD 在 24MHz（ST-LINK V3 的上限）可以到約 895 KB/s，
 * 137MB 大約三分鐘，一次性作業可以接受。
 *
 * ── 安全設計 ────────────────────────────────────────────────
 * 使用者的 SD 卡裡有相簿的照片，弄壞的代價很高，所以：
 *
 *  1. **要由外部 magic 觸發**。不寫 g_sdw_go 就完全不會進來，
 *     播放路徑一行寫入的程式碼都不會執行（board-notes 16.3）。
 *  2. **只碰一個檔案**。檔名寫死在 SDW_PATH，沒有任何 f_mkfs、f_unlink
 *     目錄走訪或萬用字元。就算指令碼錯亂，最壞情況是這個檔案內容不對。
 *  3. **磁碟層預設唯讀**。sd_bsp_diskio.c 沒解鎖的話掛載成 STA_PROTECT，
 *     FatFs 自己就會擋掉寫入。解鎖只在這個檔案裡發生。
 *
 * ── 主機端協定 ──────────────────────────────────────────────
 * 指令用一個字傳完，避免「長度和指令分兩次寫」中間被讀到的競爭：
 *
 *     g_sdw_cmd = (opcode << 28) | payload
 *
 *     opcode 1  開檔（建立或清空 SDW_PATH）
 *     opcode 2  把 SDW_BUF 的前 payload 個位元組寫進去
 *     opcode 3  關檔
 *     opcode 4  重置回播放模式
 *     opcode 5  查卡片容量（結果放 g_sdw_total_kb / g_sdw_free_kb）
 *
 * 韌體做完就把 g_sdw_cmd 清成 0 並讓 g_sdw_ack 加一，主機輪詢這個。
 * payload 有 28 位元，蓋得住 16MB 的緩衝區。
 */
#include "main.h"
#include "ff.h"
#include "ff_gen_drv.h"

#include <stdbool.h>
#include <string.h>

extern const Diskio_drvTypeDef SD_BSP_Driver;
void sd_bsp_unlock_write(void);

/* 主機把資料塞在這裡。PSRAM 有 32MB，framebuffer 那幾塊在低位址，
 * 這裡挑 0x91000000 起算的 16MB，跟播放用的緩衝區完全不重疊。 */
#define SDW_BUF         ((uint8_t *)0x91000000u)
#define SDW_CAP         (16u * 1024u * 1024u)

#define SDW_PATH        "0:/video.bin"

#define SDW_GO_MAGIC    0x52574453u     /* 'SDWR' */

volatile uint32_t g_sdw_go;             /* 主機寫 magic 才會進燒錄模式 */
volatile uint32_t g_sdw_cmd;            /* (opcode << 28) | payload */
volatile uint32_t g_sdw_ack;            /* 每做完一個指令加一 */
volatile int32_t  g_sdw_err;            /* 最後一次的 FRESULT */
volatile uint32_t g_sdw_state;          /* 走到哪一步，卡住時看這個 */
volatile uint32_t g_sdw_written;        /* 累計已寫入的位元組 */
volatile uint32_t g_sdw_total_kb;       /* 卡片容量 KB */
volatile uint32_t g_sdw_free_kb;        /* 剩餘空間 KB */
volatile uint32_t g_sdw_oldsize;        /* 覆寫前原本的檔案大小 */
volatile uint32_t g_sdw_trunc;          /* 0=沒截斷 1=進行中 2=成功 3=失敗 */
volatile uint32_t g_sdw_cap = SDW_CAP;  /* 讓主機讀得到緩衝區大小 */
volatile uint32_t g_sdw_buf_addr = (uint32_t)SDW_BUF;

static FATFS g_sdw_fs;
static FIL   g_sdw_fil;
static bool  g_sdw_open;

bool sd_writer_requested(void)
{
    return g_sdw_go == SDW_GO_MAGIC;
}

/* **原地覆寫，不要截斷。**
 *
 * 第一版用 FA_CREATE_ALWAYS，它會先把舊檔的整條簇鏈釋放掉。釋放 122MB 的
 * 簇鏈等於連續打出上百次**單磁區**的 FAT 寫入，實測約 130 次之後卡片就不再
 * 回應指令（HAL_SD_ERROR_CMD_RSP_TIMEOUT），整個覆寫在 f_open 就失敗了。
 *
 * 關鍵對比：**建立新檔完全沒問題**（實測連續寫入 4.58MB 一次過）。
 * 配置簇鏈與釋放簇鏈走的是不同的路，出事的只有釋放那條。
 *
 * 而我們根本不需要截斷 —— 覆寫的目的就是把整個檔案換掉。用 OPEN_ALWAYS
 * 開啟（不存在就建立、存在就保留內容與簇鏈），從位移 0 開始蓋過去：
 *   - 新檔比舊檔大 -> 超出的部分照常配置新簇（那條路是好的）
 *   - 新檔比舊檔小 -> 關檔前用 f_truncate 砍掉尾巴
 *
 * 後者仍然會釋放簇鏈，但只釋放「多出來的尾巴」而不是整條，而且是在資料
 * **全部寫完之後**才做 —— 就算那一步失敗，檔案內容已經是完整的。 */
static FRESULT sdw_open(void)
{
    FRESULT r;

    if (g_sdw_open) {
        (void)f_close(&g_sdw_fil);
        g_sdw_open = false;
    }
    r = f_open(&g_sdw_fil, SDW_PATH, FA_WRITE | FA_OPEN_ALWAYS);
    if (r != FR_OK) {
        return r;
    }
    g_sdw_oldsize = (uint32_t)f_size(&g_sdw_fil);
    r = f_lseek(&g_sdw_fil, 0);
    if (r != FR_OK) {
        (void)f_close(&g_sdw_fil);
        return r;
    }
    g_sdw_open    = true;
    g_sdw_written = 0;
    return FR_OK;
}

static FRESULT sdw_write(uint32_t len)
{
    UINT wrote = 0;
    FRESULT r;

    if (!g_sdw_open) {
        return FR_INVALID_OBJECT;
    }
    if (len > SDW_CAP) {
        return FR_INVALID_PARAMETER;
    }

    /* 資料是偵錯器直接寫進 PSRAM 的，**完全繞過 CPU 的 D-Cache**。
     * 接下來 FatFs 會用 CPU 去讀這塊記憶體，讀到的可能是上一輪殘留在
     * 快取裡的舊內容 —— 跟 board-notes 3.1 那個 DMA2D 的坑同一類：
     * 硬體（這裡是 SWD）寫、CPU 讀，一定要先失效快取。
     * 位址與長度都對齊到 32 位元組的快取列。 */
    SCB_InvalidateDCache_by_Addr((uint32_t *)SDW_BUF,
                                 (int32_t)((len + 31u) & ~31u));

    r = f_write(&g_sdw_fil, SDW_BUF, (UINT)len, &wrote);
    if (r == FR_OK && wrote != len) {
        r = FR_DISK_ERR;                /* 卡滿了也會走到這裡 */
    }
    if (r == FR_OK) {
        g_sdw_written += wrote;
        /* 每塊都同步一次。中途斷電時已寫進去的部分至少是完整的，
         * 而且下一塊的 16MB 傳輸要花十幾秒，這點成本無所謂。 */
        r = f_sync(&g_sdw_fil);
    }
    return r;
}

/* 丟一百多 MB 進去之前先問清楚放不放得下，不要寫到一半才失敗。
 * f_getfree 會走整條 FAT 算空閒簇，大容量的卡可能要幾秒，所以只在需要時做。 */
static FRESULT sdw_stat(void)
{
    FATFS   *fs = NULL;
    DWORD    free_clust = 0;
    FRESULT  r = f_getfree("0:", &free_clust, &fs);

    if (r != FR_OK || fs == NULL) {
        return r;
    }
    /* (簇數 x 每簇磁區數) / 2 = KB，因為一個磁區是 512 bytes。
     * 先除再乘可以避免 32 位元溢位 —— 32GB 的卡用磁區數算會爆掉。 */
    g_sdw_total_kb = ((fs->n_fatent - 2u) / 2u) * fs->csize;
    g_sdw_free_kb  = (free_clust / 2u) * fs->csize;
    return FR_OK;
}

static FRESULT sdw_close(void)
{
    FRESULT r = FR_OK;

    if (g_sdw_open) {
        /* 新檔比舊檔小的話，尾巴要砍掉，否則檔案後面會留著上一份的殘骸。
         * 放在資料全部寫完之後才做：這一步是唯一會釋放簇鏈的動作，
         * 就算它失敗，前面的內容已經完整寫進去了。 */
        if (g_sdw_written < g_sdw_oldsize) {
            g_sdw_trunc = 1u;
            r = f_truncate(&g_sdw_fil);
            g_sdw_trunc = (r == FR_OK) ? 2u : 3u;
        }
        {
            FRESULT rc = f_close(&g_sdw_fil);

            if (r == FR_OK) {
                r = rc;
            }
        }
        g_sdw_open = false;
    }
    return r;
}

/* 進來就不會返回（除非收到 opcode 4 重置）。 */
void sd_writer_run(void)
{
    char drive[4] = {0};
    FRESULT r;

    g_sdw_state = 1;

    /* 解鎖一定要在掛載之前：磁碟層是在 disk_initialize 的當下決定要不要
     * 標成 STA_PROTECT 的，掛完再解鎖沒有用。 */
    sd_bsp_unlock_write();

    g_sdw_state = 2;
    if (FATFS_LinkDriver(&SD_BSP_Driver, drive) != 0) {
        /* 播放路徑開機時已經掛接過同一個驅動，而 FatFs 只有一個磁碟槽，
         * 第二次掛接必然失敗。這不是錯誤 —— 直接沿用 0: 就好。
         * （不要在這裡卡死：燒錄模式幾乎一定是從播放模式進來的，
         *   也就是說「已經掛接過」才是常態。） */
        drive[0] = '0';
        drive[1] = ':';
        drive[2] = '\0';
    }

    /* 第三個參數 1 = 立刻掛載（會真的去初始化磁碟），不要延後。
     * 延後掛載的話錯誤會等到第一次 f_open 才浮現，比較難查。 */
    g_sdw_state = 3;
    r = f_mount(&g_sdw_fs, drive, 1);
    g_sdw_err = (int32_t)r;
    if (r != FR_OK) {
        g_sdw_state = 91;               /* 沒卡、卡沒格式化、或初始化失敗 */
        for (;;) { }
    }

    g_sdw_state = 10;                   /* 就緒，等指令 */
    for (;;) {
        uint32_t cmd = g_sdw_cmd;
        uint32_t op  = cmd >> 28;
        uint32_t arg = cmd & 0x0FFFFFFFu;

        if (op == 0u) {
            continue;
        }

        switch (op) {
        case 1u:
            g_sdw_state = 11;
            g_sdw_err = (int32_t)sdw_open();
            break;
        case 2u:
            g_sdw_state = 12;
            g_sdw_err = (int32_t)sdw_write(arg);
            break;
        case 3u:
            g_sdw_state = 13;
            g_sdw_err = (int32_t)sdw_close();
            break;
        case 4u:
            g_sdw_state = 14;
            (void)sdw_close();
            g_sdw_go  = 0;
            g_sdw_cmd = 0;
            g_sdw_ack++;
            __DSB();
            NVIC_SystemReset();         /* 重置回播放模式 */
            break;
        case 5u:
            g_sdw_state = 15;
            g_sdw_err = (int32_t)sdw_stat();
            break;
        default:
            g_sdw_err = -1;
            break;
        }

        g_sdw_state = 10;
        g_sdw_cmd = 0;                  /* 先清指令 */
        __DSB();
        g_sdw_ack++;                    /* 再通知主機，順序不能反 */
    }
}
