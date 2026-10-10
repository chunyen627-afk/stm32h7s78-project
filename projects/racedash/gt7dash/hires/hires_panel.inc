/* racedash 高解析度鏡像：記憶體畫布的低階寫入。
 *
 * 由 lgfx/v1/LGFX_Sprite.hpp 在 namespace lgfx::v1 裡面 include（只在 LGFX_RACEDASH 時），
 * 緊接在 Panel_Sprite 之後。LGFX_Sprite 改用這個子類別：每一次寫進 320x240 畫布，
 * 同一塊也寫進 2 倍畫布（放大兩倍），所以沒有特別處理的繪圖也一定會出現。 */

  namespace racedash
  {
    /* 16 位元畫布配一張兩倍大的鏡像；其他色深回 nullptr。cur 是原本的（可重用）。 */
    Hi*  attach(Hi* cur, int32_t w, int32_t h, int bits);
    void detach(Hi* hi);
    /* 1 倍座標的一塊填色（原本畫布已經畫好）。 */
    void fill(Hi* hi, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t raw);
    /* 1 倍座標的一塊照原本畫布的內容放大兩倍（貼圖等）。 */
    void sync(Hi* hi, const void* buf, int32_t stride, int32_t x, int32_t y, int32_t w, int32_t h);
    /* 小畫布貼到大畫布：兩邊都有鏡像就直接貼 2 倍的內容。 */
    bool push_begin(Hi* dst, Hi* src);
    void push_end(Hi* dst, Hi* src, LGFXBase* dst_gfx, int32_t x, int32_t y, uint32_t transp);
  }

  struct Panel_SpriteHi : public Panel_Sprite
  {
    racedash::Hi* hi = nullptr;

    void drawPixelPreclipped(uint_fast16_t x, uint_fast16_t y, uint32_t raw) override
    {
      Panel_Sprite::drawPixelPreclipped(x, y, raw);
      if (hi && !_rotation) racedash::fill(hi, x, y, 1, 1, raw);
    }
    void writeFillRectPreclipped(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, uint32_t raw) override
    {
      Panel_Sprite::writeFillRectPreclipped(x, y, w, h, raw);
      if (hi && !_rotation) racedash::fill(hi, x, y, w, h, raw);
    }
    void writeBlock(uint32_t raw, uint32_t len) override
    {
      auto y0 = _ypos;
      Panel_Sprite::writeBlock(raw, len);
      sync_rows(y0);
    }
    void writePixels(pixelcopy_t* param, uint32_t len, bool use_dma) override
    {
      auto y0 = _ypos;
      Panel_Sprite::writePixels(param, len, use_dma);
      sync_rows(y0);
    }
    void writeImage(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, pixelcopy_t* param, bool use_dma) override
    {
      Panel_Sprite::writeImage(x, y, w, h, param, use_dma);
      sync_rect(x, y, w, h);
    }
    void writeImageARGB(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, pixelcopy_t* param) override
    {
      Panel_Sprite::writeImageARGB(x, y, w, h, param);
      sync_rect(x, y, w, h);
    }
    void copyRect(uint_fast16_t dst_x, uint_fast16_t dst_y, uint_fast16_t w, uint_fast16_t h, uint_fast16_t src_x, uint_fast16_t src_y) override
    {
      Panel_Sprite::copyRect(dst_x, dst_y, w, h, src_x, src_y);
      sync_rect(dst_x, dst_y, w, h);
    }

  private:
    void sync_rect(int32_t x, int32_t y, int32_t w, int32_t h)
    {
      if (hi && !_rotation) racedash::sync(hi, getBuffer(), _bitwidth, x, y, w, h);
    }
    /* 視窗串流寫入：從寫入前的那一行到現在這一行（繞回來就整個視窗）。 */
    void sync_rows(uint_fast16_t y0)
    {
      if (!hi) return;
      uint_fast16_t y1 = _ypos;
      if (y1 < y0) { y0 = _ys; y1 = _ye; }
      if (y1 > _ye) y1 = _ye;
      sync_rect(_xs, y0, _xe - _xs + 1, y1 - y0 + 1);
    }
  };
