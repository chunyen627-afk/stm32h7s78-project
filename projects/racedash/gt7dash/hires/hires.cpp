/* racedash 高解析度、抗鋸齒：原作畫的每樣東西，再畫一份 2 倍大的。
 *
 * 原作（與 LovyanGFX）照舊畫在 320x240 的畫布上 —— 排版、量字寬、readPixel 都看那一張，
 * 行為跟原作一模一樣。每張 16 位元畫布另外配一張 2 倍大的鏡像（Hi）：
 *
 *  - 低階寫入（填色、貼圖）一律放大兩倍寫進鏡像。沒有特別處理的繪圖至少跟以前一樣，
 *    不會漏畫；長方形、水平垂直線本來就沒有鋸齒，放大兩倍就是正確答案。
 *  - 圓、圓角框、三角形、斜線：把原本 1 倍的幾何換算成 2 倍（1 倍的像素 (x,y) 佔
 *    2 倍的 [2x,2x+2)），用 stb_truetype 的掃描線（算面積覆蓋率）畫抗鋸齒。
 *  - 字：每個字的位置、字距都照原本 1 倍算出來的；字形換成原始字型（FreeSans、DejaVu）
 *    的輪廓，以 2 倍大小畫抗鋸齒。原作的 FreeSans/DejaVu 點陣字就是從這兩套字型轉出來的。
 *    沒有輪廓可用的點陣字（Font0/2/4/7）用 EPX 放大四倍再取樣，形狀不變、斜邊變平滑。
 *
 * 板子顯示的是 tft 那張畫布的鏡像（640x480）。 */
#include "lgfx_racedash.hpp"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((void)0)
#include "stb_truetype.h"

namespace
{
  struct HiGlyph { int32_t off; int16_t n; int16_t adv; int16_t x0, y0, x1, y1; };
  struct HiFace  { int upem; const stbtt_vertex* v; const HiGlyph* g; };
#include "hires_fonts.inc"
}

namespace lgfx
{
 inline namespace v1
 {
  namespace racedash
  {
    struct Hi
    {
      uint16_t* buf;
      int32_t w, h;       // 2 倍的寬高
      int sup;            // >0：高階繪圖進行中，低階鏡像暫停
      bool text;          // 原本的字正在畫：前景色的填色不放大（換成抗鋸齒字形）
      bool fillbg;
      uint16_t fg, bg;
      int32_t dy0, dy1;   // 上次取走之後改過的列 [dy0, dy1)（板子只貼這幾列）
    };

    static inline void mark(Hi* hi, int32_t y0, int32_t y1)
    {
      if (y0 >= y1) return;
      if (hi->dy0 >= hi->dy1) { hi->dy0 = y0; hi->dy1 = y1; return; }
      if (y0 < hi->dy0) hi->dy0 = y0;
      if (y1 > hi->dy1) hi->dy1 = y1;
    }

    static bool g_enabled = true;
    static void lines_flush(void);   // 還沒畫的斜線（見 draw_line）

    void set_enabled(bool on) { g_enabled = on; }
    const uint16_t* buffer(const Hi* hi) { lines_flush(); return hi ? hi->buf : nullptr; }

    bool take_dirty(Hi* hi, int32_t* y0, int32_t* y1)
    {
      lines_flush();
      if (!hi || hi->dy0 >= hi->dy1) return false;
      *y0 = hi->dy0; *y1 = hi->dy1;
      hi->dy0 = hi->dy1 = 0;
      return true;
    }

//----------------------------------------------------------------------------
// 鏡像本體

    void detach(Hi* hi)
    {
      if (!hi) return;
      lines_flush();
      free(hi->buf);
      free(hi);
    }

    Hi* attach(Hi* cur, int32_t w, int32_t h, int bits)
    {
      lines_flush();
      if (!g_enabled || bits != 16) { detach(cur); return nullptr; }
      int32_t W = w * 2, H = h * 2;
      if (cur && cur->w == W && cur->h == H)
      {
        memset(cur->buf, 0, (size_t)W * H * 2);   // 原本的畫布重建時也清成 0
        mark(cur, 0, H);
        return cur;
      }
      detach(cur);
      Hi* hi = (Hi*)calloc(1, sizeof(Hi));
      if (!hi) return nullptr;
      hi->buf = (uint16_t*)calloc((size_t)W * H, 2);
      if (!hi->buf) { free(hi); return nullptr; }
      hi->w = W;
      hi->h = H;
      mark(hi, 0, H);
      return hi;
    }

    static void hfill(Hi* hi, int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
    {
      if (x < 0) { w += x; x = 0; }
      if (y < 0) { h += y; y = 0; }
      if (x + w > hi->w) w = hi->w - x;
      if (y + h > hi->h) h = hi->h - y;
      if (w <= 0 || h <= 0) return;
      mark(hi, y, y + h);
      // 鏡像的座標都是偶數（1 倍座標 x2），一次寫兩個像素
      const uint32_t c2 = (uint32_t)c << 16 | c;
      for (int32_t j = 0; j < h; ++j)
      {
        uint16_t* d = hi->buf + (size_t)(y + j) * hi->w + x;
        int32_t i = 0;
        if ((uintptr_t)d & 2) { d[0] = c; i = 1; }
        auto d32 = (uint32_t*)(d + i);
        int32_t n2 = (w - i) >> 1;
        for (int32_t k = 0; k < n2; ++k) d32[k] = c2;
        if (i + n2 * 2 < w) d[w - 1] = c;
      }
    }

    void fill(Hi* hi, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t raw)
    {
      if (hi->sup) return;
      lines_flush();
      uint16_t c = raw;
      if (hi->text && c == hi->fg)
      {
        if (!hi->fillbg) return;     // 透明底的字：前景交給抗鋸齒字形
        c = hi->bg;                  // 有底色的字：字的位置先鋪底色，再疊字形
      }
      hfill(hi, x * 2, y * 2, w * 2, h * 2, c);
    }

    void sync(Hi* hi, const void* buf, int32_t stride, int32_t x, int32_t y, int32_t w, int32_t h)
    {
      if (hi->sup || !buf) return;
      lines_flush();
      int32_t lw = hi->w >> 1, lh = hi->h >> 1;
      if (x < 0) { w += x; x = 0; }
      if (y < 0) { h += y; y = 0; }
      if (x + w > lw) w = lw - x;
      if (y + h > lh) h = lh - y;
      if (w <= 0 || h <= 0) return;
      auto s = (const uint16_t*)buf;
      mark(hi, y * 2, (y + h) * 2);
      for (int32_t j = 0; j < h; ++j)
      {
        const uint16_t* row = s + (size_t)(y + j) * stride + x;
        uint16_t* d0 = hi->buf + (size_t)(y + j) * 2 * hi->w + x * 2;
        uint16_t* d1 = d0 + hi->w;
        for (int32_t i = 0; i < w; ++i)
        {
          uint16_t v = row[i];
          d0[i * 2] = d0[i * 2 + 1] = d1[i * 2] = d1[i * 2 + 1] = v;
        }
      }
    }

    bool push_begin(Hi* dst, Hi* src)
    {
      if (!dst || !src || dst->sup) return false;
      lines_flush();
      dst->sup++;
      return true;
    }

    void push_end(Hi* dst, Hi* src, LGFXBase* g, int32_t x, int32_t y, uint32_t transp)
    {
      dst->sup--;
      int32_t cx, cy, cw, ch;
      g->getClipRect(&cx, &cy, &cw, &ch);
      int32_t X0 = std::max(x * 2, cx * 2), Y0 = std::max(y * 2, cy * 2);
      int32_t X1 = std::min(x * 2 + src->w, (cx + cw) * 2), Y1 = std::min(y * 2 + src->h, (cy + ch) * 2);
      X1 = std::min(X1, dst->w);
      Y1 = std::min(Y1, dst->h);
      bool key = transp != pixelcopy_t::NON_TRANSP;
      mark(dst, Y0, Y1);
      for (int32_t Y = Y0; Y < Y1; ++Y)
      {
        const uint16_t* s = src->buf + (size_t)(Y - y * 2) * src->w - x * 2;
        uint16_t* d = dst->buf + (size_t)Y * dst->w;
        if (!key) { memcpy(d + X0, s + X0, (X1 - X0) * 2); continue; }
        for (int32_t X = X0; X < X1; ++X)
          if (s[X] != (uint16_t)transp) d[X] = s[X];
      }
    }

    bool begin(Hi* hi)
    {
      if (!hi || hi->sup) return false;
      hi->sup++;
      return true;
    }

    void end(Hi* hi) { hi->sup--; }

//----------------------------------------------------------------------------
// 抗鋸齒填色

    static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

    /* d、c 都是 swap565；a = 0~255 覆蓋率 */
    static inline uint16_t blend(uint16_t d, uint16_t c, unsigned a)
    {
      if (a >= 255) return c;
      if (a == 0) return d;
      unsigned D = bswap16(d), C = bswap16(c), b = 255 - a;
      // x / 255 ≈ (x * 257 + 257) >> 16（x ≤ 255*63，誤差不到 1）
      unsigned r = (((C >> 11) * a + (D >> 11) * b) * 257 + 257) >> 16;
      unsigned gg = ((((C >> 5) & 63) * a + ((D >> 5) & 63) * b) * 257 + 257) >> 16;
      unsigned bl = (((C & 31) * a + (D & 31) * b) * 257 + 257) >> 16;
      return bswap16((uint16_t)(r << 11 | gg << 5 | bl));
    }

    /* 覆蓋率圖 (w x h) 疊到 2 倍畫布的 (X, Y)，限制在 1 倍裁切框的兩倍內。 */
    static void blit_cov(LGFXBase* g, Hi* hi, const uint8_t* a, int32_t w, int32_t h,
                         int32_t X, int32_t Y, uint16_t color)
    {
      int32_t cx, cy, cw, ch;
      g->getClipRect(&cx, &cy, &cw, &ch);
      int32_t x0 = std::max<int32_t>(X, cx * 2), y0 = std::max<int32_t>(Y, cy * 2);
      int32_t x1 = std::min<int32_t>(X + w, std::min<int32_t>((cx + cw) * 2, hi->w));
      int32_t y1 = std::min<int32_t>(Y + h, std::min<int32_t>((cy + ch) * 2, hi->h));
      if (x0 < x1) mark(hi, y0, y1);
      for (int32_t yy = y0; yy < y1; ++yy)
      {
        const uint8_t* s = a + (size_t)(yy - Y) * w - X;
        uint16_t* d = hi->buf + (size_t)yy * hi->w;
        for (int32_t xx = x0; xx < x1; ++xx)
          if (s[xx]) d[xx] = blend(d[xx], color, s[xx]);
      }
    }

    /* 多邊形路徑，座標用 1/16 像素的整數存（stbtt_vertex 是 short）。 */
    static constexpr int PATH_VMAX = 4096;
    static constexpr float SUB = 16.0f;
    struct Path
    {
      stbtt_vertex* v = nullptr;   // 64KB，第一次用到才從堆積（PSRAM）配
      int n = 0;
      float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
      void reset(void) { n = 0; x0 = y0 = 1e9f; x1 = y1 = -1e9f; }
      void add(float x, float y, unsigned char type)
      {
        if (!v && !(v = (stbtt_vertex*)malloc(sizeof(stbtt_vertex) * PATH_VMAX))) return;
        if (n >= PATH_VMAX) return;
        stbtt_vertex& p = v[n++];
        memset(&p, 0, sizeof(p));
        p.x = (short)lrintf(x * SUB);
        p.y = (short)lrintf(y * SUB);
        p.type = type;
        x0 = std::min(x0, x); x1 = std::max(x1, x);
        y0 = std::min(y0, y); y1 = std::max(y1, y);
      }
      /* 一條封閉輪廓；reverse = 反方向（挖洞用） */
      void contour(const float* xy, int count, bool reverse)
      {
        for (int i = 0; i < count; ++i)
        {
          int k = reverse ? count - 1 - i : i;
          add(xy[k * 2], xy[k * 2 + 1], i ? STBTT_vline : STBTT_vmove);
        }
      }
    };
    static Path s_path;
    static float s_pts[2 * 1024];   // 一條輪廓的點（圓最多 360 點、圓角框 260 點）

    static void path_fill(LGFXBase* g, Path& p, uint16_t color)
    {
      Hi* hi = g->_racedash_hi;
      if (!hi || p.n < 3) return;
      int32_t cx, cy, cw, ch;
      g->getClipRect(&cx, &cy, &cw, &ch);
      int32_t X0 = std::max<int32_t>((int32_t)floorf(p.x0), std::max<int32_t>(cx * 2, 0));
      int32_t Y0 = std::max<int32_t>((int32_t)floorf(p.y0), std::max<int32_t>(cy * 2, 0));
      int32_t X1 = std::min<int32_t>((int32_t)ceilf(p.x1), std::min<int32_t>((cx + cw) * 2, hi->w));
      int32_t Y1 = std::min<int32_t>((int32_t)ceilf(p.y1), std::min<int32_t>((cy + ch) * 2, hi->h));
      if (X0 >= X1 || Y0 >= Y1) return;
      int32_t w = X1 - X0, h = Y1 - Y0;
      auto a = (uint8_t*)malloc((size_t)w * h);
      if (!a) return;
      stbtt__bitmap bm;
      bm.w = w; bm.h = h; bm.stride = w; bm.pixels = a;
      stbtt_Rasterize(&bm, 0.25f, p.v, p.n, 1.0f / SUB, 1.0f / SUB, 0, 0, X0, Y0, 0, nullptr);
      blit_cov(g, hi, a, w, h, X0, Y0, color);
      free(a);
    }

    static inline uint16_t raw_color(LGFXBase* g) { return (uint16_t)g->getRawColor(); }

    static int circle_points(float cx, float cy, float r, float* out)
    {
      int n = (int)(sqrtf(r) * 10.0f);
      n = std::max(16, std::min(n, 360));
      for (int i = 0; i < n; ++i)
      {
        float t = 6.28318531f * i / n;
        out[i * 2] = cx + r * cosf(t);
        out[i * 2 + 1] = cy + r * sinf(t);
      }
      return n;
    }

    /* 圓角矩形 [x0,x1]x[y0,y1]、圓角半徑 r（2 倍座標） */
    static int round_rect_points(float x0, float y0, float x1, float y1, float r, float* out)
    {
      int n = 0;
      if (r <= 0.0f)
      {
        float q[8] = { x0, y0, x1, y0, x1, y1, x0, y1 };
        memcpy(out, q, sizeof(q));
        return 4;
      }
      int seg = std::max(2, std::min((int)(sqrtf(r) * 4.0f), 64));
      const float cxs[4] = { x0 + r, x1 - r, x1 - r, x0 + r };
      const float cys[4] = { y0 + r, y0 + r, y1 - r, y1 - r };
      for (int k = 0; k < 4; ++k)
      {
        float a0 = 3.14159265f + k * 1.57079633f;   // 左上 180°→270°、右上、右下、左下（畫面座標 y 向下）
        for (int i = 0; i <= seg; ++i)
        {
          float t = a0 + 1.57079633f * i / seg;
          out[n * 2] = cxs[k] + r * cosf(t);
          out[n * 2 + 1] = cys[k] + r * sinf(t);
          ++n;
        }
      }
      return n;
    }

//----------------------------------------------------------------------------
// 圖形：原本 1 倍的幾何換算成 2 倍。1 倍的像素 (x,y) 佔 2 倍的 [2x,2x+2)，中心 2x+1。

    void fill_circle(LGFXBase* g, int32_t x, int32_t y, int32_t r)
    {
      lines_flush();
      // 原本：以 (x,y) 為中心、直徑 2r+1 個像素
      s_path.reset();
      int n = circle_points(x * 2 + 1.0f, y * 2 + 1.0f, r * 2 + 1.0f, s_pts);
      s_path.contour(s_pts, n, false);
      path_fill(g, s_path, raw_color(g));
    }

    void draw_circle(LGFXBase* g, int32_t x, int32_t y, int32_t r)
    {
      lines_flush();
      // 原本：1 像素寬的圓周，半徑 r → 2 倍是半徑 2r-1 到 2r+1 的圓環
      s_path.reset();
      int n = circle_points(x * 2 + 1.0f, y * 2 + 1.0f, r * 2 + 1.0f, s_pts);
      s_path.contour(s_pts, n, false);
      n = circle_points(x * 2 + 1.0f, y * 2 + 1.0f, r * 2 - 1.0f, s_pts);
      s_path.contour(s_pts, n, true);
      path_fill(g, s_path, raw_color(g));
    }

    /* 原本的圓角：角的圓心在 (x+r, y+r) 那一格、半徑 r → 2 倍的圓角半徑 2r+1，
     * 剛好貼齊 2 倍的外框。太大就縮到短邊的一半。 */
    static float corner(int32_t w, int32_t h, int32_t r)
    {
      return std::min<float>(r * 2 + 1.0f, (float)std::min(w, h));
    }

    void fill_round_rect(LGFXBase* g, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r)
    {
      lines_flush();
      s_path.reset();
      int n = round_rect_points(x * 2.0f, y * 2.0f, (x + w) * 2.0f, (y + h) * 2.0f, corner(w, h, r), s_pts);
      s_path.contour(s_pts, n, false);
      path_fill(g, s_path, raw_color(g));
    }

    void draw_round_rect(LGFXBase* g, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r)
    {
      lines_flush();
      s_path.reset();
      float rr = corner(w, h, r);
      int n = round_rect_points(x * 2.0f, y * 2.0f, (x + w) * 2.0f, (y + h) * 2.0f, rr, s_pts);
      s_path.contour(s_pts, n, false);
      if (w > 2 && h > 2)
      {
        n = round_rect_points(x * 2 + 2.0f, y * 2 + 2.0f, (x + w) * 2 - 2.0f, (y + h) * 2 - 2.0f,
                              std::max(0.0f, rr - 2.0f), s_pts);
        s_path.contour(s_pts, n, true);
      }
      path_fill(g, s_path, raw_color(g));
    }

    /* 原本的三角形每一行從左邊界那格填到右邊界那格（含），等於頂點在像素中心、
     * 往外多半格。2 倍就是頂點 (2x+1, 2y+1) 往外推 1 像素。共用一條邊的兩個三角形
     * 因此會重疊，不會在接縫露出細縫。 */
    void fill_triangle(LGFXBase* g, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
    {
      lines_flush();
      float px[3] = { x0 * 2 + 1.0f, x1 * 2 + 1.0f, x2 * 2 + 1.0f };
      float py[3] = { y0 * 2 + 1.0f, y1 * 2 + 1.0f, y2 * 2 + 1.0f };
      float cx = (px[0] + px[1] + px[2]) / 3, cy = (py[0] + py[1] + py[2]) / 3;
      float nx[3], ny[3];   // 第 i 條邊（i → i+1）的外法向量
      for (int i = 0; i < 3; ++i)
      {
        int j = (i + 1) % 3;
        float dx = px[j] - px[i], dy = py[j] - py[i];
        float len = sqrtf(dx * dx + dy * dy);
        if (len <= 0) return;
        nx[i] = -dy / len; ny[i] = dx / len;
        float mx = (px[i] + px[j]) / 2 - cx, my = (py[i] + py[j]) / 2 - cy;
        if (nx[i] * mx + ny[i] * my < 0) { nx[i] = -nx[i]; ny[i] = -ny[i]; }
      }
      float out[6];
      for (int i = 0; i < 3; ++i)
      {
        int p = (i + 2) % 3;   // 進到頂點 i 的邊
        float sx = nx[p] + nx[i], sy = ny[p] + ny[i];
        float den = std::max(1.0f + nx[p] * nx[i] + ny[p] * ny[i], 0.125f);   // 尖角限制外推長度
        out[i * 2] = px[i] + sx / den;
        out[i * 2 + 1] = py[i] + sy / den;
      }
      s_path.reset();
      s_path.contour(out, 3, false);
      path_fill(g, s_path, raw_color(g));
    }

    /* 線：1 像素寬、兩端各含端點那一格 → 2 倍是寬 2、兩端各延長 1 的長條。
     *
     * 原作的圓弧是一串很短的線段，端點都四捨五入到整數像素；照原樣放大兩倍會看到
     * 一節一節的扭動。所以先不畫：同一張畫布、同一個顏色的連續線段收集起來（遇到別的
     * 繪圖再一起畫）。一串首尾相接、每段都很短（≤4 像素）的線段把端點的進位誤差平均掉
     * （前後各 3 點的移動平均，兩端不動），長的直線不受影響。同一批的線段畫成一個圖形，
     * 並排的兩條弧之間也不會因為各自抗鋸齒而出現暗縫。 */
    static constexpr int LINE_PTS = 1024, LINE_CHAINS = 256;
    struct Lines
    {
      Hi* hi = nullptr;
      LGFXBase* g = nullptr;
      uint16_t color = 0;
      int32_t clip[4] = { 0, 0, 0, 0 };
      int n = 0, nch = 0;
      int16_t x[LINE_PTS], y[LINE_PTS];
      int16_t start[LINE_CHAINS + 1];
    };
    static Lines s_lines;

    static void add_segment(float ax, float ay, float bx, float by, float hw)
    {
      float dx = bx - ax, dy = by - ay;
      float len = sqrtf(dx * dx + dy * dy);
      if (len <= 0) return;
      dx /= len; dy /= len;
      float nx = -dy * hw, ny = dx * hw;
      float q[8] = {
        ax - dx + nx, ay - dy + ny,
        bx + dx + nx, by + dy + ny,
        bx + dx - nx, by + dy - ny,
        ax - dx - nx, ay - dy - ny,
      };
      s_path.contour(q, 4, false);
    }

    static void lines_flush(void)
    {
      Lines& L = s_lines;
      if (!L.n) return;
      int n = L.n;
      L.n = 0;              // 先清掉：path_fill 不會再回來呼叫，但保險
      L.start[L.nch] = n;
      s_path.reset();
      static float* fx = nullptr;
      if (!fx && !(fx = (float*)malloc(sizeof(float) * LINE_PTS * 2))) return;
      float* fy = fx + LINE_PTS;
      for (int c = 0; c < L.nch; ++c)
      {
        int a = L.start[c], b = L.start[c + 1];   // 這一串是 [a, b)
        int cnt = b - a;
        bool smooth = cnt >= 6;
        for (int i = a + 1; i < b && smooth; ++i)
          if (abs(L.x[i] - L.x[i - 1]) > 4 || abs(L.y[i] - L.y[i - 1]) > 4) smooth = false;
        for (int i = 0; i < cnt; ++i)
        {
          int hw = smooth ? std::min(3, std::min(i, cnt - 1 - i)) : 0;
          float sx = 0, sy = 0;
          for (int k = -hw; k <= hw; ++k) { sx += L.x[a + i + k]; sy += L.y[a + i + k]; }
          fx[i] = sx / (2 * hw + 1) * 2 + 1.0f;
          fy[i] = sy / (2 * hw + 1) * 2 + 1.0f;
        }
        for (int i = 1; i < cnt; ++i)
        {
          if (s_path.n > PATH_VMAX - 8) { path_fill(L.g, s_path, L.color); s_path.reset(); }
          // 圓弧（平滑過的那種）寬 1.3 倍：原作用好幾條相鄰半徑的弧疊成粗弧，各自進位後
          // 彼此之間會有不到半像素的縫（原作 1 倍時也看得到點狀的洞），加寬一點讓它們接起來
          add_segment(fx[i - 1], fy[i - 1], fx[i], fy[i], smooth ? 1.3f : 1.0f);
        }
      }
      path_fill(L.g, s_path, L.color);
      L.nch = 0;
    }

    void draw_line(LGFXBase* g, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
    {
      Lines& L = s_lines;
      uint16_t color = raw_color(g);
      int32_t clip[4];
      g->getClipRect(&clip[0], &clip[1], &clip[2], &clip[3]);
      bool same = L.n && L.hi == g->_racedash_hi && L.g == g && L.color == color && !memcmp(L.clip, clip, sizeof(clip));
      if (same && L.x[L.n - 1] == x0 && L.y[L.n - 1] == y0 && L.n < LINE_PTS)
      {
        L.x[L.n] = x1; L.y[L.n] = y1; ++L.n;     // 接在上一段後面
        return;
      }
      if (!same || L.n + 2 > LINE_PTS || L.nch >= LINE_CHAINS)
      {
        lines_flush();
        L.hi = g->_racedash_hi;
        L.g = g;
        L.color = color;
        memcpy(L.clip, clip, sizeof(clip));
      }
      L.start[L.nch++] = L.n;                     // 新的一串
      L.x[L.n] = x0; L.y[L.n] = y0; ++L.n;
      L.x[L.n] = x1; L.y[L.n] = y1; ++L.n;
    }

//----------------------------------------------------------------------------
// 字

    struct Src { const IFont* font; const HiFace* face; float ppem; };

    static const Src* find_src(const IFont* f)
    {
      // FreeSans：Adafruit fontconvert 以 141 dpi 轉出（ppem = 點數 x 141 / 72）；
      // DejaVu：點陣字型名稱的數字就是 ppem。實測與原點陣字形重疊（見 README）。
      static const Src tab[] = {
        { &fonts::FreeSans9pt7b,  &hf_freesans,  9 * 141 / 72.0f },
        { &fonts::FreeSans12pt7b, &hf_freesans, 12 * 141 / 72.0f },
        { &fonts::FreeSans18pt7b, &hf_freesans, 18 * 141 / 72.0f },
        { &fonts::FreeSans24pt7b, &hf_freesans, 24 * 141 / 72.0f },
        { &fonts::DejaVu9,  &hf_dejavu,  9 }, { &fonts::DejaVu12, &hf_dejavu, 12 },
        { &fonts::DejaVu18, &hf_dejavu, 18 }, { &fonts::DejaVu24, &hf_dejavu, 24 },
        { &fonts::DejaVu40, &hf_dejavu, 40 }, { &fonts::DejaVu56, &hf_dejavu, 56 },
        { &fonts::DejaVu72, &hf_dejavu, 72 },
        // 點陣字：沒有輪廓，EPX 放大
        { &fonts::Font0, nullptr, 0 }, { &fonts::Font2, nullptr, 0 },
        { &fonts::Font4, nullptr, 0 }, { &fonts::Font7, nullptr, 0 },
      };
      for (auto& s : tab) if (s.font == f) return &s;
      return nullptr;
    }

    /* 一個字在某個大小下的覆蓋率圖。(ox, oy) 是相對原點的位移：輪廓字的原點是
     * (筆位 x2, 基線 y 的整數部分)，點陣字是字格左上角 x2。 */
    struct Cov
    {
      const IFont* font;
      float sx, sy;
      uint16_t c;
      int16_t ox, oy, w, h;
      uint8_t a[1];
    };
    static constexpr int CACHE = 1024;
    static Cov* s_cache[CACHE];

    /* 點陣字形轉輪廓：沿像素邊界描出外框（只在角上相接的像素也算相連），再把連續兩個
     * 以上「兩邊都只有 1 格長」的轉角 —— 也就是 45° 的階梯 —— 換成穿過邊中點的斜線。
     * 有一邊長於 1 格的轉角是字本來的直角，保持方正；所以字的樣子不變，只是斜的筆畫
     * 不再是一格一格的鋸齒。結果寫進 s_path（字格內、以 2 倍像素為單位），回傳是否成功。 */
    static bool vectorize(const uint8_t* bm, int W, int H, float kx, float ky)
    {
      struct Edge { int16_t sx, sy, ex, ey, pix, next; };
      const int VW = W + 1;
      auto edges = (Edge*)malloc(sizeof(Edge) * W * H * 4 + sizeof(int16_t) * VW * (H + 1));
      if (!edges) return false;
      auto head = (int16_t*)(edges + W * H * 4);
      for (int i = 0; i < VW * (H + 1); ++i) head[i] = -1;
      int ne = 0;
      auto at = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && bm[y * W + x]; };
      auto add = [&](int sx, int sy, int ex, int ey, int pix) {
        Edge& e = edges[ne];
        e.sx = sx; e.sy = sy; e.ex = ex; e.ey = ey; e.pix = pix;
        e.next = head[sy * VW + sx];
        head[sy * VW + sx] = ne++;
      };
      // 每個實心像素沿邊界逆時針（畫面座標）：上邊往左、左邊往下、下邊往右、右邊往上
      for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
          if (!at(x, y)) continue;
          int pix = y * W + x;
          if (!at(x, y - 1)) add(x + 1, y, x, y, pix);
          if (!at(x - 1, y)) add(x, y, x, y + 1, pix);
          if (!at(x, y + 1)) add(x, y + 1, x + 1, y + 1, pix);
          if (!at(x + 1, y)) add(x + 1, y + 1, x + 1, y, pix);
        }

      auto used = (uint8_t*)calloc(ne ? ne : 1, 1);
      auto px = (int16_t*)malloc(sizeof(int16_t) * 2 * (ne + 1));
      if (!used || !px) { free(edges); free(used); free(px); return false; }
      int16_t* py = px + ne + 1;

      s_path.reset();
      for (int first = 0; first < ne; ++first)
      {
        if (used[first]) continue;
        // 描一圈：在某個頂點有兩條路可走時（兩個像素只在角上相接），走向另一個像素，
        // 讓斜角相接的像素連成一筆
        int n = 0, cur = first;
        do
        {
          used[cur] = 1;
          px[n] = edges[cur].sx; py[n] = edges[cur].sy; ++n;
          int v = edges[cur].ey * VW + edges[cur].ex, pick = -1;
          for (int e = head[v]; e >= 0; e = edges[e].next)
            if (pick < 0 || (edges[e].pix != edges[cur].pix && edges[pick].pix == edges[cur].pix)) pick = e;
          cur = pick;
        } while (cur >= 0 && !used[cur] && n < ne);

        // 去掉同一直線上的中間點，留下轉角
        int m = 0;
        for (int i = 0; i < n; ++i)
        {
          int a = (i + n - 1) % n, b = (i + 1) % n;
          int dx0 = px[i] - px[a], dy0 = py[i] - py[a], dx1 = px[b] - px[i], dy1 = py[b] - py[i];
          if (dx0 * dy1 - dy0 * dx1 == 0 && dx0 * dx1 + dy0 * dy1 > 0) continue;
          px[m] = px[i]; py[m] = py[i]; ++m;
        }
        if (m < 3) continue;

        static float* out = nullptr;
        if (!out && !(out = (float*)malloc(sizeof(float) * 2 * 1024))) break;
        int k = 0;
        auto len = [&](int i) { int j = (i + 1) % m; return abs(px[j] - px[i]) + abs(py[j] - py[i]); };
        auto cand = [&](int i) { return len((i + m - 1) % m) == 1 && len(i) == 1; };
        // 轉向：階梯是凸、凹交替；連續兩個同向的短轉角是突出的一格（例如「4」橫筆的尾巴），不切
        auto turn = [&](int i) {
          int a = (i + m - 1) % m, b = (i + 1) % m;
          return (px[i] - px[a]) * (py[b] - py[i]) - (py[i] - py[a]) * (px[b] - px[i]) > 0;
        };
        for (int i = 0; i < m && k < 1022; ++i)
        {
          int a = (i + m - 1) % m, b = (i + 1) % m;
          bool cut = m > 4 && cand(i) &&
                     ((cand(a) && turn(a) != turn(i)) || (cand(b) && turn(b) != turn(i)));
          if (cut)
          {
            out[k * 2] = (px[a] + px[i]) * 0.5f * kx; out[k * 2 + 1] = (py[a] + py[i]) * 0.5f * ky; ++k;
            out[k * 2] = (px[i] + px[b]) * 0.5f * kx; out[k * 2 + 1] = (py[i] + py[b]) * 0.5f * ky; ++k;
          }
          else
          {
            out[k * 2] = px[i] * kx; out[k * 2 + 1] = py[i] * ky; ++k;
          }
        }
        s_path.contour(out, k, false);
      }
      free(edges); free(used); free(px);
      return true;
    }

    static Cov* make_outline(const Src* src, uint16_t c, float sx, float sy, const FontMetrics* m)
    {
      const HiGlyph& G = src->face->g[c - 0x20];
      float k = src->ppem / src->face->upem;
      float scx = 2 * sx * k, scy = 2 * sy * k;
      // 原點陣字形比輪廓低 1 列（fontconvert 的 yOffset = 1 - bitmap_top）
      float K = 2 * sy * (1 - m->y_offset);
      float fb = K - floorf(K);
      int ix0 = 0, iy0 = 0, w = 0, h = 0;
      if (G.n)
      {
        ix0 = (int)floorf(G.x0 * scx);
        iy0 = (int)floorf(-G.y1 * scy + fb);
        w = (int)ceilf(G.x1 * scx) - ix0;
        h = (int)ceilf(-G.y0 * scy + fb) - iy0;
        if (w < 0 || h < 0) w = h = 0;
      }
      auto cv = (Cov*)malloc(sizeof(Cov) + (size_t)w * h);
      if (!cv) return nullptr;
      cv->ox = ix0;
      cv->oy = iy0;
      cv->w = w;
      cv->h = h;
      if (w && h)
      {
        stbtt__bitmap bm;
        bm.w = w; bm.h = h; bm.stride = w; bm.pixels = cv->a;
        stbtt_Rasterize(&bm, 0.35f, (stbtt_vertex*)(src->face->v + G.off), G.n,
                        scx, scy, 0, fb, ix0, iy0, 1, nullptr);
      }
      return cv;
    }

    static Cov* make_bitmap(const IFont* font, uint16_t c, float sx, float sy, const TextStyle* st)
    {
      FontMetrics mm;
      font->getDefaultMetric(&mm);
      font->updateFontMetric(&mm, c);
      int W = mm.width, H = mm.height;
      if (W <= 0 || H <= 0 || W > 64 || H > 64) return nullptr;

      // 用一張沒有鏡像的 8 位元小畫布，讓 LovyanGFX 自己把原本的字形畫出來
      static LGFX_Sprite* scratch = nullptr;
      if (!scratch) scratch = new LGFX_Sprite();
      scratch->setColorDepth(8);
      if (!scratch->createSprite(W, H)) return nullptr;
      scratch->fillScreen(0);
      TextStyle ts = *st;
      ts.fore_rgb888 = 0xFFFFFFu;
      ts.back_rgb888 = 0;
      ts.size_x = ts.size_y = 1;
      int32_t fx = 0;
      font->drawChar(scratch, 0, 0, c, &ts, &mm, fx);

      auto b1 = (uint8_t*)malloc((size_t)W * H);
      if (!b1) return nullptr;
      for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
          b1[y * W + x] = scratch->readPixelValue(x, y) ? 1 : 0;
      // 1 倍字格的一格 = 2 倍畫布的 2*size 像素
      bool ok = vectorize(b1, W, H, 2 * sx, 2 * sy);
      free(b1);
      if (!ok) return nullptr;

      int w = (int)ceilf(2 * W * sx - 0.001f), h = (int)ceilf(2 * H * sy - 0.001f);
      auto cv = (Cov*)malloc(sizeof(Cov) + (size_t)w * h);
      if (!cv) return nullptr;
      cv->ox = cv->oy = 0;
      cv->w = w;
      cv->h = h;
      if (s_path.n >= 3)
      {
        stbtt__bitmap bm;
        bm.w = w; bm.h = h; bm.stride = w; bm.pixels = cv->a;
        stbtt_Rasterize(&bm, 0.25f, s_path.v, s_path.n, 1.0f / SUB, 1.0f / SUB, 0, 0, 0, 0, 0, nullptr);
      }
      else memset(cv->a, 0, (size_t)w * h);
      return cv;
    }

    static Cov* glyph(const Src* src, uint16_t c, const TextStyle* st, const FontMetrics* m)
    {
      float sx = st->size_x, sy = st->size_y;
      uint32_t bx, by;
      memcpy(&bx, &sx, 4);
      memcpy(&by, &sy, 4);
      uint32_t hsh = (uint32_t)(uintptr_t)src->font * 2654435761u ^ c * 40503u ^ bx * 7919u ^ by * 104729u;
      Cov*& slot = s_cache[(hsh ^ (hsh >> 13)) % CACHE];
      if (slot && slot->font == src->font && slot->c == c && slot->sx == sx && slot->sy == sy) return slot;
      Cov* cv = src->face ? make_outline(src, c, sx, sy, m) : make_bitmap(src->font, c, sx, sy, st);
      if (!cv) return nullptr;
      cv->font = src->font;
      cv->c = c;
      cv->sx = sx;
      cv->sy = sy;
      free(slot);
      slot = cv;
      return cv;
    }

    size_t draw_char(LGFXBase* g, const IFont* font, int32_t x, int32_t y, uint16_t c,
                     const TextStyle* st, FontMetrics* m, int32_t& filled_x)
    {
      Hi* hi = g->_racedash_hi;
      const Src* src = (hi && !hi->sup && c >= 0x20 && c <= 0x7E) ? find_src(font) : nullptr;
      if (!src) return font->drawChar(g, x, y, c, st, m, filled_x);
      lines_flush();

      auto cc = g->getColorConverter();
      hi->fg = cc->convert(st->fore_rgb888);
      hi->bg = cc->convert(st->back_rgb888);
      hi->fillbg = st->fore_rgb888 != st->back_rgb888;
      hi->text = true;
      size_t adv = font->drawChar(g, x, y, c, st, m, filled_x);
      hi->text = false;

      Cov* cv = glyph(src, c, st, m);
      if (!cv || !cv->w || !cv->h) return adv;
      int32_t X, Y;
      if (src->face)
      {
        // 跟 GFXfont::drawChar 一樣先把 y 移到字的頂端，再加上基線
        int32_t isy = 65536 * st->size_y;
        int32_t yt = y + ((m->y_offset * isy) >> 16);
        X = x * 2 + cv->ox;
        Y = yt * 2 + (int32_t)floorf(2 * st->size_y * (1 - m->y_offset)) + cv->oy;
      }
      else
      {
        X = x * 2;
        Y = y * 2;
      }
      blit_cov(g, hi, cv->a, cv->w, cv->h, X, Y, hi->fg);
      return adv;
    }
  }
 }
}
