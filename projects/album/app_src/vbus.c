/**
 * 偵測 CN18（USB1）上有沒有 VBUS —— 用來判斷「USB 線插到電腦了沒」。
 *
 * 板子是用 ADC 量 VBUS 的，不是某支數位腳位：
 *   PF14 -> ADC2 通道 6，中間有 3300/499 的分壓
 *   （BSP 的 stm32h7s78_discovery_usbpd_pwr.h，TCPP0203_PORT0_VBUSC_*）
 * 5V 進來換算後 ADC 大約落在 815（滿刻度 4095），餘裕很夠。
 *
 * **為什麼不直接呼叫 BSP_USBPD_PWR_VBUSGetVoltage()**：那個要先跑
 * BSP_USBPD_PWR_Init()，會一併設定 TCPP03 的角色與電源路徑、掛上 EXTI 中斷，
 * 等於把整套 USB-PD 拉進相簿。這裡只需要回答「有沒有電壓」，自己讀一個 ADC
 * 通道就夠，也不會動到相簿以外的任何硬體。
 *
 * ADC 的設定是照抄 BSP 那份能動的（連續轉換模式），沒有自己發明 —— 唯一的
 * 差別是這裡不啟用中斷，需要的時候直接讀資料暫存器。
 */
#include "vbus.h"
#include "main.h"

/* 分壓與換算，數值取自 BSP 的 TCPP0203_PORT0_VBUSC_RA / RB。 */
#define VBUS_RA          3300u
#define VBUS_RB          499u
#define VBUS_ADC_FULL    4095u
#define VBUS_VDD_MV      3300u

/* 判定「插上主機」的門檻。BSP 對 5V 的判定門檻是 3900mV，沿用同一個數字。
 * 沒插線時分壓點被拉到接近 0，中間沒有模稜兩可的區域。 */
#define VBUS_ON_MV       3900u

static ADC_HandleTypeDef g_adc;
static bool              g_ready;

/* 除錯用：SWD 直接讀得到，不必猜。 */
volatile uint32_t g_vbus_raw;
volatile uint32_t g_vbus_last_mv;
volatile uint32_t g_vbus_initfail;

void vbus_init(void)
{
    ADC_ChannelConfTypeDef ch = {0};
    GPIO_InitTypeDef       io = {0};

    if (g_ready) { return; }

    __HAL_RCC_GPIOF_CLK_ENABLE();
    io.Pin  = GPIO_PIN_14;
    io.Mode = GPIO_MODE_ANALOG;
    io.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOF, &io);

    /* ADC 除了匯流排時脈，還需要**核心時脈**。少了它 HAL_ADC_Init 會卡在
     * 等待 ADC 就緒的迴圈裡不出來 —— 而且不回傳錯誤，症狀是整個相簿停在
     * 啟動階段（g_stage 停在 1、g_vbus_initfail 卻是 0）。實際踩過。
     * 來源沿用 BSP 的選擇（CLKP）。 */
    {
        RCC_PeriphCLKInitTypeDef pc = {0};
        pc.PeriphClockSelection = RCC_PERIPHCLK_ADC;
        pc.AdcClockSelection    = RCC_ADCCLKSOURCE_CLKP;
        if (HAL_RCCEx_PeriphCLKConfig(&pc) != HAL_OK) { g_vbus_initfail = 5u; return; }
    }

    __HAL_RCC_ADC12_CLK_ENABLE();

    g_adc.Instance                      = ADC2;
    g_adc.Init.ClockPrescaler           = ADC_CLOCK_ASYNC_DIV4;
    g_adc.Init.Resolution               = ADC_RESOLUTION_12B;
    g_adc.Init.ScanConvMode             = ADC_SCAN_ENABLE;
    g_adc.Init.EOCSelection             = ADC_EOC_SINGLE_CONV;
    g_adc.Init.LowPowerAutoWait         = DISABLE;
    g_adc.Init.ContinuousConvMode       = ENABLE;
    g_adc.Init.NbrOfConversion          = 1;
    g_adc.Init.DiscontinuousConvMode    = DISABLE;
    g_adc.Init.ExternalTrigConv         = ADC_SOFTWARE_START;
    g_adc.Init.ExternalTrigConvEdge     = ADC_EXTERNALTRIGCONVEDGE_NONE;
    g_adc.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
    g_adc.Init.Overrun                  = ADC_OVR_DATA_OVERWRITTEN;
    g_adc.Init.OversamplingMode         = DISABLE;

    if (HAL_ADC_Init(&g_adc) != HAL_OK) { g_vbus_initfail = 1u; return; }

    ch.Channel          = ADC_CHANNEL_6;
    ch.Rank             = ADC_REGULAR_RANK_1;
    /* 分壓的源阻抗有 3.3k，取樣時間要夠長，否則讀到的值會偏低。 */
    ch.SamplingTime     = ADC_SAMPLETIME_247CYCLES_5;
    ch.SingleDiff       = ADC_SINGLE_ENDED;
    ch.OffsetNumber     = ADC_OFFSET_NONE;
    ch.Offset           = 0u;
    ch.OffsetSaturation = DISABLE;
    ch.OffsetSign       = ADC_OFFSET_SIGN_POSITIVE;

    if (HAL_ADC_ConfigChannel(&g_adc, &ch) != HAL_OK) { g_vbus_initfail = 2u; return; }
    if (HAL_ADCEx_Calibration_Start(&g_adc, ADC_SINGLE_ENDED) != HAL_OK) { g_vbus_initfail = 3u; return; }
    if (HAL_ADC_Start(&g_adc) != HAL_OK) { g_vbus_initfail = 4u; return; }

    g_ready = true;
}

uint32_t vbus_mv(void)
{
    uint32_t raw, vadc;

    if (!g_ready) { return 0u; }

    /* 連續轉換模式下資料暫存器一直是新的，直接讀。 */
    raw  = HAL_ADC_GetValue(&g_adc);
    vadc = (raw * VBUS_VDD_MV) / VBUS_ADC_FULL;

    g_vbus_raw     = raw;
    g_vbus_last_mv = vadc * (VBUS_RA + VBUS_RB) / VBUS_RB;
    return g_vbus_last_mv;
}

bool vbus_present(void)
{
    return vbus_mv() >= VBUS_ON_MV;
}
