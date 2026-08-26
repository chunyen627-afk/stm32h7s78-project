/**
 * 音訊輸出：WM8904，走 I2S6（SPI6 的 I2S 模式）+ GPDMA1 Channel 2。
 * 實體輸出只有 3.5mm 耳機孔 CN16，板子上沒有喇叭功放。
 *
 * **這是第一步：只證明「能不能發出聲音」。**
 * 從 RAM 播一段自己產生的正弦波，不碰 SD、不解析 WAV、不做同步。
 * 有聲音就代表 BSP + I2S6 + WM8904 這條鏈路是通的；沒有的話也知道問題
 * 一定在這一層，不必去懷疑檔案或時序。
 *
 * 這個順序是有代價換來的：隨身碟那輪把三個沒單獨驗證過的東西疊在一起才開始
 * 測，每次失敗都分不清是哪一個造成的（board-notes 22 與第八章第 6 條）。
 *
 * 已知的風險，之後要量：JPEG 解碼目前是**輪詢式**的，而音訊 DMA 會持續產生
 * 中斷去打斷它。這不是理論顧慮 —— repo 裡有一個 2026-08-22 的 stash，
 * 訊息就寫著「中斷干擾輪詢解碼」。所以第二步一定要量格率，不能只聽聲音。
 */
#include "audio_out.h"
#include "wav_hdr.h"
#include "usbaudio.h"
#include "main.h"
#include "stm32h7s78_discovery_audio.h"
#include "stm32h7s78_discovery_lcd.h"   /* MX_LTDC_ClockConfig 的原型 */
#include "stm32h7s78_discovery_bus.h"   /* WM8904 掛在 I2C1，直接讀它的暫存器 */
#include "wm8904_reg.h"

#include "ff.h"

#include <math.h>
#include <string.h>

/* BSP_AUDIO_OUT_Play 的 NbrOfBytes 是 uint16 換算來的，**上限 65535**。
 * 取 32KB：48kHz 立體聲 16-bit 下等於 170ms，夠長到補資料來得及，
 * 又不佔太多 AXI SRAM（相簿已經用掉 264KB / 465KB）。 */
#define AUDIO_BUF_BYTES   (32u * 1024u)

/* DMA 直接讀這塊，而且要對它做快取維護 —— 對齊到快取列（32 bytes），
 * 免得 clean 的範圍連帶動到相鄰資料（board-notes 22.3 踩過）。 */
static int16_t  g_buf[AUDIO_BUF_BYTES / 2u] __attribute__((aligned(32)));
static bool     g_ready;
static uint32_t g_rate;

/* 診斷用，SWD 讀得到。板子沒有 UART，這是唯一的觀察方式。
 * 意義與哨兵值見 audio_out.h。 */
volatile uint32_t g_dbg_aud_init = AUDIO_UNSET;
volatile uint32_t g_dbg_aud_play = AUDIO_UNSET;
volatile uint32_t g_dbg_aud_step;     /* 位元圖，一路 OR 上去 */
/* 時脈設定的兩個步驟各留一個回傳值 —— 分得出「PLL 開不起來」與
 * 「PLL 好了但 SPI6 選不到它」。合成一個就又回到「最後一次的值」那種
 * 分不清病因與症狀的診斷資料（board-notes 18.2）。 */
volatile uint32_t g_dbg_aud_pll = AUDIO_UNSET;   /* HAL_RCC_OscConfig */
volatile uint32_t g_dbg_aud_sel = AUDIO_UNSET;   /* HAL_RCCEx_PeriphCLKConfig */
volatile uint32_t g_dbg_aud_half;     /* 半滿回呼進來幾次 */
volatile uint32_t g_dbg_aud_full;     /* 全滿回呼進來幾次 */
volatile uint32_t g_dbg_aud_err;      /* 錯誤回呼進來幾次 */
volatile uint32_t g_dbg_aud_vol = AUDIO_UNSET;   /* SetVolume 的回傳值 */

/* **腳印要即時寫進 DTCM，不能只放 .bss。**
 * 現在唯一連得上的 mode=UR 一定會重置，而 .bss 一重置就被啟動碼清光 ——
 * 也就是說「掛在哪裡」這個最重要的資訊，正好是重置後讀不到的那種。
 * DTCM 不被啟動碼清（board-notes 22.5），所以每設一個位元就同步抄過去。
 * 位置與 album_main.c 的黑盒子同一塊（第 17 格）。 */
#define AUD_BBOX  ((volatile uint32_t *)0x20004020u)

static void aud_step(uint32_t bit)
{
    g_dbg_aud_step |= bit;
    AUD_BBOX[17] = g_dbg_aud_step;
}

/**
 * 覆寫 BSP 的 LTDC 時脈設定，**讓 PLL3 同時餵 LCD 與音訊**。
 *
 * 這塊板子上兩顆可用的 PLL 都名花有主：
 *   PLL2 -> XSPI1（PSRAM）與 XSPI2（外部 NOR，程式就在上面執行）
 *   PLL3 -> LTDC 的像素時脈（LCD BSP 設 PLL3R=16 得到 25MHz）
 *
 * 音訊需要第三個來源，但沒有第三顆 PLL。踩過的兩條路：
 *
 * 1. BSP 原本的 MX_I2S6_ClockConfig 去重設 **PLL2** —— 回 -9
 *    （BSP_ERROR_CLOCK_FAILURE）。XSPI 的時脈保護不讓它停 PLL2，所以失敗。
 *    **那個失敗其實是保護**：成功的話就是抽掉自己執行中的 Flash 的時脈。
 * 2. 改成重設 **PLL3** —— 初始化全部回報成功、測試音也啟動了，
 *    然後**整台掛死在 present() 存取 LTDC 暫存器的那一行**。因為 HAL 要
 *    重設 PLL3 就得先把它關掉，而 LTDC 正靠它掃描面板。
 *    症狀是「畫面停在最後一幀、程式看起來還活著」，完全不指向時脈。
 *
 * 正解是**不要重設任何正在被使用的 PLL**：開機時就把 PLL3 設成一個
 * 兩邊都夠用的頻率，之後誰都不再動它。
 *
 *   VCO = 16MHz x (24 + 4719/8192) = 393.2168 MHz
 *   PLL3R = 16 -> LTDC   24.5760 MHz（BSP 原本 25.000，差 1.7%，
 *                        面板更新率跟著差 1.7%，看不出來）
 *   PLL3Q = 8  -> 音訊   49.1521 MHz（目標 49.152，差 0.0002%）
 *                        I2S 再除 4 就是 12.288MHz = 48kHz x 256
 *
 * 要用分數分頻是因為 49.152 這個數字本身就不是整數分頻分得出來的 ——
 * 而 48kHz 這一族的音訊時脈全都是它的倍數，湊不到就會音高不準、
 * 之後對影片的時候還會持續漂移。
 *
 * 這個函式在 LCD BSP 裡是 __weak，在應用層定義同名函式就會蓋過去，
 * 不必改韌體包裡的任何檔案（board-notes 10.3 的老招）。
 */
HAL_StatusTypeDef MX_LTDC_ClockConfig(LTDC_HandleTypeDef *hltdc)
{
    RCC_OscInitTypeDef       osc = {0};
    RCC_PeriphCLKInitTypeDef pc  = {0};
    HAL_StatusTypeDef        status;

    UNUSED(hltdc);

    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState            = RCC_HSI_ON;
    osc.HSIDiv              = RCC_HSI_DIV1;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL1.PLLState       = RCC_PLL_NONE;   /* 系統時脈，不准碰 */
    osc.PLL2.PLLState       = RCC_PLL_NONE;   /* XSPI（Flash/PSRAM），不准碰 */
    osc.PLL3.PLLState       = RCC_PLL_ON;
    osc.PLL3.PLLSource      = RCC_PLLSOURCE_HSI;
    osc.PLL3.PLLM           = 4;              /* 64MHz / 4 = 16MHz */
    osc.PLL3.PLLN           = 24;
    osc.PLL3.PLLFractional  = 4719;           /* 24 + 4719/8192 = 24.576 */
    osc.PLL3.PLLP           = 2;              /* 沒用到 */
    osc.PLL3.PLLQ           = 8;              /* -> 音訊 49.152 MHz */
    osc.PLL3.PLLR           = 16;             /* -> LTDC 24.576 MHz */
    osc.PLL3.PLLS           = 1;              /* 沒用到 */
    osc.PLL3.PLLT           = 1;              /* 沒用到 */

    /* 這裡只設 LTDC 的來源。音訊那邊（SPI6 <- PLL3Q）留給
     * MX_I2S6_ClockConfig，它只動選擇器、不動 PLL。 */
    pc.PeriphClockSelection = RCC_PERIPHCLK_LTDC;
    pc.LtdcClockSelection   = RCC_LTDCCLKSOURCE_PLL3R;

    status = HAL_RCC_OscConfig(&osc);
    if (status != HAL_OK) { return status; }

    return HAL_RCCEx_PeriphCLKConfig(&pc);
}

/**
 * 覆寫 BSP 的音訊時脈設定：**只切選擇器，一顆 PLL 都不重設。**
 *
 * PLL3 已經在 MX_LTDC_ClockConfig（開機時、LCD 初始化那一步）設好了，
 * PLL3Q 就是給音訊準備的。這裡要做的只剩下把 SPI6 的來源指過去 ——
 * 而 HAL_RCCEx_PeriphCLKConfig 選到 PLL3Q 時會順手把 PLL3 的 Q 輸出打開
 * （stm32h7rsxx_hal_rcc_ex.c 的 SPI6 分支），所以連輸出致能都不必自己做。
 *
 * **這個函式絕對不能去 HAL_RCC_OscConfig 任何一顆 PLL。** 上面那段註解
 * 記著兩次的代價：動 PLL2 得到 -9，動 PLL3 直接掛死在 LTDC。
 */
HAL_StatusTypeDef MX_I2S6_ClockConfig(I2S_HandleTypeDef *hi2s, uint32_t SampleRate)
{
    RCC_PeriphCLKInitTypeDef pc = {0};
    HAL_StatusTypeDef        status;

    UNUSED(hi2s);
    aud_step(AUD_STEP_CLK_CALL);

    /* PLL3 的 Q 輸出固定是 49.152 MHz（見上面的頻率計畫），48kHz 這一族
     * 都對得上。其他取樣率這一版不支援 —— 與其偷偷用錯的時脈播出音高不對
     * 的聲音，不如當場回報失敗。 */
    if (SampleRate != AUDIO_FREQUENCY_48K && SampleRate != AUDIO_FREQUENCY_96K &&
        SampleRate != AUDIO_FREQUENCY_16K && SampleRate != AUDIO_FREQUENCY_8K &&
        SampleRate != AUDIO_FREQUENCY_32K && SampleRate != AUDIO_FREQUENCY_192K) {
        g_dbg_aud_pll = 0xBADFu;
        AUD_BBOX[19]  = g_dbg_aud_pll;
        return HAL_ERROR;
    }

    /* PLL3 必須已經在跑（LCD 初始化時設好的）。沒跑代表開機順序被改動過，
     * 這裡直接失敗比默默用一個沒鎖定的時脈好。 */
    if ((RCC->CR & RCC_CR_PLL3RDY) == 0u) {
        g_dbg_aud_pll = 0xBAD3u;
        AUD_BBOX[19]  = g_dbg_aud_pll;
        return HAL_ERROR;
    }
    g_dbg_aud_pll = (uint32_t)HAL_OK;
    AUD_BBOX[19]  = g_dbg_aud_pll;
    aud_step(AUD_STEP_CLK_OSC);

    pc.PeriphClockSelection = RCC_PERIPHCLK_SPI6;
    pc.Spi6ClockSelection   = RCC_SPI6CLKSOURCE_PLL3Q;

    status        = HAL_RCCEx_PeriphCLKConfig(&pc);
    g_dbg_aud_sel = (uint32_t)status;
    AUD_BBOX[20]  = g_dbg_aud_sel;
    aud_step(AUD_STEP_CLK_SEL);
    return status;
}

bool audio_init(uint32_t rate, uint32_t volume)
{
    BSP_AUDIO_Init_t cfg;
    int32_t          r;

    aud_step(AUD_STEP_INIT_CALL);

    for (uint32_t i = 52u; i <= 59u; i++) { AUD_BBOX[i] = 0u; }

    if (g_ready) { return true; }

    cfg.Device        = AUDIO_OUT_HEADPHONE;
    cfg.SampleRate    = rate;
    cfg.BitsPerSample = AUDIO_RESOLUTION_16B;
    cfg.ChannelsNbr   = 2;
    cfg.Volume        = volume;

    r = BSP_AUDIO_OUT_Init(0, &cfg);
    g_dbg_aud_init = (uint32_t)r;
    AUD_BBOX[18] = g_dbg_aud_init;
    /* **Init 自己就會呼叫錯誤回呼，卻照樣回傳成功。** BSP 的 I2S_MspInit
     * 在建 DMA 連結串列失敗時是呼叫 BSP_AUDIO_OUT_Error_CallBack(0) 然後
     * 繼續往下走 —— Init 的回傳值完全看不出來。所以要在這裡先記一次，
     * 才分得出「錯誤是初始化時就發生的」還是「播放之後 DMA 才出錯」。 */
    AUD_BBOX[46] = g_dbg_aud_err;
    aud_step(AUD_STEP_BSP_RET);   /* 它「有回來」本身就是一項資訊 */
    if (r != BSP_ERROR_NONE) { return false; }

    /* **音量要自己再設一次。** BSP_AUDIO_OUT_Init 的 cfg.Volume 有沒有真的
     * 傳到耳機放大器那一級，我沒有單獨驗過 —— 而第一次實測就是「DMA 在跑、
     * 編解碼器也沒靜音，但聽不到」，把音量明確設上去才聽得到。
     * 與其相信一個沒驗過的路徑，不如多打一次 I2C。 */
    g_dbg_aud_vol = (uint32_t)BSP_AUDIO_OUT_SetVolume(0, (uint8_t)volume);
    AUD_BBOX[104] = g_dbg_aud_vol;

    g_rate  = rate;
    g_ready = true;
    aud_step(AUD_STEP_INIT_OK);
    return true;
}

bool audio_tone(uint32_t hz)
{
    uint32_t frames = AUDIO_BUF_BYTES / 4u;   /* 一個 frame = L+R = 4 bytes */
    int32_t  r;
    uint32_t g_st = 0u;

    aud_step(AUD_STEP_TONE_CALL);

    if (!g_ready) { return false; }

    /* **讓緩衝首尾相接**：取整數個週期，否則循環播放時每一圈的接縫都會
     * 「啪」一聲。frames 取到最接近的整數週期倍數。 */
    {
        uint32_t period = (hz > 0u) ? (g_rate / hz) : frames;
        if (period > 0u && period < frames) {
            frames = (frames / period) * period;
        }
    }

    for (uint32_t i = 0; i < frames; i++) {
        /* 振幅只給三分之一。第一次出聲最怕的是音量開太大 —— 耳機貼著耳朵
         * 的話會很傷，而這階段還不確定音量設定有沒有生效。 */
        double   t = (2.0 * 3.14159265358979 * (double)hz * (double)i) / (double)g_rate;
        int16_t  s = (int16_t)(10000.0 * sin(t));

        g_buf[i * 2u]      = s;   /* L */
        g_buf[i * 2u + 1u] = s;   /* R */
    }

    /* **DMA 從記憶體讀，資料還在 D-Cache 裡的話它讀到的是舊內容。**
     * 這一輪已經在 USB 那邊踩過同一類問題兩次（board-notes 22.3 / 22.5）。 */
    SCB_CleanDCache_by_Addr((uint32_t *)g_buf, (int32_t)(frames * 4u));
    aud_step(AUD_STEP_TONE_FILL);

    r = BSP_AUDIO_OUT_Play(0, (uint8_t *)g_buf, frames * 4u);
    g_dbg_aud_play = (uint32_t)r;
    AUD_BBOX[21] = g_dbg_aud_play;
    aud_step(AUD_STEP_PLAY_RET);

    /* 硬體自己的狀態，比任何推測都準（board-notes 16.4）。
     * DMA 的 CSR 會直接寫著是哪一種錯：USEF=設定錯、DTEF=傳輸錯、
     * ULEF=連結清單錯；SPI6 的 SR 則說 I2S 這邊有沒有 underrun。 */
    AUD_BBOX[27] = GPDMA1_Channel2->CSR;
    AUD_BBOX[28] = GPDMA1_Channel2->CCR;
    AUD_BBOX[29] = SPI6->SR;
    AUD_BBOX[45] = (uint32_t)BSP_AUDIO_OUT_GetState(0, &g_st) == 0u ? g_st : 0xFFFFFFFFu;

    /* 再等一下下重看一次：Play 回來的那一瞬間 DMA 可能還沒被啟動，
     * 只看那一刻會把「還沒開始」誤判成「起不來」。 */
    HAL_Delay(50);
    AUD_BBOX[43] = GPDMA1_Channel2->CSR;
    AUD_BBOX[44] = GPDMA1_Channel2->CCR;
    AUD_BBOX[47] = SPI6->CR1;
    AUD_BBOX[48] = SPI6->CFG1;
    /* **把編解碼器自己的狀態讀出來。**
     * 到這裡為止證明的是「DMA 有在把資料餵給 I2S」，那是 I2S 之前的事。
     * 沒有聲音的話問題在 I2S 之後 —— 而 WM8904 掛在 I2C1（跟觸控同一條），
     * BSP 有現成的讀取函式，可以直接問它「你到底有沒有被設好」，
     * 不必用推的（board-notes 16.4：靠硬體自己的狀態，不要猜）。 */
    {
        static const uint16_t regs[] = {
            WM8904_SW_RESET,             /* 讀回來應該是 0x8904，證明 I2C 通 */
            WM8904_BIAS_CONTROL0,
            WM8904_PWR_MANAGEMENT0, WM8904_PWR_MANAGEMENT2,
            WM8904_PWR_MANAGEMENT3, WM8904_PWR_MANAGEMENT6,
            WM8904_CLOCK_RATES0, WM8904_CLOCK_RATES1, WM8904_CLOCK_RATES2,
            WM8904_AUDIO_INTERFACE1, WM8904_AUDIO_INTERFACE2,
            WM8904_AUDIO_INTERFACE3,
            WM8904_DAC_DIGITAL_VOL_LEFT, WM8904_DAC_DIGITAL1,
            WM8904_ANALOG_HP0, WM8904_CHARGE_PUMP0, WM8904_CLASS_W0,
            /* 類比輸出那一級：DAC 沒靜音不代表耳機放大器沒靜音。
             * HPOUT?_MUTE 是另一個位元，音量也是另一組暫存器。 */
            WM8904_ANALOG_OUTPUT1_LEFT, WM8904_ANALOG_OUTPUT1_RIGHT,
            WM8904_DC_SERVO0, WM8904_DC_SERVO_READBACK0,
        };
        AUD_BBOX[80] = SPI6->I2SCFGR;
        AUD_BBOX[81] = SPI6->CR1;
        AUD_BBOX[82] = SPI6->SR;
        for (uint32_t i = 0; i < (sizeof(regs) / sizeof(regs[0])); i++) {
            uint8_t  b[2] = {0, 0};
            int32_t  rr   = BSP_I2C1_ReadReg(AUDIO_I2C_ADDRESS, regs[i], b, 2);
            AUD_BBOX[83u + i] = (rr != 0) ? 0xE2C0000u
                                          : (((uint32_t)b[0] << 8) | b[1]);
        }
    }

    AUD_BBOX[49] = g_dbg_aud_half;
    AUD_BBOX[50] = g_dbg_aud_full;
    AUD_BBOX[51] = g_dbg_aud_err;
    return r == BSP_ERROR_NONE;
}

/* ------------------------------------------------------------------ */
/* 從卡上串流 WAV                                                      */
/* ------------------------------------------------------------------ */
/**
 * 做法：把 g_buf 當成一個循環播放的環形緩衝，DMA 一直繞著它跑，
 * 我們在它「剛播完的那一半」補上新資料。
 *
 *   半滿回呼 -> DMA 已經播完**前半** -> 前半可以安全覆寫
 *   全滿回呼 -> DMA 已經播完**後半** -> 後半可以安全覆寫
 *
 * 每半 16KB，48kHz 立體聲 16-bit 下是 85ms，而讀 16KB 只要幾毫秒 ——
 * 餘裕很大，但還是要量（見 g_dbg_wav_under）。
 *
 * **補資料不在回呼裡做。** 讀卡片可能要幾毫秒到幾十毫秒，在中斷裡忙等會把
 * SysTick 押後、HAL_GetTick() 漏拍，對時與逾時判斷全部失準
 * （board-notes 16.13 記過這條）。回呼只計次，實際的讀取交給主迴圈的
 * audio_wav_pump()。
 *
 * 計數用「ISR 只加、主迴圈只加」兩個各自單一寫入者的變數，不用旗標 ——
 * 旗標的 `|=` 與 `&=~` 會互相踩掉（丟更新），而那種 bug 是間歇性的。
 */
#define WAV_HALF   (AUDIO_BUF_BYTES / 2u)

static FIL      g_wav;
static bool     g_wav_open;
static uint32_t g_wav_left;        /* data 區塊還剩幾 bytes 沒讀 */
static uint32_t g_wav_data0;       /* data 區塊在檔案裡的起始位移 */
static uint32_t g_wav_total;       /* data 區塊總長，換算時間用 */
static bool     g_wav_paused;

/* 48kHz 立體聲 16-bit = 每秒 192000 bytes。所有時間換算都走這個常數，
 * 不要在各處各寫一次 —— 這種常數散開之後改取樣率就會漏掉一處。 */
#define WAV_BYTES_PER_SEC   192000u

static volatile uint32_t g_ht_cnt; /* 回呼加的 */
static volatile uint32_t g_tc_cnt;
static uint32_t g_ht_done;         /* 主迴圈加的 */
static uint32_t g_tc_done;

/* 播放位置的原點與暫停補償，見 audio_wav_pos_ms()。 */
static uint32_t g_pos_org;         /* 檔案位移的原點 */
static uint32_t g_pos_org_play;    /* 對應那一刻已送出的位元組數 */
static uint32_t g_pause_play;      /* 進入暫停時已送出的位元組數 */
static uint32_t g_pause_total;     /* 暫停期間送出去的靜音總量 */
static bool     g_playing;         /* DMA 真的在跑（CBR1 才有意義）*/

volatile uint32_t g_dbg_wav_rate;
volatile uint32_t g_dbg_wav_fmt;   /* (聲道 << 16) | 位元數 */
volatile uint32_t g_dbg_wav_bytes; /* data 區塊總長 */
volatile uint32_t g_dbg_wav_fed;   /* 已經餵給 DMA 幾 bytes */
volatile uint32_t g_dbg_wav_under; /* 補不上的次數（欠載）*/
volatile uint32_t g_dbg_wav_rderr; /* f_read 失敗次數 */
volatile uint32_t g_dbg_wav_step;  /* 開檔失敗時停在哪一步 */
/* 補資料花掉多少時間。**這段不在影片任何一個計時區間裡** —— 影片的分項
 * 加起來只有 20ms 卻跑不到 24fps，差的就是這裡，所以一定要單獨量。 */
volatile uint32_t g_dbg_wav_us;    /* 累計的 f_read 耗時（微秒）*/
volatile uint32_t g_dbg_wav_reads; /* 讀了幾次 */

/* 把某一半補滿。不夠就補靜音（結尾那一段），這樣不會播到上一圈的殘響。 */
static void wav_fill(uint32_t half)
{
    uint8_t *dst  = (uint8_t *)g_buf + half * WAV_HALF;
    uint32_t want = (g_wav_left < WAV_HALF) ? g_wav_left : WAV_HALF;
    UINT     got  = 0;

    /* 暫停 = 餵靜音，**檔案的位置不動**。
     *
     * 這樣恢復時聲音正好接在中斷的地方，暫停多久都不會跟畫面差開；
     * 而且完全不碰 I2S 與 DMA 的狀態機 —— 那條路踩過一次很貴的坑，見
     * audio_wav_pause() 的說明。 */
    if (g_wav_paused) {
        memset(dst, 0, WAV_HALF);
        SCB_CleanDCache_by_Addr((uint32_t *)dst, (int32_t)WAV_HALF);
        return;
    }

    if (want != 0u) {
        uint32_t c0 = DWT->CYCCNT;

        if (f_read(&g_wav, dst, want, &got) != FR_OK) {
            got = 0;
            g_dbg_wav_rderr++;
        }
        g_wav_left      -= got;
        g_dbg_wav_fed   += got;
        g_dbg_wav_us    += (DWT->CYCCNT - c0) / (SystemCoreClock / 1000000u);
        g_dbg_wav_reads++;
    }
    if (got < WAV_HALF) {
        memset(dst + got, 0, WAV_HALF - got);
    }
    /* **DMA 讀的是記憶體，不是快取。** 每補一次都要寫回，不能只在開始時做一次
     * （board-notes 3.1 / 23.3 同一家族）。 */
    SCB_CleanDCache_by_Addr((uint32_t *)dst, (int32_t)WAV_HALF);
}

/* WAV 檔頭：不要假設 data 一定在第 44 個位元組。
 * 很多轉檔工具會塞 LIST/INFO 之類的區塊進去，寫死 44 會直接把雜訊當成音訊播。 */
/* 檔頭解析搬到 wav_hdr.c 跟 USB 那條路共用（行為刻意一模一樣）。
 * 這裡只負責把結果填進本檔既有的全域，其餘完全沒動。 */
static bool wav_parse(void)
{
    wav_hdr_t h;

    if (!wav_hdr_parse(&g_wav, &h)) { return false; }

    g_dbg_wav_rate  = h.rate;
    g_dbg_wav_fmt   = ((uint32_t)h.channels << 16) | h.bits;
    g_wav_left      = h.data_len;
    g_wav_total     = h.data_len;
    g_wav_data0     = h.data_off;
    g_dbg_wav_bytes = h.data_len;
    return true;
}

/* --- 輸出路徑：USB 無線耳機 vs 3.5mm 耳機孔 ---------------------------
 * **兩條都保留。** 偵測到 dongle 就走 USB，沒有（或 USB 那邊開不起來）
 * 就退回 I2S —— 使用者明確要求 3.5mm 要能繼續用。
 *
 * 選擇只在 audio_wav_start() 做一次，之後整段都用同一條路：
 * 播到一半換路徑會在聲音上留下一個接縫，而且位置換算的原點也會跳掉。
 *
 * 這個旗標是**下面六個函式唯一的分歧點** —— I2S 的實作完全沒有動。 */
static bool g_use_usb;

bool audio_wav_start(const char *path, uint32_t volume)
{
    g_use_usb = false;
    if (usbaudio_ready() && usbaudio_wav_start(path, volume)) {
        g_use_usb = true;
        return true;
    }

    g_dbg_wav_step  = 1u;
    g_dbg_wav_fed   = 0u;
    g_dbg_wav_under = 0u;
    g_dbg_wav_rderr = 0u;

    audio_wav_stop();

    if (f_open(&g_wav, path, FA_READ) != FR_OK) { return false; }
    g_wav_open = true;

    g_dbg_wav_step = 2u;
    if (!wav_parse()) { audio_wav_stop(); return false; }

    /* **取樣率只支援 48k 這一族。** PLL3Q 固定在 49.152MHz（見
     * MX_LTDC_ClockConfig 的頻率計畫），44.1k 那一族湊不出來 —— 硬播會音高
     * 不準而且對影片會持續漂移。與其偷偷播錯，不如當場失敗。 */
    g_dbg_wav_step = 3u;
    if (g_dbg_wav_rate != 48000u || (g_dbg_wav_fmt & 0xFFFFu) != 16u ||
        (g_dbg_wav_fmt >> 16) != 2u) {
        audio_wav_stop();
        return false;
    }

    g_dbg_wav_step = 4u;
    if (!audio_init(48000u, volume)) { audio_wav_stop(); return false; }

    /* 兩半都先填滿再開始，否則第一圈會播到空的後半。 */
    g_ht_cnt = 0u; g_tc_cnt = 0u; g_ht_done = 0u; g_tc_done = 0u;
    wav_fill(0u);
    wav_fill(1u);

    g_dbg_wav_step = 5u;
    g_pos_org      = 0u;
    g_pos_org_play = 0u;
    g_pause_total  = 0u;
    g_pause_play   = 0u;
    g_playing      = true;
    if (BSP_AUDIO_OUT_Play(0, (uint8_t *)g_buf, AUDIO_BUF_BYTES) != BSP_ERROR_NONE) {
        g_playing = false;
        audio_wav_stop();
        return false;
    }
    g_dbg_wav_step = 6u;
    return true;
}

void audio_wav_pump(void)
{
    /* --- **USB 的狀態機一定要在這裡也被推進** --------------------------
     * 不管這一段音訊走的是 I2S 還是 USB。
     *
     * 原因是實測出來的：相簿的影片節奏是
     *     `while (next_ms > HAL_GetTick()) { audio_wav_pump(); }`
     * 而**影片迴圈完全不經過 nap()** —— USB 原本只掛在 nap() 裡，
     * 於是一進影片就沒有人服務 USB 了。後果不只是「音訊不會開播」：
     * 主機起動了、埠供電了、dongle 掛在上面而沒有人去清它的中斷旗標，
     * 那是中斷風暴，會把主迴圈餓死。
     *
     * 實測（自動滑動測試）：USB 開著滑兩次就當；把 USB 整個編掉，
     * 滑 15 次、跑滿 60 秒完全正常。
     *
     * usbaudio_process() 在抽送已經交給中斷時會自己直接回來。 */
    usbaudio_process();

    if (g_use_usb) { usbaudio_wav_pump(); return; }

    if (!g_wav_open) { return; }

    /* 差距大於 1 代表上一次還沒補完就又被播過去了 —— 那一段是舊資料，
     * 會聽到一小段重複。這個數字比「聽起來怪怪的」可靠得多。 */
    if ((uint32_t)(g_ht_cnt - g_ht_done) > 1u ||
        (uint32_t)(g_tc_cnt - g_tc_done) > 1u) {
        g_dbg_wav_under++;
    }
    while (g_ht_done != g_ht_cnt) { wav_fill(0u); g_ht_done++; }
    while (g_tc_done != g_tc_cnt) { wav_fill(1u); g_tc_done++; }
}

/* 已經送出去（＝已經播出去）的位元組數。
 *
 * 之前是用「餵進緩衝的量」估的，而那個值以 16KB 為單位跳，解析度只有
 * 85ms —— 拿來量漂移的話，尺比要量的東西還粗（實測九分鐘量到的差
 * 58ms，比解析度還小，連正負號都定不下來）。
 *
 * 改成問 DMA 自己：`CBR1` 的 BNDT 是「這一塊還剩幾 bytes 沒搬」，
 * 所以緩衝內的位置就是 `BUF - BNDT`，精度是位元組（0.005ms）。
 *
 * **要防繞圈的競爭。** 讀 BNDT 跟讀「已經跑完幾圈」不是原子的：如果剛好
 * 在兩次讀取之間繞了一圈，算出來會整整多一個緩衝（170ms），而且是偶發的
 * —— 那種偏差混在漂移數據裡會非常難查。做法是前後各讀一次圈數，
 * 不一樣就重來。
 */
static uint32_t played_bytes(void)
{
    uint32_t c1, c2, bndt;

    if (!g_playing) { return 0u; }
    do {
        c1   = g_tc_cnt;
        bndt = GPDMA1_Channel2->CBR1 & DMA_CBR1_BNDT;
        c2   = g_tc_cnt;
    } while (c1 != c2);

    return c1 * AUDIO_BUF_BYTES + (AUDIO_BUF_BYTES - bndt);
}

/* 目前播到第幾毫秒（扣掉暫停期間送出去的靜音）。 */
uint32_t audio_wav_pos_ms(void)
{
    if (g_use_usb) { return usbaudio_wav_pos_ms(); }

    uint32_t played = played_bytes();
    uint32_t paused_now = g_wav_paused ? (played - g_pause_play) : 0u;
    uint32_t off;

    if (!g_wav_open || played < g_pos_org_play) { return 0u; }

    off = g_pos_org + (played - g_pos_org_play) - g_pause_total - paused_now;
    return (uint32_t)(((uint64_t)off * 1000u) / WAV_BYTES_PER_SEC);
}

uint32_t audio_wav_len_ms(void)
{
    if (g_use_usb) { return usbaudio_wav_len_ms(); }

    return (uint32_t)(((uint64_t)g_wav_total * 1000u) / WAV_BYTES_PER_SEC);
}

/* 跳到指定的時間點。拖曳進度條之後聲音要跟著跳，否則畫面對了聲音沒對。 */
bool audio_wav_seek_ms(uint32_t ms)
{
    if (g_use_usb) { return usbaudio_wav_seek_ms(ms); }

    uint32_t off;

    if (!g_wav_open) { return false; }

    off = (uint32_t)(((uint64_t)ms * WAV_BYTES_PER_SEC) / 1000u);
    off &= ~3u;                                  /* 對齊到一個取樣框 */
    if (off > g_wav_total) { off = g_wav_total; }

    /* 麵包屑（跟 album_main.c 的 BBOX[149] 同一格）：
     * 10 進入 11 lseek 回來 12 第一半填完 13 第二半填完。 */
    ((volatile uint32_t *)0x20004020u)[149] = 10u;
    if (f_lseek(&g_wav, g_wav_data0 + off) != FR_OK) { return false; }
    ((volatile uint32_t *)0x20004020u)[149] = 11u;
    g_wav_left    = g_wav_total - off;
    g_dbg_wav_fed = off + AUDIO_BUF_BYTES;

    /* 位置的原點跟著搬。注意剛跳完的那一個緩衝（最多 170ms）裡還是舊資料，
     * 所以位置會有一段短暫的誤差 —— 這是環形緩衝本來就有的，不是 bug。 */
    g_pos_org      = off;
    g_pos_org_play = played_bytes();
    g_pause_total  = 0u;
    g_pause_play   = g_pos_org_play;

    /* 兩半都重填，否則會先播出跳之前的殘留（聽起來像「跳完先閃一段舊的」）。*/
    wav_fill(0u);
    ((volatile uint32_t *)0x20004020u)[149] = 12u;
    wav_fill(1u);
    ((volatile uint32_t *)0x20004020u)[149] = 13u;
    return true;
}

/**
 * 暫停／恢復。**不要用 BSP_AUDIO_OUT_Pause()。**
 *
 * 它會走 HAL_I2S_DMAPause -> 設 CSUSP 再等 CSTART 清掉，而那個等待的逾時是
 * `I2S_TIMEOUT = 0xFFFF` = **65.5 秒**。相簿的看門狗只有 16 秒 ——
 * 逾時還沒到，狗就先把板子打掉了。實際症狀是使用者回報的
 * 「點暫停會卡死，過一段時間 RESET 跳回選單」，而那個畫面看起來像
 * 「暫停功能寫壞了」，完全不會聯想到是 HAL 裡一個比看門狗還長的逾時。
 *
 * 改成讓 DMA 繼續跑、只是餵靜音（見 wav_fill）。好處不只是不會卡：
 * **檔案位置在暫停期間不動，所以恢復時音畫還是對齊的** ——
 * 真的去暫停 DMA 的話，恢復後聲音會比畫面少掉暫停的那一段。
 *
 * 代價是按下暫停之後，緩衝裡已經排隊的最多 170ms 還是會播出去。
 */
void audio_wav_pause(bool on)
{
    if (g_use_usb) { usbaudio_wav_pause(on); return; }

    if (!g_wav_open || on == g_wav_paused) { return; }

    /* 暫停期間 DMA 照跑（送的是靜音），所以「已送出的位元組」會繼續增加。
     * 不補償的話播放位置會在暫停時往前跑，恢復後就跟畫面差掉暫停的時間。 */
    if (on) {
        g_pause_play = played_bytes();
    } else {
        g_pause_total += played_bytes() - g_pause_play;
    }
    g_wav_paused = on;
}

bool audio_wav_active(void)
{
    if (g_use_usb) { return usbaudio_wav_active(); }

    /* 資料讀完之後還要讓 DMA 把緩衝裡剩下的播完（最多兩個半區）。 */
    return g_wav_open && (g_wav_left != 0u ||
                          (uint32_t)(g_ht_cnt + g_tc_cnt) < 2u ||
                          g_dbg_wav_fed == 0u);
}

void audio_wav_stop(void)
{
    /* **兩邊都收。** stop 可能發生在還沒選定路徑、或剛切換完的時候，
     * 只收一邊會留下一條還在跑的串流。兩個都是冪等的。 */
    if (g_use_usb) { usbaudio_wav_stop(); g_use_usb = false; return; }

    if (g_wav_open) {
        audio_stop();
        (void)f_close(&g_wav);
        g_wav_open = false;
    }
    g_wav_left   = 0u;
    g_wav_paused = false;
    g_playing    = false;
}

/* 設定音量（0~100）。**0 是真的靜音，不是最小聲。**
 *
 * BSP 的換算是 `暫存器 = 音量/2 + 13`，所以傳 0 進去得到的是 -44dB ——
 * 還聽得見。使用者把音量調到 0 卻還聽得到一點聲音是很糟的體驗，
 * 所以 0 走 BSP 的靜音（它會下 WM8904 的 DAC_MUTE），其餘才走音量。 */
void audio_set_volume(uint32_t pct)
{
    if (g_use_usb) { usbaudio_set_volume(pct); return; }

    if (pct > 100u) { pct = 100u; }

    if (pct == 0u) {
        (void)BSP_AUDIO_OUT_Mute(0);
    } else {
        (void)BSP_AUDIO_OUT_UnMute(0);
        g_dbg_aud_vol = (uint32_t)BSP_AUDIO_OUT_SetVolume(0, (uint8_t)pct);
    }
}

void audio_stop(void)
{
    if (g_ready) {
        (void)BSP_AUDIO_OUT_Stop(0);
    }
}

/**
 * 音訊輸出 DMA 的中斷處理常式（BSP 的表：I2S6 輸出 = GPDMA1 Channel 2）。
 *
 * **少了它就是無限迴圈。** 向量表裡沒有被定義的中斷會連到啟動碼的
 * `Default_Handler`，而那個函式是 `b .` —— 中斷一進來，CPU 就永遠停在
 * 那裡，不會有任何錯誤訊息。
 *
 * 實測症狀：`BSP_AUDIO_OUT_Play` 進去就沒再出來（腳印停在 0x3BE，
 * PLAY_RET 這個位元不亮），而且相簿在啟動看門狗**之前**跑音訊測試，
 * 所以沒有人來收拾，板子就一直掛著 —— 螢幕停在「音訊測試」那一頁。
 * 從外面看跟「相簿當掉」一模一樣，完全不會聯想到是少了一個中斷向量。
 *
 * 定義在應用層而不是 cube 的 `stm32h7rsxx_it.c`：中斷處理常式本來就是
 * 覆寫弱符號，放哪裡效果都一樣；而 cube/ 是 gitignore 的，寫在那裡
 * 重跑 setup.sh 就會消失。
 */
void GPDMA1_Channel2_IRQHandler(void)
{
    /* **在進 HAL 之前先凍結第一次的現場。** HAL_DMA_IRQHandler 會先把狀態
     * 旗標寫掉才呼叫錯誤回呼，所以事後再讀 CSR 永遠是乾淨的 —— 看起來
     * 像「沒有錯誤」，而錯誤回呼卻真的進來過。
     * 只留第一次：後面的會覆蓋掉，而第一次才是病因（board-notes 16.12/18.2）。 */
    if (AUD_BBOX[53] == 0u) {
        AUD_BBOX[52] = GPDMA1_Channel2->CSR;
        AUD_BBOX[54] = GPDMA1_Channel2->CBR1;
        AUD_BBOX[55] = GPDMA1_Channel2->CSAR;
        AUD_BBOX[56] = GPDMA1_Channel2->CDAR;
        AUD_BBOX[57] = GPDMA1_Channel2->CLLR;
        AUD_BBOX[58] = GPDMA1_Channel2->CTR1;
        AUD_BBOX[59] = GPDMA1_Channel2->CTR2;
    }
    AUD_BBOX[53]++;

    BSP_AUDIO_OUT_IRQHandler(0, AUDIO_OUT_HEADPHONE);
}

/* BSP 的回呼。第一步只計次 —— 有沒有進來就知道 DMA 有沒有在跑，
 * 這比「聽起來好像有聲音」可靠。 */
void BSP_AUDIO_OUT_HalfTransfer_CallBack(uint32_t Instance)
{
    (void)Instance;
    g_dbg_aud_half++;
    g_ht_cnt++;          /* DMA 播完前半 -> 前半可以覆寫了 */
}

void BSP_AUDIO_OUT_TransferComplete_CallBack(uint32_t Instance)
{
    (void)Instance;
    g_dbg_aud_full++;
    g_tc_cnt++;          /* DMA 播完後半 -> 後半可以覆寫了 */
}

void BSP_AUDIO_OUT_Error_CallBack(uint32_t Instance)
{
    (void)Instance;
    g_dbg_aud_err++;
    /* 直接寫 DTCM：要分辨「錯一次」與「錯誤中斷一直重進」（中斷風暴會讓
     * 主程式永遠跑不下去，症狀就是整台掛住）。而這個數字必須活過重置。 */
    AUD_BBOX[30] = g_dbg_aud_err;
}
