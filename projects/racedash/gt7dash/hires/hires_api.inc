/* racedash 高解析度鏡像：給 LovyanGFX 用的介面。
 *
 * 由 lgfx/v1/LGFXBase.hpp 在 namespace lgfx::v1 裡面 include（只在 LGFX_RACEDASH 時）。
 * 做法見 hires.cpp 開頭。 */

  class LGFXBase;
  struct IFont;
  struct TextStyle;
  struct FontMetrics;

  namespace racedash
  {
    struct Hi;

    /* 2 倍畫布（寬高都是原本的兩倍，位元組順序跟原本一樣是 swap565）。沒有就回 nullptr。 */
    const uint16_t* buffer(const Hi* hi);
    /* 上次呼叫之後 2 倍畫布改過的列 [y0, y1)；沒改回 false。 */
    bool take_dirty(Hi* hi, int32_t* y0, int32_t* y1);
    /* 之後建立的畫布要不要配 2 倍鏡像（預設要）。比對用。 */
    void set_enabled(bool on);

    /* 高階繪圖（圓、圓角框、三角形、斜線、字）開始時呼叫：暫停低階的「放大兩倍」鏡像，
     * 原本的 320x240 照畫，畫完再用抗鋸齒重畫一次到 2 倍畫布。
     * hi 為空、或已經在另一個高階繪圖裡面時回 false（那就照一般流程走）。 */
    bool begin(Hi* hi);
    void end(Hi* hi);

    void fill_circle(LGFXBase* g, int32_t x, int32_t y, int32_t r);
    void draw_circle(LGFXBase* g, int32_t x, int32_t y, int32_t r);
    void fill_round_rect(LGFXBase* g, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r);
    void draw_round_rect(LGFXBase* g, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r);
    void fill_triangle(LGFXBase* g, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2);
    void draw_line(LGFXBase* g, int32_t x0, int32_t y0, int32_t x1, int32_t y1);

    /* 取代 font->drawChar(gfx, ...)：原本的字照畫，再用字型輪廓畫 2 倍的抗鋸齒字形。 */
    size_t draw_char(LGFXBase* g, const IFont* font, int32_t x, int32_t y, uint16_t c,
                     const TextStyle* style, FontMetrics* metrics, int32_t& filled_x);
  }

/* 高階繪圖函式開頭用：條件成立且不在其他高階繪圖裡面時，先遞迴呼叫自己畫原本的 1 倍
 * （這時鏡像暫停），再畫 2 倍的抗鋸齒版本。 */
#define LGFX_RACEDASH_SHAPE(cond, call_1x, call_hi) \
    if ((cond) && racedash::begin(_racedash_hi)) {   \
      call_1x;                                       \
      racedash::end(_racedash_hi);                   \
      racedash::call_hi;                             \
      return;                                        \
    }
