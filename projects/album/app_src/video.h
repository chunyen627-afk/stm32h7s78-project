#ifndef VIDEO_H_INCLUDED
#define VIDEO_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

/* MJPEG 影片播放。
 *
 * 這一層只負責「把第 N 格解出來畫進指定的 framebuffer」，
 * 播放迴圈、選單、進度條、seek 都留在 album_main.c —— 那裡本來就有
 * present()、gfx、觸控與看門狗，搬過來只會變成兩份。
 *
 * 影格包由 projects/video/tools/mp4pack.py 產生（格式見該檔說明）。
 * 板子上沒有解多工器也沒有 H.264 解碼器，所以不是 mp4 而是預先拆好的
 * JPEG 序列加一張位移表 —— 位移表讓任何一格都能直接定址，seek 幾乎免費。
 *
 * ── 與照片路徑共存 ────────────────────────────────────────
 * 照片走**輪詢**解碼、影片走 **DMA** 分塊解碼，而 HAL_JPEG_*Callback 是全域
 * 符號，兩者不能各自定義。做法是讓 photo.c 的回呼在影片播放時轉交給這裡
 * （video_jpeg_active() 為真時），影片沒在播就完全等於不存在。
 *
 * board-notes 16.6 記過「DMA 解碼整合進相簿正式路徑就把週邊弄壞」，
 * 所以 video_close() 一定要做完整拆除並把照片路徑的解碼器重新初始化。
 */

#define VIDEO_PATH_MAX  160
#define VIDEO_NAME_MAX  64

typedef struct {
    char     path[VIDEO_PATH_MAX];
    char     name[VIDEO_NAME_MAX];
    uint32_t count;        /* 影格數 */
    uint32_t width;
    uint32_t height;
    uint32_t fps_x100;
    uint32_t max_size;     /* 最大單格 bytes */
} video_info_t;

/* 只讀檔頭（24 bytes）驗證這是不是有效的影格包。掃描整張卡時會呼叫很多次，
 * 所以刻意不碰位移表。 */
bool video_probe(const char *path, video_info_t *info);

/* 開檔並把位移表讀進 PSRAM。成功之後才能呼叫 video_decode()。 */
bool video_open(const video_info_t *info);

/* 解出第 idx 格並轉成 RGB565 寫進 dst（必須是 800x480 的 framebuffer）。
 * 失敗時 dst 內容不保證，呼叫端跳過該格即可。 */
bool video_decode(uint32_t idx, uint8_t *dst);

/* 關檔並把 JPEG 週邊完整還給照片路徑。**離開影片模式一定要呼叫。** */
void video_close(void);

/* 給 photo.c 的回呼分派用。影片沒在播時 video_jpeg_active() 回 false，
 * photo.c 的行為就跟沒有影片這回事一模一樣。 */
bool video_jpeg_active(void);
void video_jpeg_data_ready(void *hjpeg, uint8_t *pDataOut, uint32_t len);
void video_jpeg_get_data(void *hjpeg, uint32_t nb_decoded);

/* 診斷（SWD 讀）。 */
extern volatile uint32_t g_vdbg_decoded;
extern volatile uint32_t g_vdbg_fail;
extern volatile int32_t  g_vdbg_lasterr;
extern volatile uint32_t g_vdbg_us_read;
extern volatile uint32_t g_vdbg_us_dec;
extern volatile uint32_t g_vdbg_us_cc;

#endif /* VIDEO_H_INCLUDED */
