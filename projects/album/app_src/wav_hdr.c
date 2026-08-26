/**
 * WAV 檔頭解析。說明見 wav_hdr.h。
 *
 * 這份是從 audio_out.c 的 wav_parse() 原封搬過來的（只把寫全域改成
 * 填結構），行為刻意保持一模一樣 —— 那條路已經被使用者聽過很多次，
 * 抽出來的時候不該順手「改進」它。
 */
#include "wav_hdr.h"

#include <string.h>

bool wav_hdr_parse(FIL *fp, wav_hdr_t *out)
{
    uint8_t  hdr[12];
    UINT     got = 0;
    uint32_t guard;
    bool     have_fmt = false;

    if (fp == NULL || out == NULL) { return false; }

    if (f_read(fp, hdr, 12u, &got) != FR_OK || got != 12u) { return false; }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        return false;
    }

    for (guard = 0; guard < 32u; guard++) {   /* 有上限，壞檔不會讓它繞不出來 */
        uint8_t  ck[8];
        uint32_t len;

        if (f_read(fp, ck, 8u, &got) != FR_OK || got != 8u) { return false; }
        len = (uint32_t)ck[4] | ((uint32_t)ck[5] << 8) |
              ((uint32_t)ck[6] << 16) | ((uint32_t)ck[7] << 24);

        if (memcmp(ck, "fmt ", 4) == 0) {
            uint8_t f[16];

            if (len < 16u || f_read(fp, f, 16u, &got) != FR_OK || got != 16u) {
                return false;
            }
            out->rate     = (uint32_t)f[4] | ((uint32_t)f[5] << 8) |
                            ((uint32_t)f[6] << 16) | ((uint32_t)f[7] << 24);
            out->channels = (uint16_t)((uint16_t)f[2] | ((uint16_t)f[3] << 8));
            out->bits     = (uint16_t)((uint16_t)f[14] | ((uint16_t)f[15] << 8));
            have_fmt = true;
            if (len > 16u && f_lseek(fp, f_tell(fp) + (len - 16u)) != FR_OK) {
                return false;
            }
        } else if (memcmp(ck, "data", 4) == 0) {
            if (!have_fmt) { return false; }
            out->data_len = len;
            out->data_off = (uint32_t)f_tell(fp);
            return true;                      /* 檔案指標正好停在資料開頭 */
        } else {
            /* 區塊是偶數對齊的，奇數長度後面會補一個位元組。 */
            if (f_lseek(fp, f_tell(fp) + len + (len & 1u)) != FR_OK) {
                return false;
            }
        }
    }
    return false;
}
