/**
 * USB Host 的底層膠合（LL）—— 從 ST 的 AUDIO_Standalone 範例
 * Appli/Src/usbh_conf.c 搬過來，因為 cube/ 是 gitignore 的。
 *
 * 跟範例的差異只有兩處，其餘逐字保留（都是 HAL <-> USBH 的樣板）：
 *   1. 時脈：USB 全速的 48MHz 走 HSI48 + CRS 校準到 LSE，**完全繞開 PLL3**。
 *      相簿的 PLL3 三個輸出都名花有主（board-notes 23.1：PLL3R 給 LTDC、
 *      PLL3Q 給 I2S 的 49.1521MHz），而 LTDC 只有 PLL3R 一個來源。
 *      **這是「3.5mm 要保留」這個需求的直接後果** —— 走 PLL3Q=48MHz 的話
 *      I2S 會音高差 2.3%。實測 -44ppm、0 斷線。
 *   2. SOF 回呼不驅動 USBH_Process（1kHz 實測只送得出 500 包/秒），
 *      抽送由 TIM7 8kHz 做，見 usbaudio.c。
 *
 * OTG_FS 的中斷處理常式也在這裡（範例是放在 it.c，那個檔在 cube/ 底下）。
 */
/* Includes ------------------------------------------------------------------*/
#include "usbh_core.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* Private variables ---------------------------------------------------------*/

/* USER CODE END PV */

HCD_HandleTypeDef hhcd_USB_OTG_FS;

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* USER CODE BEGIN PFP */
/* Private function prototypes -----------------------------------------------*/
USBH_StatusTypeDef USBH_Get_USB_Status(HAL_StatusTypeDef hal_status);

/* USER CODE END PFP */

/* Private functions ---------------------------------------------------------*/

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/*******************************************************************************
                       LL Driver Callbacks (HCD -> USB Host Library)
*******************************************************************************/
/* MSP Init */

void HAL_HCD_MspInit(HCD_HandleTypeDef* hcdHandle)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};
  if(hcdHandle->Instance==USB_OTG_FS)
  {
  /* USER CODE BEGIN USB_OTG_FS_MspInit 0 */

  /* USER CODE END USB_OTG_FS_MspInit 0 */

    /** Initializes the peripherals clock
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

      /* 黑盒子 193：USB 時脈到底走了哪條路（相簿沒有 UART，上面那行
       * UsrLog 看不到）。0xC1C0000１ = HSI48+CRS、0xC1C00002 = 退回
       * PLL3Q（+2.4%，控制傳輸過得了、等時音訊全滅 —— 「冷上電沒聲音」
       * 查的就是這個）。附上 tick 佐證是哪一輪寫的。 */
      ((volatile uint32_t *)0x20004020u)[193] =
          ((lse_ok != 0U) ? 0xC1000000u : 0xC2000000u) |
          (HAL_GetTick() & 0x00FFFFFFu);

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
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
    {
      Error_Handler();
    }

    /** Enable USB Voltage detector
    */
    HAL_PWREx_EnableUSBVoltageDetector();

    /* Peripheral clock enable */
    __HAL_RCC_USB_OTG_FS_CLK_ENABLE();
    __HAL_RCC_USBPHYC_CLK_ENABLE();

    /* --- **不要開 NVIC 的 OTG 中斷：改成純輪詢。** -----------------------
     * 實測：只要開啟 OTG 全域中斷，相簿就會在影片滑動時整台停住 ——
     * 主迴圈、TIM7、看門狗全部凍結，而且**沒有 CPU 故障**。
     * 那是 HAL_HCD_IRQHandler 進去不出來（或不斷重入）的形狀。
     * 二分法確認過：USBH_Init 不開中斷 -> 自動滑動 12 次跑滿 50 秒正常；
     * 一開中斷 -> 滑一兩次就當。跟 VBUS 無關（只開中斷不供電也一樣當）。
     *
     * 改成輪詢之後這個風險從根本消失：USB 只在我們呼叫它的時候動，
     * 而抽送頻率（8kHz）遠高於 USB 訊框的 1kHz，時序完全夠。
     * 呼叫點見 usbaudio.c 的 usb_poll()。 */
    HAL_NVIC_DisableIRQ(OTG_FS_IRQn);
  /* USER CODE BEGIN USB_OTG_FS_MspInit 1 */

  /* USER CODE END USB_OTG_FS_MspInit 1 */
  }
}

void HAL_HCD_MspDeInit(HCD_HandleTypeDef* hcdHandle)
{
  if(hcdHandle->Instance==USB_OTG_FS)
  {
  /* USER CODE BEGIN USB_OTG_FS_MspDeInit 0 */

  /* USER CODE END USB_OTG_FS_MspDeInit 0 */
    /* Disable Peripheral clock */
    __HAL_RCC_USB_OTG_FS_CLK_DISABLE();
    __HAL_RCC_USBPHYC_CLK_DISABLE();

    /* Peripheral interrupt Deinit*/
    HAL_NVIC_DisableIRQ(OTG_FS_IRQn);

  /* USER CODE BEGIN USB_OTG_FS_MspDeInit 1 */

  /* USER CODE END USB_OTG_FS_MspDeInit 1 */
  }
}

/**
  * @brief  SOF callback.
  * @param  hhcd: HCD handle
  * @retval None
  */
/* 抽送改由 TIM7 8kHz 做（見 audio.c）。SOF 1kHz 實測只有 500 包/秒：
 * 每個訊框進來時上一筆 URB 還沒被標記成 URB_DONE。
 * 這個旗標只當「主迴圈不要再驅動 USBH_Process」用。 */
volatile uint8_t g_usb_from_sof = 0U;
extern USBH_HandleTypeDef hUsbHostFS;

void HAL_HCD_SOF_Callback(HCD_HandleTypeDef *hhcd)
{
  USBH_LL_IncTimer(hhcd->pData);
}

/**
  * @brief  SOF callback.
  * @param  hhcd: HCD handle
  * @retval None
  */
void HAL_HCD_Connect_Callback(HCD_HandleTypeDef *hhcd)
{
  USBH_LL_Connect(hhcd->pData);
}

/**
  * @brief  SOF callback.
  * @param  hhcd: HCD handle
  * @retval None
  */
void HAL_HCD_Disconnect_Callback(HCD_HandleTypeDef *hhcd)
{
  USBH_LL_Disconnect(hhcd->pData);
}

/**
  * @brief  Notify URB state change callback.
  * @param  hhcd: HCD handle
  * @param  chnum: channel number
  * @param  urb_state: state
  * @retval None
  */
void HAL_HCD_HC_NotifyURBChange_Callback(HCD_HandleTypeDef *hhcd, uint8_t chnum, HCD_URBStateTypeDef urb_state)
{
  /* To be used with OS to sync URB state with the global state machine */
#if (USBH_USE_OS == 1)
  USBH_LL_NotifyURBChange(hhcd->pData);
#endif
}
/**
* @brief  Port Port Enabled callback.
  * @param  hhcd: HCD handle
  * @retval None
  */
void HAL_HCD_PortEnabled_Callback(HCD_HandleTypeDef *hhcd)
{
  USBH_LL_PortEnabled(hhcd->pData);
}

/**
  * @brief  Port Port Disabled callback.
  * @param  hhcd: HCD handle
  * @retval None
  */
void HAL_HCD_PortDisabled_Callback(HCD_HandleTypeDef *hhcd)
{
  USBH_LL_PortDisabled(hhcd->pData);
}

/*******************************************************************************
                       LL Driver Interface (USB Host Library --> HCD)
*******************************************************************************/

/**
  * @brief  Initialize the low level portion of the host driver.
  * @param  phost: Host handle
  * @retval USBH status
  */
USBH_StatusTypeDef USBH_LL_Init(USBH_HandleTypeDef *phost)
{
  /* Init USB_IP */
  if (phost->id == HOST_FS) {
  /* Link the driver to the stack. */
  hhcd_USB_OTG_FS.pData = phost;
  phost->pData = &hhcd_USB_OTG_FS;

  hhcd_USB_OTG_FS.Instance = USB_OTG_FS;
  hhcd_USB_OTG_FS.Init.Host_channels = 12;
  hhcd_USB_OTG_FS.Init.speed = USB_OTG_SPEED_FULL;
  hhcd_USB_OTG_FS.Init.dma_enable = DISABLE;
  hhcd_USB_OTG_FS.Init.phy_itface = HCD_PHY_EMBEDDED;
  hhcd_USB_OTG_FS.Init.Sof_enable = DISABLE;
  hhcd_USB_OTG_FS.Init.vbus_sensing_enable = DISABLE;
  if (HAL_HCD_Init(&hhcd_USB_OTG_FS) != HAL_OK)
  {
    Error_Handler();
  }

  USBH_LL_SetTimer(phost, HAL_HCD_GetCurrentFrame(&hhcd_USB_OTG_FS));
  }
  return USBH_OK;
}

/**
  * @brief  De-Initialize the low level portion of the host driver.
  * @param  phost: Host handle
  * @retval USBH status
  */
USBH_StatusTypeDef USBH_LL_DeInit(USBH_HandleTypeDef *phost)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBH_StatusTypeDef usb_status = USBH_OK;

  hal_status = HAL_HCD_DeInit(phost->pData);

  usb_status = USBH_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Start the low level portion of the host driver.
  * @param  phost: Host handle
  * @retval USBH status
  */
USBH_StatusTypeDef USBH_LL_Start(USBH_HandleTypeDef *phost)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBH_StatusTypeDef usb_status = USBH_OK;

  hal_status = HAL_HCD_Start(phost->pData);

  usb_status = USBH_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Stop the low level portion of the host driver.
  * @param  phost: Host handle
  * @retval USBH status
  */
USBH_StatusTypeDef USBH_LL_Stop(USBH_HandleTypeDef *phost)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBH_StatusTypeDef usb_status = USBH_OK;

  hal_status = HAL_HCD_Stop(phost->pData);

  usb_status = USBH_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Return the USB host speed from the low level driver.
  * @param  phost: Host handle
  * @retval USBH speeds
  */
USBH_SpeedTypeDef USBH_LL_GetSpeed(USBH_HandleTypeDef *phost)
{
  USBH_SpeedTypeDef speed = USBH_SPEED_FULL;

  switch (HAL_HCD_GetCurrentSpeed(phost->pData))
  {
  case 0 :
    speed = USBH_SPEED_HIGH;
    break;

  case 1 :
    speed = USBH_SPEED_FULL;
    break;

  case 2 :
    speed = USBH_SPEED_LOW;
    break;

  default:
   speed = USBH_SPEED_FULL;
    break;
  }
  return  speed;
}

/**
  * @brief  Reset the Host port of the low level driver.
  * @param  phost: Host handle
  * @retval USBH status
  */
USBH_StatusTypeDef USBH_LL_ResetPort(USBH_HandleTypeDef *phost)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBH_StatusTypeDef usb_status = USBH_OK;

  hal_status = HAL_HCD_ResetPort(phost->pData);

  usb_status = USBH_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Return the last transferred packet size.
  * @param  phost: Host handle
  * @param  pipe: Pipe index
  * @retval Packet size
  */
uint32_t USBH_LL_GetLastXferSize(USBH_HandleTypeDef *phost, uint8_t pipe)
{
  return HAL_HCD_HC_GetXferCount(phost->pData, pipe);
}

/**
  * @brief  Open a pipe of the low level driver.
  * @param  phost: Host handle
  * @param  pipe_num: Pipe index
  * @param  epnum: Endpoint number
  * @param  dev_address: Device USB address
  * @param  speed: Device Speed
  * @param  ep_type: Endpoint type
  * @param  mps: Endpoint max packet size
  * @retval USBH status
  */
USBH_StatusTypeDef USBH_LL_OpenPipe(USBH_HandleTypeDef *phost, uint8_t pipe_num, uint8_t epnum,
                                    uint8_t dev_address, uint8_t speed, uint8_t ep_type, uint16_t mps)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBH_StatusTypeDef usb_status = USBH_OK;

  hal_status = HAL_HCD_HC_Init(phost->pData, pipe_num, epnum,
                               dev_address, speed, ep_type, mps);

  usb_status = USBH_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Close a pipe of the low level driver.
  * @param  phost: Host handle
  * @param  pipe: Pipe index
  * @retval USBH status
  */
USBH_StatusTypeDef USBH_LL_ClosePipe(USBH_HandleTypeDef *phost, uint8_t pipe)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBH_StatusTypeDef usb_status = USBH_OK;

  hal_status = HAL_HCD_HC_Halt(phost->pData, pipe);

  usb_status = USBH_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Submit a new URB to the low level driver.
  * @param  phost: Host handle
  * @param  pipe: Pipe index
  *         This parameter can be a value from 1 to 15
  * @param  direction : Channel number
  *          This parameter can be one of the these values:
  *           0 : Output
  *           1 : Input
  * @param  ep_type : Endpoint Type
  *          This parameter can be one of the these values:
  *            @arg EP_TYPE_CTRL: Control type
  *            @arg EP_TYPE_ISOC: Isochrounous type
  *            @arg EP_TYPE_BULK: Bulk type
  *            @arg EP_TYPE_INTR: Interrupt type
  * @param  token : Endpoint Type
  *          This parameter can be one of the these values:
  *            @arg 0: PID_SETUP
  *            @arg 1: PID_DATA
  * @param  pbuff : pointer to URB data
  * @param  length : Length of URB data
  * @param  do_ping : activate do ping protocol (for high speed only)
  *          This parameter can be one of the these values:
  *           0 : do ping inactive
  *           1 : do ping active
  * @retval Status
  */
USBH_StatusTypeDef USBH_LL_SubmitURB(USBH_HandleTypeDef *phost, uint8_t pipe, uint8_t direction,
                                     uint8_t ep_type, uint8_t token, uint8_t *pbuff, uint16_t length,
                                     uint8_t do_ping)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBH_StatusTypeDef usb_status = USBH_OK;

  hal_status = HAL_HCD_HC_SubmitRequest(phost->pData, pipe, direction ,
                                        ep_type, token, pbuff, length,
                                        do_ping);
  usb_status =  USBH_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Get a URB state from the low level driver.
  * @param  phost: Host handle
  * @param  pipe: Pipe index
  *         This parameter can be a value from 1 to 15
  * @retval URB state
  *          This parameter can be one of the these values:
  *            @arg URB_IDLE
  *            @arg URB_DONE
  *            @arg URB_NOTREADY
  *            @arg URB_NYET
  *            @arg URB_ERROR
  *            @arg URB_STALL
  */
USBH_URBStateTypeDef USBH_LL_GetURBState(USBH_HandleTypeDef *phost, uint8_t pipe)
{
  return (USBH_URBStateTypeDef)HAL_HCD_HC_GetURBState (phost->pData, pipe);
}

/**
  * @brief  Drive VBUS.
  * @param  phost: Host handle
  * @param  state : VBUS state
  *          This parameter can be one of the these values:
  *           0 : VBUS Inactive
  *           1 : VBUS Active
  * @retval Status
  */
USBH_StatusTypeDef USBH_LL_DriverVBUS(USBH_HandleTypeDef *phost, uint8_t state)
{

  /* USER CODE BEGIN 0 */

  /* USER CODE END 0*/

  if (phost->id == HOST_FS)
  {
    if (state == 0)
    {
      /* Drive high Charge pump */
      /* ToDo: Add IOE driver control */
      /* USER CODE BEGIN DRIVE_HIGH_CHARGE_FOR_FS */

      /* USER CODE END DRIVE_HIGH_CHARGE_FOR_FS */
    }
    else
    {
      /* Drive low Charge pump */
      /* ToDo: Add IOE driver control */
      /* USER CODE BEGIN DRIVE_LOW_CHARGE_FOR_FS */

      /* USER CODE END DRIVE_LOW_CHARGE_FOR_FS */
    }
  }
  /* --- **不要用 HAL_Delay。** ------------------------------------------
   * 實測 usbaudio_init() 卡死在 USBH_Start 裡，而 USBH_Start 唯一會等的
   * 就是這一行。HAL_Delay 忙等 uwTick，而 uwTick 靠 SysTick 中斷更新 ——
   * 卡住只有一個意思：那一刻 SysTick 沒在跑。
   *
   * 這裡只是要等 VBUS 穩定，不需要精確的毫秒，改成不依賴中斷的空轉。
   * 600MHz 下大約 200 毫秒。 */
  {
    volatile uint32_t spin;

    for (spin = 0U; spin < 30000000U; spin++)
    {
      __NOP();
    }
  }
  return USBH_OK;
}

/**
  * @brief  Set toggle for a pipe.
  * @param  phost: Host handle
  * @param  pipe: Pipe index
  * @param  toggle: toggle (0/1)
  * @retval Status
  */
USBH_StatusTypeDef USBH_LL_SetToggle(USBH_HandleTypeDef *phost, uint8_t pipe, uint8_t toggle)
{
  HCD_HandleTypeDef *pHandle;
  pHandle = phost->pData;

  if(pHandle->hc[pipe].ep_is_in)
  {
    pHandle->hc[pipe].toggle_in = toggle;
  }
  else
  {
    pHandle->hc[pipe].toggle_out = toggle;
  }

  return USBH_OK;
}

/**
  * @brief  Return the current toggle of a pipe.
  * @param  phost: Host handle
  * @param  pipe: Pipe index
  * @retval toggle (0/1)
  */
uint8_t USBH_LL_GetToggle(USBH_HandleTypeDef *phost, uint8_t pipe)
{
  uint8_t toggle = 0;
  HCD_HandleTypeDef *pHandle;
  pHandle = phost->pData;

  if(pHandle->hc[pipe].ep_is_in)
  {
    toggle = pHandle->hc[pipe].toggle_in;
  }
  else
  {
    toggle = pHandle->hc[pipe].toggle_out;
  }
  return toggle;
}

/**
  * @brief  Delay routine for the USB Host Library
  * @param  Delay: Delay in ms
  * @retval None
  */
void USBH_Delay(uint32_t Delay)
{
  HAL_Delay(Delay);
}

/**
  * @brief  Returns the USB status depending on the HAL status:
  * @param  hal_status: HAL status
  * @retval USB status
  */
USBH_StatusTypeDef USBH_Get_USB_Status(HAL_StatusTypeDef hal_status)
{
  USBH_StatusTypeDef usb_status = USBH_OK;

  switch (hal_status)
  {
    case HAL_OK :
      usb_status = USBH_OK;
    break;
    case HAL_ERROR :
      usb_status = USBH_FAIL;
    break;
    case HAL_BUSY :
      usb_status = USBH_BUSY;
    break;
    case HAL_TIMEOUT :
      usb_status = USBH_FAIL;
    break;
    default :
      usb_status = USBH_FAIL;
    break;
  }
  return usb_status;
}



/* --- OTG_FS 的中斷處理常式 -------------------------------------------
 * ST 的範例把它放在 cube 的 stm32h7rsxx_it.c，而那個檔是 gitignore 的。
 * 中斷處理常式本來就是覆寫啟動碼的弱符號，放應用層效果一樣
 * （board-notes 23.2 同一招）。 */
void OTG_FS_IRQHandler(void)
{
  /* 數它進來幾次。**中斷風暴是目前唯一還沒排除的假設** ——
   * USB 主機起動了、埠供電了、dongle 掛著，但沒有人呼叫 USBH_Process
   * 去把狀態推進，中斷旗標就可能一直重新觸發，把主迴圈餓死。
   * 幾秒內衝到百萬就是風暴；只有幾十次就不是。 */
  ((volatile uint32_t *)0x20004020u)[185]++;
  HAL_HCD_IRQHandler(&hhcd_USB_OTG_FS);
}
