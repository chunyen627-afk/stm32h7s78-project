/**
 * PSRAM（XSPI1）介面時脈：滿速 200MHz，或減半成 100MHz。
 *
 * BSP 把時脈設在 APS256xx 的規格上限 200MHz，而時序餘裕會隨 PCB 走線、晶片
 * 批次、溫度而有板子個體差異 —— **這是板子的個體差異，不是型號的通性**。
 *
 * 第一塊板子落在錯誤側：200MHz 下整片 16MB 掃描有 0 個乾淨區塊、一百多萬個
 * 字出錯，降到 100MHz 之後是 256/256 全乾淨、零錯誤（board-notes 10）。
 *
 * 代價是格率差一倍：100MHz 只有 30.4 fps，200MHz 是 60.8 fps —— 60 就是面板
 * 上限（board-notes 19）。瓶頸在 DMA2D 的轉色，不在 LTDC。
 *
 * 換板子時要自己確認撐不撐得住，撐不住的症狀是：
 *   - framebuffer 壞位元 -> 畫面出現閃爍的細線，**畫面靜止也照閃**
 *   - 路徑字串壞位元     -> FR_NO_PATH，照片開不起來
 *   - 解碼後的影像壞位元 -> 畫面出現雜點
 * 要確定的話照 board-notes 10.4 掃整片：測試區設 non-cacheable，
 * 而且一定要有內部 SRAM 的對照組。
 *
 * 有上述症狀就把 PSRAM_HALF_CLOCK 改成 1。
 *
 * MX_XSPI_RAM_Init 在 BSP 裡是 __weak，這裡定義同名函式就會取代它，
 * 不必改到韌體包內的任何檔案（重跑 setup.sh 也不會被蓋掉）。
 *
 * 與 BSP 版唯一的差異就是 ClockPrescaler 那一行（逐行比對確認過）。
 * Refresh 是「2us 換算成幾個時脈」，公式直接從 ClockPrescaler 推導，
 * 所以會自動跟著分頻走 —— 不要把它拆開來寫死，否則減半時會違反 tCEM 上限，
 * 反而比不改更糟。
 *
 * 只影響 PSRAM。程式碼在外部 NOR Flash 上（XSPI2），不受影響。
 */
#include "main.h"
#include "stm32h7s78_discovery_xspi.h"

/* 0 = 滿速 200MHz（與 BSP 相同），1 = 減半成 100MHz。 */
#define PSRAM_HALF_CLOCK  0

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

#if PSRAM_HALF_CLOCK
    /* 分頻 = prescaler + 1，所以這樣寫等於時脈減半。 */
    hxspi->Init.ClockPrescaler          = (Init->ClockPrescaler + 1U) * 2U - 1U;
#else
    hxspi->Init.ClockPrescaler          = Init->ClockPrescaler;
#endif

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
