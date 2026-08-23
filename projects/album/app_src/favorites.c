/**
 * 「我的最愛」：把照片複製到卡上的一個資料夾，之後就是一個普通的最上層
 * 資料夾，選單勾它就能播 —— 播放那半完全不需要新程式碼。
 *
 * 為什麼是複製檔案而不是存一份清單：使用者要的是「SD 卡插到電腦上也看得到
 * 這些照片」。清單檔做不到這件事，而複製出來的資料夾在任何電腦上都是
 * 一般的相簿資料夾。
 *
 * ── 安全設計（照 projects/video/app_src/sd_writer.c 那套）─────────
 * 卡上是使用者真正的相簿，4254 張照片沒有第二份，所以：
 *
 *  1. **只碰一個資料夾**。所有寫入與刪除的目標都由 fav_target() 算出來，
 *     而且動手之前一定再用 under_fav_dir() 檢查一次前綴。最壞情況是這個
 *     資料夾裡的副本不對，永遠碰不到原檔。
 *  2. **沒有任何目錄走訪、萬用字元、遞迴**。f_unlink 只對單一檔案呼叫，
 *     一次使用者動作只處理一個檔案 —— 不做批次迴圈，就不會逼近
 *     board-notes 17.7 那個「連續上百次單磁區 FAT 寫入之後卡片不回應」。
 *  3. **磁碟層預設唯讀**。sd_bsp_diskio.c 沒解鎖的話掛成 STA_PROTECT，
 *     FatFs 自己就擋掉寫入。解鎖只在這個檔案裡、而且只在 begin/end 之間。
 *  4. **建立新檔而不是覆寫**。用 FA_CREATE_NEW：檔案已存在就失敗，
 *     絕不會走到「先釋放舊檔簇鏈」那條路（17.7 就是那條）。
 *  5. **不建資料夾**。f_mkdir 是唯一會做密集單磁區寫入的操作，這張卡撐不住
 *     （理由與實測寫在 favorites.h）。資料夾請在電腦上建好。
 *
 * ── 不要重新掛載（第一版的錯誤）─────────────────────────────
 * 原本以為 STA_PROTECT 只在 disk_initialize 當下決定，所以做成
 * 「卸載 -> 解鎖 -> 掛載 -> 動手 -> 卸載 -> 上鎖 -> 掛回去」。
 *
 * 實際看 ff.c:3416-3419：volume 已掛載時 mount_volume() 仍然每次都呼叫
 * disk_status() 並重新檢查 STA_PROTECT —— 重新掛載完全沒有必要。
 *
 * 而它的代價是真的踩到了：卡片拒絕寫入時，收尾那次 f_mount 也一起失敗，
 * 檔案系統就停在**卸載狀態**，相簿之後每次 f_open 都失敗。
 * 現在只切旗標，不動掛載，這個失敗模式就不存在了。
 */
#include "main.h"
#include "favorites.h"

#include <stdio.h>
#include <string.h>

/* 磁碟層的寫入閘門。 */
void sd_bsp_unlock_write(void);
void sd_bsp_lock_write(void);

/* 複製用的中繼緩衝區。
 *
 * 放 PSRAM 的尾端空區：0x91E00000 是資料夾索引（8KB），到 0x92000000 是
 * 32MB 的盡頭，中間全是空的。跟索引隔開 64KB 而不是首尾相接 ——
 * board-notes 11.5 那條。
 *
 * 64KB 一塊：仍然是「大塊循序寫入」那條實測沒問題的路徑（17.9），
 * 而且 2MB 的照片剛好切成 32 次進度回呼。 */
#define PSRAM_BASE      0x90000000u
#define FAV_BUF         ((uint8_t *)(PSRAM_BASE + 0x01E10000u))
#define FAV_CHUNK       (64u * 1024u)

/* 路徑上限。相簿的 PATH_MAX 是 256，映射後只是把 '/' 換成 '~'，長度不變，
 * 再加上最愛資料夾前綴，320 綽綽有餘。 */
#define FAV_PATH_MAX    320u

static FATFS *g_fs;
static char   g_drv[4];                 /* "0:/" */
static char   g_favdir[FAV_PATH_MAX];   /* "0:/我的最愛" */
static size_t g_favdir_len;

/* SWD 觀察用。每個失敗出口各自累計 —— 只留「最後一次的錯誤碼」在級聯
 * 失敗裡一定看到二次症狀（board-notes 18.2）。 */
volatile uint32_t g_fav_add_ok;
volatile uint32_t g_fav_add_fail;
volatile uint32_t g_fav_del_ok;
volatile uint32_t g_fav_del_fail;
volatile int32_t  g_fav_lasterr;
volatile uint32_t g_fav_errhist[14];    /* 索引 = -錯誤碼，見 fav_result_t */
volatile uint32_t g_fav_bytes;          /* 最後一次複製的長度 */
volatile uint32_t g_fav_expect;         /* 應該要有的長度 */
volatile uint32_t g_fav_partial_rm;     /* 失敗後清掉半截檔案的次數 */

/* FatFs 自己的回傳碼。只知道「mkdir 失敗」不知道 FatFs 說的是什麼，
 * 等於還是在猜 —— 上一輪就卡在這裡（board-notes 第八章：有辦法量的時候
 * 不要推理）。FR_WRITE_PROTECTED=10、FR_DISK_ERR=1、FR_DENIED=7。 */
volatile uint32_t g_fav_fr_dir;
volatile uint32_t g_fav_fr_src;
volatile uint32_t g_fav_fr_dst;
volatile uint32_t g_fav_fr_write;
volatile uint32_t g_fav_fr_unlink;

static fav_result_t fail(fav_result_t e)
{
    uint32_t i = (uint32_t)(-(int32_t)e);

    g_fav_lasterr = (int32_t)e;
    if (i < (sizeof(g_fav_errhist) / sizeof(g_fav_errhist[0]))) {
        g_fav_errhist[i]++;
    }
    return e;
}

void fav_init(FATFS *fs, const char *drive)
{
    g_fs = fs;
    strncpy(g_drv, drive, sizeof(g_drv) - 1u);
    g_drv[sizeof(g_drv) - 1u] = 0;

    /* g_drv 是 "0:/"（結尾有斜線），直接接上資料夾名就是 "0:/我的最愛"。 */
    (void)snprintf(g_favdir, sizeof(g_favdir), "%s%s", g_drv, FAV_DIR_NAME);
    g_favdir_len = strlen(g_favdir);
}

/* 去掉磁碟機前綴："0:/2020/a/x.jpg" -> "2020/a/x.jpg" */
static const char *strip_drive(const char *p)
{
    const char *s = strchr(p, '/');

    return (s != NULL) ? (s + 1) : p;
}

/* 目標路徑：來源的相對路徑把 '/' 換成 '~'，放進最愛資料夾。
 *
 *   0:/2020/a/IMG_1.jpg  ->  0:/我的最愛/2020~a~IMG_1.jpg
 *
 * 這個映射是**決定性**的，所以「收藏了沒」與「要刪誰」都用同一個字串去
 * f_stat，不必維護任何索引、不必開機載入、也不會有索引跟實際檔案不同步
 * 的問題。而且在電腦上看得出這張原本放在哪個資料夾。 */
static fav_result_t fav_target(const char *src, char *out, size_t cap)
{
    const char *rel = strip_drive(src);
    size_t      n   = g_favdir_len;
    size_t      i;

    /* 映射後整串就是**一個**檔名，不能超過 FF_MAX_LFN。 */
    if (strlen(rel) > FF_MAX_LFN) {
        return FAV_ERR_NAME;
    }
    if (n + 1u + strlen(rel) + 1u > cap) {
        return FAV_ERR_NAME;
    }

    memcpy(out, g_favdir, n);
    out[n++] = '/';
    for (i = 0; rel[i] != 0; i++) {
        out[n++] = (rel[i] == '/') ? '~' : rel[i];
    }
    out[n] = 0;
    return FAV_OK;
}

/* 動手前的最後一道關卡：這個路徑真的在最愛資料夾底下嗎。
 *
 * 這是整份程式最重要的一行。就算上面的映射寫錯，只要這裡擋住，
 * 最壞情況也只是「副本沒建成」，不可能刪到使用者的原檔。 */
static bool under_fav_dir(const char *p)
{
    return strncmp(p, g_favdir, g_favdir_len) == 0 && p[g_favdir_len] == '/'
           && p[g_favdir_len + 1u] != 0;
}

bool fav_source_is_in_folder(const char *src)
{
    return under_fav_dir(src);
}

bool fav_exists(const char *src)
{
    char    dst[FAV_PATH_MAX];
    FILINFO fi;

    if (g_fs == NULL || fav_target(src, dst, sizeof(dst)) != FAV_OK) {
        return false;
    }
    return f_stat(dst, &fi) == FR_OK;
}

/* ------------------------------------------------------------------ */
/* 讀寫模式的進出                                                      */
/* ------------------------------------------------------------------ */

/* **不要重新掛載。**
 *
 * 第一版是照影片專案那樣「卸載 -> 解鎖 -> 掛載」，因為以為 STA_PROTECT
 * 只在 disk_initialize 當下決定。實際看 ff.c:3416-3419，volume 已掛載時
 * mount_volume() 仍然每次都呼叫 disk_status() 並重新檢查 STA_PROTECT ——
 * 所以只要 sdbsp_status() 重算，解鎖就立刻生效。
 *
 * 那一版的代價很實在：卡片拒絕寫入時，收尾的 f_mount 也跟著失敗，
 * 檔案系統就停在卸載狀態，相簿之後每次 f_open 都失敗。不卸載就沒這回事。 */
static void begin_write(void)
{
    sd_bsp_unlock_write();
}

static void end_write(void)
{
    sd_bsp_lock_write();
}

/* ------------------------------------------------------------------ */
/* 加入                                                                */
/* ------------------------------------------------------------------ */

static fav_result_t copy_file(const char *src, const char *dst,
                              fav_progress_fn progress)
{
    FIL          fi, fo;
    FRESULT      r;
    uint32_t     total, done = 0;
    fav_result_t res = FAV_OK;

    /* **刻意不呼叫 f_mkdir** —— 理由寫在 favorites.h 的長註解裡：
     * 它會逐磁區清空目錄簇，這張卡在第 33 次單磁區寫入就不再回應，
     * 而且失敗會留下遺失簇。資料夾請在電腦上先建好。 */
    {
        FILINFO di;

        r = f_stat(g_favdir, &di);
        g_fav_fr_dir = (uint32_t)r;
        if (r != FR_OK) {
            return FAV_ERR_NODIR;
        }
        if ((di.fattrib & AM_DIR) == 0) {
            return FAV_ERR_NODIR;       /* 同名的是檔案不是資料夾 */
        }
    }

    r = f_open(&fi, src, FA_READ);
    g_fav_fr_src = (uint32_t)r;
    if (r != FR_OK) {
        return FAV_ERR_OPEN_SRC;
    }
    total        = (uint32_t)f_size(&fi);
    g_fav_expect = total;
    g_fav_bytes  = 0;

    /* FA_CREATE_NEW：檔案已經在就失敗。**刻意不用 CREATE_ALWAYS** ——
     * 那會先釋放舊檔的簇鏈，正是 board-notes 17.7 把卡搞到不回應的動作。 */
    r = f_open(&fo, dst, FA_WRITE | FA_CREATE_NEW);
    g_fav_fr_dst = (uint32_t)r;
    if (r != FR_OK) {
        (void)f_close(&fi);
        return FAV_ERR_OPEN_DST;
    }

    while (done < total) {
        UINT want = (UINT)((total - done) > FAV_CHUNK ? FAV_CHUNK
                                                      : (total - done));
        UINT got = 0, put = 0;

        if (f_read(&fi, FAV_BUF, want, &got) != FR_OK || got == 0u) {
            res = FAV_ERR_READ;
            break;
        }
        r = f_write(&fo, FAV_BUF, got, &put);
        g_fav_fr_write = (uint32_t)r;
        if (r != FR_OK) {
            res = FAV_ERR_WRITE;
            break;
        }
        if (put != got) {
            /* FatFs 寫不滿而且沒回錯誤 = 沒空間了。 */
            res = FAV_ERR_SPACE;
            break;
        }
        done += put;
        if (progress != NULL) {
            progress(done, total);      /* 呼叫端在這裡餵看門狗 */
        }
    }

    (void)f_close(&fi);
    if (f_close(&fo) != FR_OK && res == FAV_OK) {
        res = FAV_ERR_WRITE;            /* 關檔才會把最後一批寫下去 */
    }
    g_fav_bytes = done;

    if (res == FAV_OK && done != total) {
        res = FAV_ERR_VERIFY;
    }

    /* 半截的副本比沒有更糟：畫面上會出現一張壞掉的照片，而且愛心會顯示
     * 已收藏。清掉它 —— 這是唯一一處在失敗路徑上呼叫 f_unlink，
     * 目標同樣先過 under_fav_dir()。 */
    if (res != FAV_OK && under_fav_dir(dst)) {
        if (f_unlink(dst) == FR_OK) {
            g_fav_partial_rm++;
        }
    }
    return res;
}

fav_result_t fav_add(const char *src, fav_progress_fn progress)
{
    char         dst[FAV_PATH_MAX];
    fav_result_t r;

    if (g_fs == NULL) {
        return fail(FAV_ERR_MOUNT);
    }
    if (fav_source_is_in_folder(src)) {
        return fail(FAV_ERR_IS_FAV);
    }
    r = fav_target(src, dst, sizeof(dst));
    if (r != FAV_OK) {
        return fail(r);
    }
    if (!under_fav_dir(dst)) {
        return fail(FAV_ERR_GUARD);
    }

    /* 解鎖 -> 動手 -> 上鎖。中間不重新掛載，所以不管成功失敗，
     * 檔案系統都維持在可用狀態。 */
    begin_write();
    r = copy_file(src, dst, progress);
    end_write();

    if (r == FAV_OK) {
        g_fav_add_ok++;
        g_fav_lasterr = 0;
    } else {
        g_fav_add_fail++;
        (void)fail(r);
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* 移除                                                                */
/* ------------------------------------------------------------------ */

fav_result_t fav_remove(const char *src)
{
    char         dst[FAV_PATH_MAX];
    FILINFO      st;
    fav_result_t r;

    if (g_fs == NULL) {
        return fail(FAV_ERR_MOUNT);
    }
    /* 正在看的就是副本本身：刪掉之後沒有來源可以復原。呼叫端應該已經
     * 把愛心設成不可按，這裡再擋一次。 */
    if (fav_source_is_in_folder(src)) {
        return fail(FAV_ERR_IS_FAV);
    }
    r = fav_target(src, dst, sizeof(dst));
    if (r != FAV_OK) {
        return fail(r);
    }
    /* 刪除只允許發生在最愛資料夾底下。 */
    if (!under_fav_dir(dst)) {
        return fail(FAV_ERR_GUARD);
    }

    begin_write();
    {
        FRESULT fr = f_unlink(dst);

        g_fav_fr_unlink = (uint32_t)fr;
        if (fr != FR_OK) {
            r = FAV_ERR_UNLINK;
        } else if (f_stat(dst, &st) != FR_NO_FILE) {
            /* 回傳 OK 不等於真的不見了（board-notes 第八章：量到「沒報錯」
             * 不等於「結果正確」）。 */
            r = FAV_ERR_VERIFY;
        }
    }
    end_write();

    if (r == FAV_OK) {
        g_fav_del_ok++;
        g_fav_lasterr = 0;
    } else {
        g_fav_del_fail++;
        (void)fail(r);
    }
    return r;
}
