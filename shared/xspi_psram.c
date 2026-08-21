/**
 * 把 PSRAM（XSPI1）的介面時脈減半：200MHz -> 100MHz。
 *
 * BSP 把時脈設在 APS256xx 的規格上限 200MHz，而時序餘裕會隨 PCB 走線、晶片
 * 批次、溫度而有板子個體差異。這塊板子落在錯誤側，200MHz 下讀取會偶發單一
 * 位元錯誤 —— 實測整片 16MB 掃描有 0 個乾淨區塊、一百多萬個字出錯，降到
 * 100MHz 之後是 256/256 全乾淨、零錯誤。
 *
 * 對相簿的殺傷力特別大，因為 PSRAM 裡放的東西全都怕錯一個位元：
 *   - 播放清單的路徑字串 -> 檔名錯一個位元組就 FR_NO_PATH，照片開不起來
 *   - 解碼後的影像       -> 畫面出現雜點
 *   - framebuffer        -> LTDC 每秒讀 60 次，錯誤位元變成閃爍的像素
 *
 * MX_XSPI_RAM_Init 在 BSP 裡是 __weak，這裡定義同名函式就會取代它，
 * 不必改到韌體包內的任何檔案（重跑 setup.sh 也不會被蓋掉）。
 *
 * 內容與 BSP 版相同，只有兩處不同：prescaler 加倍，以及 Refresh 依新的分頻
 * 重算 —— Refresh 是「2us 換算成幾個時脈」，時脈改了不跟著改就會違反 PSRAM
 * 的 tCEM 上限，反而更糟。
 *
 * 只影響 PSRAM。程式碼在外部 NOR Flash 上（XSPI2），不受影響。
 */
#include "main.h"
#include "stm32h7s78_discovery_xspi.h"

HAL_StatusTypeDef MX_XSPI_RAM_Init(XSPI_HandleTypeDef *hxspi,
                                   MX_XSPI_InitTypeDef *Init)
{
    uint32_t hspi_clk = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_XSPI1);

    hxspi->Instance = XSPI1;

    hxspi->Init.FifoThresholdByte       = 4U;
    hxspi->Init.MemoryType              = HAL_XSPI_MEMTYPE_APMEM_16BITS;
    hxspi->Init.MemoryMode              = HAL_XSPI_SINGLE_MEM;
    hxspi->Init.MemorySize              = Init->MemorySize;
    hxspi->Init.MemorySelect            = HAL_XSPI_CSSEL_NCS1;
    hxspi->Init.ChipSelectHighTimeCycle = 5U;      /* tCPH = 24 ns min */
    hxspi->Init.ClockMode               = HAL_XSPI_CLOCK_MODE_0;

    /* 分頻 = prescaler + 1，所以這樣寫等於時脈減半。 */
    hxspi->Init.ClockPrescaler          = (Init->ClockPrescaler + 1U) * 2U - 1U;

    hxspi->Init.SampleShifting          = Init->SampleShifting;
    hxspi->Init.DelayHoldQuarterCycle   = HAL_XSPI_DHQC_DISABLE;
    hxspi->Init.ChipSelectBoundary      = HAL_XSPI_BONDARYOF_16KB;
    hxspi->Init.FreeRunningClock        = HAL_XSPI_FREERUNCLK_DISABLE;
    hxspi->Init.Refresh                 =
        ((2U * (hspi_clk / (hxspi->Init.ClockPrescaler + 1U)) / 1000000U) - 4U);
    hxspi->Init.WrapSize                = HAL_XSPI_WRAP_NOT_SUPPORTED;
    hxspi->Init.MaxTran                 = 0U;

    return HAL_XSPI_Init(hxspi);
}
