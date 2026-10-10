/*----------------------------------------------------------------------------/
  Lovyan GFX - racedash 平台（STM32H7S78-DK 賽車儀表，以及電腦端的參考渲染器）

  只用 LGFX_Sprite（記憶體畫布），不接任何實體面板或匯流排，所以平台層只要
  計時與記憶體配置。以 platforms/framebuffer/common.hpp 為範本。
  LovyanGFX: FreeBSD License (https://github.com/lovyan03/LovyanGFX)
/----------------------------------------------------------------------------*/
#pragma once

#include "../../misc/DataWrapper.hpp"
#include "../../misc/enum.hpp"
#include "../../../utility/result.hpp"

#include <stdlib.h>
#include <stdint.h>

namespace lgfx
{
 inline namespace v1
 {
  unsigned long millis(void);
  unsigned long micros(void);
  void delay(unsigned long milliseconds);
  void delayMicroseconds(unsigned int us);

  static inline void* heap_alloc(      size_t length) { return malloc(length); }
  static inline void* heap_alloc_psram(size_t length) { return malloc(length); }
  static inline void* heap_alloc_dma(  size_t length) { return malloc(length); }
  static inline void heap_free(void* buf) { free(buf); }

  static inline void gpio_hi(uint32_t) {}
  static inline void gpio_lo(uint32_t) {}
  static inline bool gpio_in(uint32_t) { return false; }

  enum pin_mode_t { output, input, input_pullup, input_pulldown };
  static inline void pinMode(int_fast16_t, pin_mode_t) {}
  static inline void lgfxPinMode(int_fast16_t, pin_mode_t) {}

  /* 不從檔案系統讀字型或圖片。 */
  struct FileWrapper : public DataWrapper
  {
    FileWrapper() : DataWrapper() { need_transaction = false; }
    bool open(const char*) override { return false; }
    int read(uint8_t*, uint32_t) override { return 0; }
    void skip(int32_t) override {}
    bool seek(uint32_t) override { return false; }
    void close() override {}
    int32_t tell(void) override { return 0; }
  };
 }
}
