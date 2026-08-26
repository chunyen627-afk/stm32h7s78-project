/* 這一段插在 usbh_audio.c 的 USBH_AUDIO_InterfaceInit 之前，
 * 並在該函式裡（USBH_AUDIO_FindHIDControl 呼叫之前）加一行：
 *     USBH_AUDIO_PickAltByFormat(phost, 2U, 16U, 48000U);
 * 由 tools/patch_usbaudio.py 自動套用。cube/ 是 gitignore 的，正本在這裡。
 */

/* --- 本專案的修改：按格式挑 alt setting，而不是挑端點最大的 -------------
 * 上面那個「largest endpoint size : default behavior」對只有一個 alt 的
 * 裝置沒差，但 ROG Delta II (2.4GHz) 有兩個：
 *     if=1 alt=1  16-bit  mps 384
 *     if=1 alt=2  24-bit  mps 576   <- 原本會挑這個
 * 而應用層送的是 16-bit。**必須在這裡挑**，不能等 InterfaceInit 跑完
 * 再從外面改 —— 下面幾行就用 headphone.EpSize 去開管線，改晚了主機通道
 * 會照 576 排程，而裝置的端點是 384。
 *
 * 走原始設定描述元找 2 聲道 / 16 bit / 支援 48000 的 OUT 串流。
 * 找不到就完全不動，維持原本的行為。
 * 這是「外面的主機」都在做的按格式協商，不是哪一支耳機的專用補丁。 */
static void USBH_AUDIO_PickAltByFormat(USBH_HandleTypeDef *phost,
                                       uint8_t ch, uint8_t bits, uint32_t rate)
{
  AUDIO_HandleTypeDef *AUDIO_Handle = (AUDIO_HandleTypeDef *)phost->pActiveClass->pData;
  const uint8_t *raw = phost->device.CfgDesc_Raw;
  uint16_t have = phost->device.CfgDesc.wTotalLength;
  uint16_t i = 0U;
  uint8_t  cur_alt = 0xFFU;
  uint8_t  fmt_ok = 0U;
  uint8_t  index;

  while ((uint16_t)(i + 2U) <= have)
  {
    uint8_t blen = raw[i];

    if ((blen < 2U) || ((uint16_t)(i + blen) > have)) { break; }

    if ((raw[i + 1U] == 0x04U) && (blen >= 9U))          /* INTERFACE */
    {
      cur_alt = raw[i + 3U];
      fmt_ok  = 0U;
    }
    else if ((raw[i + 1U] == 0x24U) && (blen >= 8U) && (raw[i + 2U] == 0x02U))
    {
      if ((raw[i + 3U] == 0x01U) && (raw[i + 4U] == ch) && (raw[i + 6U] == bits))
      {
        uint8_t nfreq = raw[i + 7U];
        uint8_t k;

        for (k = 0U; k < nfreq; k++)
        {
          uint16_t o = (uint16_t)(i + 8U + ((uint16_t)k * 3U));

          if ((uint16_t)(o + 3U) > (uint16_t)(i + blen)) { break; }
          if ((((uint32_t)raw[o]) | ((uint32_t)raw[o + 1U] << 8) |
               ((uint32_t)raw[o + 2U] << 16)) == rate)
          {
            fmt_ok = 1U;
            break;
          }
        }
      }
    }
    else if ((raw[i + 1U] == 0x05U) && (blen >= 7U) && (fmt_ok != 0U) &&
             ((raw[i + 2U] & 0x80U) == 0U) && ((raw[i + 3U] & 0x03U) == 0x01U))
    {
      /* 找到了。從 stream_out[] 取同一個 alt 的那一筆（Poll 等欄位在那裡）*/
      for (index = 0U; index < AUDIO_MAX_AUDIO_STD_INTERFACE; index++)
      {
        if ((AUDIO_Handle->stream_out[index].valid == 1U) &&
            (AUDIO_Handle->stream_out[index].AltSettings == cur_alt))
        {
          AUDIO_Handle->headphone.interface   = AUDIO_Handle->stream_out[index].interface;
          AUDIO_Handle->headphone.AltSettings = AUDIO_Handle->stream_out[index].AltSettings;
          AUDIO_Handle->headphone.Ep          = AUDIO_Handle->stream_out[index].Ep;
          AUDIO_Handle->headphone.EpSize      = AUDIO_Handle->stream_out[index].EpSize;
          AUDIO_Handle->headphone.Poll        = (uint8_t)AUDIO_Handle->stream_out[index].Poll;
          AUDIO_Handle->headphone.supported   = 1U;
          USBH_UsrLog("AUDIO: 按格式挑 alt=%u（%uch/%ubit/%luHz）EpSize=%u",
                      (unsigned)cur_alt, (unsigned)ch, (unsigned)bits,
                      (unsigned long)rate,
                      (unsigned)AUDIO_Handle->headphone.EpSize);
          break;
        }
      }
      break;
    }
    else
    {
      /* 其他描述元。 */
    }

    i = (uint16_t)(i + blen);
  }
}

