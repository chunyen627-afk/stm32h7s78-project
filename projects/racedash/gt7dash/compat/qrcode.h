/* 原作用 ricmoo/QRCode；LovyanGFX 自帶同一份（函式名加了 lgfx_ 前綴）。 */
#pragma once
extern "C" {
#include "../lgfx/utility/lgfx_qrcode.h"
}
#define qrcode_getBufferSize lgfx_qrcode_getBufferSize
#define qrcode_initText      lgfx_qrcode_initText
#define qrcode_initBytes     lgfx_qrcode_initBytes
#define qrcode_getModule     lgfx_qrcode_getModule
