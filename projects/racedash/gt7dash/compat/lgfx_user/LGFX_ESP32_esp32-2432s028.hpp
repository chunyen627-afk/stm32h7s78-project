/* 原作這裡是 ESP32-2432S028 面板的設定。racedash 改成一張 320x240 的記憶體
 * 畫布：原作畫什麼都畫在這裡，再由 racedash 放大兩倍貼到 7 吋面板上。 */
#pragma once
#include "../LovyanGFX.hpp"

/* 原作面板的旋轉設定；畫布不轉，值只是讓程式編得過。 */
#define DASHBOARD_DISPLAY_ROTATION 1

/* 觸控與背光由 racedash 的主程式提供（觸控座標已換算成 320x240）。 */
extern "C" bool racedash_touch(int *x, int *y);
extern "C" void racedash_backlight(unsigned char brightness);

class LGFX : public lgfx::LGFX_Sprite
{
public:
    bool init()
    {
        if (getBuffer() == nullptr) {
            setColorDepth(16);
            createSprite(320, 240);
        }
        return getBuffer() != nullptr;
    }
    bool begin() { return init(); }
    using lgfx::LGFX_Sprite::print;
    size_t print(const String &s) { return lgfx::LGFX_Sprite::print(s.c_str()); }
    void setBrightness(uint8_t b) { racedash_backlight(b); }
    /* 畫布就是橫向 320x240；原作的旋轉是給實體面板的，這裡不能轉畫布。 */
    void setRotation(uint_fast8_t) {}
    template <typename T> bool getTouch(T *x, T *y)
    {
        int tx, ty;
        if (!racedash_touch(&tx, &ty)) { return false; }
        if (x) { *x = (T)tx; }
        if (y) { *y = (T)ty; }
        return true;
    }
};
