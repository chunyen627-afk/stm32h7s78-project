#ifndef FAVORITES_H_INCLUDED
#define FAVORITES_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#include "ff.h"

/* 最愛資料夾的名稱（不含磁碟機前綴）。
 *
 * 掃描器要拿它比對：掃到同名的最上層資料夾時**預設不勾選**，否則收藏過的
 * 照片會同時出現在原資料夾與最愛資料夾裡，隨機播放就會播到兩次。 */
#define FAV_DIR_NAME    "我的最愛"

/* 每個失敗出口一個代碼。board-notes 18.2：級聯失敗裡「最後一次的錯誤碼」
 * 一定是二次症狀，所以呼叫端要把它們分開記，不要只留最後一個。 */
typedef enum {
    FAV_OK           =   0,
    FAV_ERR_NAME     =  -1,   /* 映射後的檔名超過 FF_MAX_LFN */
    FAV_ERR_IS_FAV   =  -2,   /* 來源本身就在最愛資料夾裡，不能刪自己 */
    FAV_ERR_MOUNT    =  -3,   /* 切換讀寫模式時掛載失敗 */
    FAV_ERR_MKDIR    =  -4,
    FAV_ERR_OPEN_SRC =  -5,
    FAV_ERR_OPEN_DST =  -6,
    FAV_ERR_READ     =  -7,
    FAV_ERR_WRITE    =  -8,
    FAV_ERR_SPACE    =  -9,   /* 卡片空間不足 */
    FAV_ERR_UNLINK   = -10,
    FAV_ERR_VERIFY   = -11,   /* 做完了但結果不對（長度不符／檔案還在）*/
    FAV_ERR_GUARD    = -12,   /* 目標不在最愛資料夾底下，拒絕動作 */
    FAV_ERR_NODIR    = -13,   /* 最愛資料夾不存在，要先在電腦上建好 */
} fav_result_t;

/* 為什麼不由韌體 f_mkdir 建這個資料夾
 * ------------------------------------
 * FatFs 的 f_mkdir 會呼叫 dir_clear()，而它在 FF_USE_LFN != 3 的設定下是
 * **一次一個磁區**把整個目錄簇清成 0（ff.c:1685，原始碼註解自己就寫著
 * "many single-sector writes may take a time"）。
 *
 * 這張卡受不了密集單磁區寫入 —— board-notes 17.9 量過「讀取數千次零錯誤、
 * 大塊循序寫入 4.58MB 一次過、密集單磁區寫入約 130 次後死」。實測 f_mkdir
 * 每次都在第 33 次寫入時卡片進 BUSY 就不再回應（兩次完全相同，加上
 * 每次寫入之間的最小間隔也沒有改善 —— 所以不是時序而是這條路徑本身）。
 *
 * 更糟的是失敗時簇已經配置出去，會在卡上留下遺失簇，重試就一直累積。
 *
 * 複製檔案本身沒有這個問題：FatFs 對大塊資料是多磁區寫入，正是實測過
 * 沒問題的那條路。所以資料夾請在電腦上建好，韌體只負責放照片進去。 */

/* 複製過程的回呼。呼叫端要在這裡餵看門狗（16 秒）並更新進度顯示。
 * 每 64KB 呼叫一次，2MB 的照片約 32 次。 */
typedef void (*fav_progress_fn)(uint32_t done, uint32_t total);

/* drive 是 FATFS_LinkDriver 給的字串（"0:/"），fs 是相簿那個 FATFS 物件 ——
 * 切換讀寫模式要重新掛載，得用同一個。 */
void fav_init(FATFS *fs, const char *drive);

/* 這張照片本身就在最愛資料夾裡嗎？
 *
 * 是的話愛心要**不可按**：刪掉的就是正在看的檔案，而且沒有來源可以復原。
 * 這是整個功能裡唯一會真的遺失資料的情況。 */
bool fav_source_is_in_folder(const char *src);

/* 已經收藏了嗎（副本存不存在）。不維護索引，直接 f_stat 算出來的路徑。 */
bool fav_exists(const char *src);

fav_result_t fav_add(const char *src, fav_progress_fn progress);
fav_result_t fav_remove(const char *src);

#endif /* FAVORITES_H_INCLUDED */
