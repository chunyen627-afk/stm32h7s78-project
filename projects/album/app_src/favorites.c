/**
 * 「我的最愛」：卡上一個固定大小的清單檔，記住哪幾張被收藏了。
 *
 * ── 為什麼不是複製照片 ──────────────────────────────────────
 * 第一版真的是把照片複製到 `我的最愛` 資料夾，理由是「卡插到電腦上也看得到
 * 那些照片」。功能做出來也驗證過了（連續四次操作零失敗、長度逐位元組相符），
 * 但實際用起來有三個問題，而第一個是決定性的：
 *
 *  1. **寫入量差約 400 倍。** 複製一張 600KB 的照片是約 1200 個磁區寫入，
 *     而這張卡在連續寫入下會進 BUSY 就不再回應（board-notes 20）。
 *     卡上是使用者唯一一份 4254 張照片，不值得每收藏一張就去壓它的極限。
 *  2. **目錄簇的上限。** 副本檔名長，一個 32KB 的簇大約放 200 個檔案，
 *     再多就要配置第二個簇 -> 又是 64 次單磁區寫入 -> 同樣掛掉。
 *  3. **選單不會即時更新。** 相簿只在開機／拔插卡片時掃描，所以複製完的
 *     照片要重開機才看得到 —— 使用者第一次用就踩到這個。
 *
 * 清單版每次收藏只寫 2~3 個磁區，沒有數量上限，而且清單在記憶體裡，
 * 選單立刻反映。代價是電腦上看到的是一份文字清單而不是照片資料夾。
 *
 * ── 為什麼是固定寬度的紀錄 ──────────────────────────────────
 * 每筆固定 FAV_REC_BYTES，所以改一筆只要 f_lseek 到那一格覆寫 256 個位元組，
 * FatFs 就只做一個磁區的 read-modify-write。**檔案大小從頭到尾不變**，
 * 於是不配置新簇、不釋放簇鏈、不動 FAT —— 而釋放簇鏈正是 board-notes 17.7
 * 把這張卡搞到不回應的動作。
 *
 * ── 安全設計（沿用第一版）───────────────────────────────────
 *  - 磁碟層預設唯讀（STA_PROTECT），只有這個檔案會解鎖，動完立刻鎖回去
 *  - 只碰 FAV_FILE_NAME 這一個檔案，沒有 f_unlink、沒有目錄走訪、沒有遞迴
 *  - 建立清單檔用 FA_CREATE_NEW，不用 CREATE_ALWAYS（後者會先釋放舊檔的簇鏈）
 *  - 寫完讀回來比對，回傳 OK 不等於做對了
 */
#include "main.h"
#include "favorites.h"

#include <stdio.h>
#include <string.h>

/* 磁碟層的寫入閘門。 */
void sd_bsp_unlock_write(void);
void sd_bsp_lock_write(void);
int32_t sd_bsp_hard_reinit(void);

/* 每寫幾次就主動把 SD 週邊重新初始化一次；0 = 不做。實驗用。 */
volatile uint32_t g_dbg_wrreinit;

/* 清單的記憶體映像，同時也是檔案內容。64KB，放 PSRAM 尾端的空區：
 * 0x91E00000 是資料夾索引（8KB），到 0x92000000 是 32MB 的盡頭。
 * 跟索引隔開一段而不是首尾相接（board-notes 11.5）。 */
#define PSRAM_BASE      0x90000000u
#define FAV_TABLE       ((char *)(PSRAM_BASE + 0x01E10000u))

#define FAV_PATH_MAX    (FAV_REC_BYTES - 2u)   /* 扣掉結尾的 CRLF */

static char  g_path[64];                /* "0:/我的最愛.txt" */
static bool  g_ready;
static uint32_t g_count;

/* SWD 觀察用。每個失敗出口各自累計 —— 只留「最後一次的錯誤碼」在級聯
 * 失敗裡一定看到二次症狀（board-notes 18.2）。 */
volatile uint32_t g_fav_add_ok;
volatile uint32_t g_fav_add_fail;
volatile uint32_t g_fav_del_ok;
volatile uint32_t g_fav_del_fail;
volatile int32_t  g_fav_lasterr;
volatile uint32_t g_fav_errhist[9];     /* 索引 = -錯誤碼 */
volatile uint32_t g_fav_loaded;         /* 開機載入到幾筆 */
volatile uint32_t g_fav_created;        /* 建立過清單檔幾次 */
volatile uint32_t g_fav_slot_writes;    /* 寫過幾格 */
volatile uint32_t g_fav_fr;             /* 最後一次 FatFs 的回傳碼 */
volatile uint32_t g_fav_fr_open;        /* 開機時 f_open 清單檔的回傳碼 */
volatile uint32_t g_fav_badlen;         /* 清單檔長度不對的次數 */
volatile uint32_t g_fav_openerr;        /* 開機讀不到清單檔（非 NO_FILE）的次數 */

/* 寫入壓力測試的結果，見 fav_stress()。 */
volatile uint32_t g_fav_st_ok;
volatile uint32_t g_fav_st_fail;
volatile uint32_t g_fav_st_first;       /* 第幾次開始失敗 */
volatile uint32_t g_fav_st_done;
volatile uint32_t g_dbg_wrspread;       /* 1 = 每次換一格寫（散布磁區）*/

static fav_result_t fail(fav_result_t e)
{
    uint32_t i = (uint32_t)(-(int32_t)e);

    g_fav_lasterr = (int32_t)e;
    if (i < (sizeof(g_fav_errhist) / sizeof(g_fav_errhist[0]))) {
        g_fav_errhist[i]++;
    }
    return e;
}

static char *rec(uint32_t i)
{
    return FAV_TABLE + (size_t)i * FAV_REC_BYTES;
}

/* 空格子：整筆都是空白。路徑一定以 '/' 開頭，所以看第一個位元組就分得出來。 */
static bool rec_empty(uint32_t i)
{
    return rec(i)[0] != '/';
}

/* 這一格存的是不是這個路徑。
 *
 * **不能拿空白當結束符。** 紀錄是用空白補到固定寬度的，但使用者的資料夾
 * 名稱裡本來就可能有空白（卡上實際就有 "Ayu Makihara (...) 77 set in 1"）。
 * 第一版用空白當終止字元，路徑一有空白就被截斷，比對永遠失敗 ——
 * 症狀是「收藏當下愛心變紅，但下次看到同一張又是空心，最愛也播不出來」。
 *
 * **而且用沒有空白的資料夾完全測不到。** 先前那三張測試照片都在
 * ai_generated/ 底下，路徑裡一個空白都沒有，所以全部通過；
 * 自動測試收藏 12 張、最愛模式只找回 2 張，才把它抓出來。
 *
 * 正解是從固定的結尾往回剝掉補的空白。FAT 檔名不會以空白結尾
 * （Windows 會去掉），所以不會誤傷。
 *
 * 直接比對而不先取出字串，順便快很多：絕大多數格子第一個位元組就不同，
 * memcmp 立刻返回。 */
static bool rec_eq(uint32_t i, const char *rel, size_t rl)
{
    const char *r = rec(i);

    if (rl == 0u || rl > FAV_PATH_MAX) {
        return false;
    }
    if (memcmp(r, rel, rl) != 0) {
        return false;
    }
    for (size_t k = rl; k < FAV_PATH_MAX; k++) {
        if (r[k] != ' ') { return false; }      /* 後面必須全是補的空白 */
    }
    return true;
}

/* 相簿的路徑是 "0:/資料夾/檔名.jpg"，清單裡存的是去掉 "0:" 之後的
 * "/資料夾/檔名.jpg" —— 在電腦上讀起來乾淨，而且跟磁碟機代號無關。 */
static const char *to_rel(const char *full)
{
    const char *s = strchr(full, '/');

    return (s != NULL) ? s : full;
}

static void rec_set(uint32_t i, const char *rel)
{
    char  *r = rec(i);
    size_t n = (rel != NULL) ? strlen(rel) : 0u;

    memset(r, ' ', FAV_REC_BYTES);
    if (n > 0u && n <= FAV_PATH_MAX) {
        memcpy(r, rel, n);
    }
    r[FAV_REC_BYTES - 2u] = '\r';       /* 一筆 = 記事本裡的一行 */
    r[FAV_REC_BYTES - 1u] = '\n';
}

static uint32_t recount(void)
{
    uint32_t n = 0;

    for (uint32_t i = 0; i < FAV_MAX; i++) {
        if (!rec_empty(i)) { n++; }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* 檔案                                                                */
/* ------------------------------------------------------------------ */

/* 把整塊表寫成一個新檔。只在清單檔不存在時做一次。
 *
 * 這是大塊循序寫入（64KB 一次交出去，FatFs 會做多磁區傳輸），
 * 也就是 board-notes 17.9 實測過沒問題的那條路 —— 跟 f_mkdir 那種
 * 逐磁區清空目錄簇完全不同。 */
static fav_result_t create_file(void)
{
    FIL     f;
    FRESULT r;
    UINT    put = 0;

    for (uint32_t i = 0; i < FAV_MAX; i++) {
        rec_set(i, NULL);
    }

    sd_bsp_unlock_write();
    r = f_open(&f, g_path, FA_WRITE | FA_CREATE_NEW);
    g_fav_fr = (uint32_t)r;
    if (r != FR_OK) {
        sd_bsp_lock_write();
        return FAV_ERR_CREATE;
    }
    r = f_write(&f, FAV_TABLE, FAV_FILE_BYTES, &put);
    g_fav_fr = (uint32_t)r;
    (void)f_close(&f);
    sd_bsp_lock_write();

    if (r != FR_OK || put != FAV_FILE_BYTES) {
        return FAV_ERR_CREATE;
    }
    g_fav_created++;
    return FAV_OK;
}

/* 只覆寫一格。**整個設計的重點就是這個函式只動一個磁區。** */
static fav_result_t write_slot(uint32_t i)
{
    FIL      f;
    FRESULT  r;
    UINT     put = 0, got = 0;
    char     back[FAV_REC_BYTES];

    sd_bsp_unlock_write();
    r = f_open(&f, g_path, FA_WRITE | FA_READ | FA_OPEN_EXISTING);
    g_fav_fr = (uint32_t)r;
    if (r != FR_OK) {
        sd_bsp_lock_write();
        return FAV_ERR_OPEN;
    }
    r = f_lseek(&f, (FSIZE_t)i * FAV_REC_BYTES);
    g_fav_fr = (uint32_t)r;
    if (r != FR_OK) {
        (void)f_close(&f);
        sd_bsp_lock_write();
        return FAV_ERR_SEEK;
    }
    r = f_write(&f, rec(i), FAV_REC_BYTES, &put);
    g_fav_fr = (uint32_t)r;
    if (r != FR_OK || put != FAV_REC_BYTES) {
        (void)f_close(&f);
        sd_bsp_lock_write();
        return FAV_ERR_WRITE;
    }

    /* 讀回來比對。寫入回傳 OK 不代表卡片真的收下了 ——
     * board-notes 第八章那條「量到沒報錯不等於結果正確」。 */
    if (f_lseek(&f, (FSIZE_t)i * FAV_REC_BYTES) == FR_OK &&
        f_read(&f, back, FAV_REC_BYTES, &got) == FR_OK &&
        got == FAV_REC_BYTES &&
        memcmp(back, rec(i), FAV_REC_BYTES) == 0) {
        r = f_close(&f);
        g_fav_fr = (uint32_t)r;
        sd_bsp_lock_write();
        if (r != FR_OK) {
            return FAV_ERR_WRITE;
        }
        g_fav_slot_writes++;
        return FAV_OK;
    }

    (void)f_close(&f);
    sd_bsp_lock_write();
    return FAV_ERR_VERIFY;
}

void fav_init(const char *drive)
{
    FIL     f;
    FRESULT r;
    UINT    got = 0;

    g_ready = false;
    g_count = 0;

    /* drive 是 "0:/"，結尾已經有斜線。 */
    (void)snprintf(g_path, sizeof(g_path), "%s%s", drive, FAV_FILE_NAME);

    r = f_open(&f, g_path, FA_READ);
    g_fav_fr_open = (uint32_t)r;

    if (r == FR_OK) {
        r = f_read(&f, FAV_TABLE, FAV_FILE_BYTES, &got);
        (void)f_close(&f);
        if (r == FR_OK && got == FAV_FILE_BYTES) {
            g_count      = recount();
            g_fav_loaded = g_count;
            g_ready      = true;
            return;
        }
        /* 長度不對（被人改過／舊版）就不要亂猜，也**不要重建** ——
         * 使用者在電腦上刪掉它，下次開機才會建一個乾淨的。 */
        g_fav_badlen++;
        return;
    }

    /* **只有「真的沒有這個檔案」才建新的。**
     *
     * 這裡踩過一個會**掉資料**的坑：原本是「f_open 失敗就 create_file()」，
     * 把 FR_DISK_ERR 也當成「檔案不存在」。這張卡偶爾會有一瞬間讀不到，
     * 那一下就足以讓韌體用 FA_CREATE_NEW 造一個新的空清單 ——
     * 使用者 41 筆收藏就這樣變成 1 筆。
     *
     * f_open 的失敗必須分開看：FR_NO_FILE / FR_NO_PATH 是「真的沒有」，
     * 其餘（FR_DISK_ERR、FR_NOT_READY…）是「這次讀不到」，那就這輪不提供
     * 收藏功能（愛心會顯示成不可按），下次開機再說。**寧可少一個功能，
     * 不要毀掉使用者的清單。** */
    if (r == FR_NO_FILE || r == FR_NO_PATH) {
        if (create_file() == FAV_OK) {
            g_count = 0;
            g_ready = true;
        }
    } else {
        g_fav_openerr++;        /* 卡片這次讀不到，什麼都不做 */
    }
}

/* 純粹的寫入壓力測試：對最後一格反覆寫入同樣的空白內容 N 次。
 *
 * **不動清單內容**（最後一格本來就是空的，寫進去還是空的），所以這是在量
 * 「這張卡連續接受幾次小型寫入」，跟最愛的邏輯完全無關 ——
 * 用來回答「到底是不是卡片的問題」。
 *
 * 每次寫入 = 一個資料磁區 + 一個目錄項，跟真正收藏一次的成本相同。 */
void fav_stress(uint32_t n)
{
    uint32_t run = 0;

    g_fav_st_ok = 0; g_fav_st_fail = 0; g_fav_st_first = 0; g_fav_st_done = 0;
    if (!g_ready) {
        return;
    }

    for (uint32_t i = 0; i < n; i++) {
        /* g_dbg_wrspread：每次換一格寫，等於散布到不同的磁區。
         *
         * 預設是**一直寫同一格**（同一個資料磁區 + 同一個目錄項）。
         * 如果只有這樣會死、換位置就不會，代表卡片受不了的是「反覆寫同一個
         * 位置」而不是「寫入」本身 —— 那在軟體端有解（把紀錄輪流散開）。
         * 兩者要分開量才知道。
         *
         * 從尾端往回取，避開前面正在用的格子。 */
        uint32_t slot = g_dbg_wrspread
                        ? (FAV_MAX - 1u - (i % (FAV_MAX / 2u)))
                        : (FAV_MAX - 1u);

        if (g_dbg_wrreinit != 0u && i != 0u && (i % g_dbg_wrreinit) == 0u) {
            (void)sd_bsp_hard_reinit();
        }
        rec_set(slot, NULL);
        if (write_slot(slot) == FAV_OK) {
            g_fav_st_ok++;
            run = 0;
        } else {
            g_fav_st_fail++;
            if (g_fav_st_first == 0u) { g_fav_st_first = i + 1u; }
            /* 卡片一旦不回應就再也不會好，繼續試只是每次多等十幾秒
             * （每次失敗要走完兩輪 SD_WRITE_WAIT_MS）。連續八次就收工。 */
            if (++run >= 8u) { break; }
        }
    }
    g_fav_st_done = 1u;
}

bool     fav_ready(void) { return g_ready; }
uint32_t fav_count(void) { return g_count; }

static uint32_t find_slot(const char *rel)
{
    size_t rl = strlen(rel);

    for (uint32_t i = 0; i < FAV_MAX; i++) {
        if (rec_empty(i)) { continue; }
        if (rec_eq(i, rel, rl)) { return i; }
    }
    return FAV_MAX;
}

bool fav_is(const char *path)
{
    if (!g_ready || path == NULL) { return false; }
    return find_slot(to_rel(path)) < FAV_MAX;
}

fav_result_t fav_add(const char *path)
{
    const char  *rel;
    uint32_t     slot;
    fav_result_t r;

    if (!g_ready || path == NULL) {
        g_fav_add_fail++;
        return fail(FAV_ERR_NOTREADY);
    }
    rel = to_rel(path);
    if (strlen(rel) > FAV_PATH_MAX) {
        g_fav_add_fail++;
        return fail(FAV_ERR_NAME);
    }
    if (find_slot(rel) < FAV_MAX) {
        return FAV_OK;                  /* 已經收藏了，當成成功 */
    }

    for (slot = 0; slot < FAV_MAX; slot++) {
        if (rec_empty(slot)) { break; }
    }
    if (slot >= FAV_MAX) {
        g_fav_add_fail++;
        return fail(FAV_ERR_FULL);
    }

    rec_set(slot, rel);
    r = write_slot(slot);
    if (r != FAV_OK) {
        rec_set(slot, NULL);            /* 卡上沒寫成功就不要留在記憶體裡 */
        g_fav_add_fail++;
        return fail(r);
    }
    g_count++;
    g_fav_add_ok++;
    g_fav_lasterr = 0;
    return FAV_OK;
}

fav_result_t fav_remove(const char *path)
{
    const char  *rel;
    uint32_t     slot;
    char         keep[FAV_REC_BYTES];
    fav_result_t r;

    if (!g_ready || path == NULL) {
        g_fav_del_fail++;
        return fail(FAV_ERR_NOTREADY);
    }
    rel  = to_rel(path);
    slot = find_slot(rel);
    if (slot >= FAV_MAX) {
        return FAV_OK;                  /* 本來就不在清單裡 */
    }

    memcpy(keep, rec(slot), FAV_REC_BYTES);
    rec_set(slot, NULL);
    r = write_slot(slot);
    if (r != FAV_OK) {
        memcpy(rec(slot), keep, FAV_REC_BYTES);   /* 還原記憶體 */
        g_fav_del_fail++;
        return fail(r);
    }
    if (g_count > 0u) { g_count--; }
    g_fav_del_ok++;
    g_fav_lasterr = 0;
    return FAV_OK;
}
