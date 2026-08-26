/* 這一段取代 cube 的 AUDIO_Standalone/Appli/Src/usbh_conf.c 裡
 * HAL_HCD_MspInit() 原本的兩行：
 *
 *     PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USBOTGFS;
 *     PeriphClkInit.UsbOtgFsClockSelection = RCC_USBOTGFSCLKSOURCE_PLL3Q;
 *
 * 由 tools/patch_usbaudio.py 自動套用。cube/ 是 gitignore 的，所以正本在這裡。
 */

    /* --- USB 全速的 48MHz 從哪裡來 --------------------------------------
     * 相簿的 PLL3 三個輸出都名花有主（board-notes 23.1：PLL3R 給 LTDC、
     * PLL3Q 給 I2S 的 49.1521MHz），而 **LTDC 只有 PLL3R 一個來源**，
     * 所以 PLL3 放不掉。相簿的 VCO 是 393.2168MHz，393.2168/48 = 8.192，
     * 沒有任何整數分頻能生出 48MHz —— 改分頻救不了。
     *
     * 試過的路：
     *   CLK48（USBPHYC）  -> **沒有時脈**，韌體卡在 HAL_HCD_Init 之前。
     *                        USBPHYC 的 PLL 不是光選多工器就會跑。
     *   HSI48 原生         -> 實測 **+3177 ppm**。USB 全速的訊框時序規格是
     *                        ±0.05%，超出六倍 —— dongle 一直斷線重連，
     *                        聲音只出來一瞬間。**不是音質差，是根本連不住。**
     *   HSI48 + CRS(LSE)   -> 這一版要驗的。CRS 用 LSE 當基準持續校準 HSI48。
     *                        SYNCDIV=32 時 48e6 x 32 / 32768 = 46875 **剛好
     *                        整除**，所以量化誤差是 0，剩下的就是晶振本身的
     *                        精度（手錶晶振通常 ±20ppm 以內）。
     *
     * 前提是這塊板子真的有 32.768kHz 晶振 —— 這個 repo 從來沒碰過 LSE，
     * 所以先試著把它啟動起來，起不來就退回 PLL3Q。 */
    {
      RCC_OscInitTypeDef osc = {0};   /* PLLxState 為 0 = RCC_PLL_NONE，不碰 PLL */
      uint8_t lse_ok;

      osc.OscillatorType = RCC_OSCILLATORTYPE_LSE;
      osc.LSEState       = RCC_LSE_ON;
      lse_ok = (HAL_RCC_OscConfig(&osc) == HAL_OK) ? 1U : 0U;

      USBH_UsrLog("CLK: LSE %s", (lse_ok != 0U) ? "ready" :
                  "TIMEOUT（這塊板子沒有 32.768kHz 晶振）");

      if (lse_ok != 0U)
      {
        RCC_CRSInitTypeDef crs = {0};

        osc.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
        osc.LSEState       = 0U;
        osc.HSI48State     = RCC_HSI48_ON;
        if (HAL_RCC_OscConfig(&osc) != HAL_OK)
        {
          Error_Handler();
        }

        __HAL_RCC_CRS_CLK_ENABLE();

        crs.Prescaler             = RCC_CRS_SYNC_DIV32;
        crs.Source                = RCC_CRS_SYNC_SOURCE_LSE;
        crs.Polarity              = RCC_CRS_SYNC_POLARITY_RISING;
        /* 48e6 x 32 / 32768 = 46875 -> RELOAD = 46874（16 bit 放得下）*/
        crs.ReloadValue           = 46874U;
        crs.ErrorLimitValue       = RCC_CRS_ERRORLIMIT_DEFAULT;
        crs.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
        HAL_RCCEx_CRSConfig(&crs);

        PeriphClkInit.UsbOtgFsClockSelection = RCC_USBOTGFSCLKSOURCE_HSI48;
        USBH_UsrLog("CLK: USB <- HSI48 + CRS(LSE)");
      }
      else
      {
        PeriphClkInit.UsbOtgFsClockSelection = RCC_USBOTGFSCLKSOURCE_PLL3Q;
        USBH_UsrLog("CLK: USB <- PLL3Q（退路，會跟相簿的 I2S 撞車）");
      }
    }

    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USBOTGFS;
