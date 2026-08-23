#ifndef FAVORITES_H_INCLUDED
#define FAVORITES_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#include "ff.h"

/* 最愛清單檔。放卡的根目錄，純文字，在電腦上打得開。
 *
 * 掃描器只收 .jpg/.jpeg 而且只收 depth > 0 的檔案，所以這個檔案不會被
 * 當成照片撈進播放清單。 */
#define FAV_FILE_NAME   "我的最愛.txt"

/* 固定寬度的紀錄，這是整個設計的重點 ——
 *
 * 每一筆佔固定的 FAV_REC_BYTES，所以「收藏／取消一張」只要 f_lseek 到那一格
 * 覆寫 256 個位元組，FatFs 就只做一個磁區的 read-modify-write。
 * 檔案大小從頭到尾不變 -> **不配置新簇、不釋放簇鏈、不動 FAT**。
 *
 * 這件事對這張卡是關鍵：複製一張 600KB 的照片要打約 1200 個磁區寫入，
 * 而它在連續寫入下會進 BUSY 就不再回應（board-notes 20）。
 * 改成清單之後每次收藏只有 2~3 個磁區。
 *
 * 版面刻意做成「一行一筆」：路徑後面用空白補到 254，最後接 CRLF，
 * 所以在電腦上用記事本打開就是一行一個路徑。空的格子整行都是空白。 */
#define FAV_REC_BYTES   256u
#define FAV_MAX         256u                              /* 最多收藏幾張 */
#define FAV_FILE_BYTES  (FAV_REC_BYTES * FAV_MAX)         /* 64 KB，固定 */

typedef enum {
    FAV_OK           =   0,
    FAV_ERR_NAME     =  -1,   /* 路徑太長，塞不進一筆紀錄 */
    FAV_ERR_FULL     =  -2,   /* 已經收藏滿 FAV_MAX 張 */
    FAV_ERR_NOTREADY =  -3,   /* 還沒 fav_init／清單沒載入 */
    FAV_ERR_OPEN     =  -4,
    FAV_ERR_SEEK     =  -5,
    FAV_ERR_WRITE    =  -6,
    FAV_ERR_CREATE   =  -7,   /* 清單檔不存在而且建不起來 */
    FAV_ERR_VERIFY   =  -8,   /* 寫回去再讀出來對不起來 */
} fav_result_t;

/* drive 是 FATFS_LinkDriver 給的字串（"0:/"）。掛載完、掃描前呼叫。
 * 會把清單檔讀進記憶體；檔案不存在就建一個空的。 */
void fav_init(const char *drive);

/* 清單已經可用（載入或建立成功）。false 的話愛心要顯示成不可按。 */
bool fav_ready(void);

/* 目前收藏了幾張。選單上那顆「最愛」要拿它決定亮不亮。 */
uint32_t fav_count(void);

/* 這張收藏了沒。**純記憶體查表**，不碰卡片，所以每張照片問一次也不費事。 */
bool fav_is(const char *path);

/* 收藏／取消。只寫清單檔裡的那一格（一個磁區）。 */
fav_result_t fav_add(const char *path);
fav_result_t fav_remove(const char *path);

#endif /* FAVORITES_H_INCLUDED */
