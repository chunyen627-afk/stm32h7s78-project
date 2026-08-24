#!/usr/bin/env python3
"""把 Template_XIP 改造成能跑電子相簿的專案。

Template_XIP 只會閃 LED，要跑相簿必須補四件事：
  1. 開啟 LTDC / DMA2D / I2C / XSPI / SD / JPEG 等 HAL 模組
  2. 把 BSP、元件驅動、FatFs、相簿原始碼加進 .project 的 linked resources
  3. 補 include 路徑
  4. 改掉 FatFs 的預設設定（長檔名、字碼頁）

CubeIDE 沒有命令列可以做這些，所以直接改專案檔。
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CUBE = os.environ.get("CUBE_DIR") or \
    os.path.join(ROOT, "..", "stm32h7s78-tetris", "cube")
CUBE = os.path.abspath(CUBE)

PROJ = os.path.join(CUBE, "Projects", "STM32H7S78-DK", "Templates", "Album")
APPLI = os.path.join(PROJ, "STM32CubeIDE", "Appli")

# 專案檔用 PARENT-n-PROJECT_LOC 表示往上 n 層。Appli 專案在
# Templates/Album/STM32CubeIDE/Appli，往上 6 層是韌體包根目錄。
PKG = "PARENT-6-PROJECT_LOC"
SRC = "PARENT-2-PROJECT_LOC"

# 沒有這些模組，BSP 的 LCD/觸控/PSRAM/SD 都編不過。
HAL_MODULES = ["LTDC", "I2C", "DMA2D", "XSPI", "LPTIM", "SDRAM",
               "GFXTIM", "RAMCFG", "MDF", "CRC", "SD", "JPEG", "IWDG",
               "ADC"]   # ADC 只為了量 VBUS（偵測 USB 線插上沒），見 vbus.c

HAL_SOURCES = [
    "stm32h7rsxx_hal_ltdc.c", "stm32h7rsxx_hal_ltdc_ex.c",
    "stm32h7rsxx_hal_i2c.c", "stm32h7rsxx_hal_i2c_ex.c",
    "stm32h7rsxx_hal_dma2d.c", "stm32h7rsxx_hal_xspi.c",
    "stm32h7rsxx_hal_lptim.c",
    "stm32h7rsxx_hal_sd.c", "stm32h7rsxx_hal_sd_ex.c",
    "stm32h7rsxx_ll_sdmmc.c",
    "stm32h7rsxx_hal_jpeg.c",
    "stm32h7rsxx_hal_iwdg.c",
    # 影片用 JPEG 硬體解碼的 DMA 模式（HPDMA），照片的輪詢解碼不需要。
    "stm32h7rsxx_hal_dma.c", "stm32h7rsxx_hal_dma_ex.c",
    # 量 CN18 的 VBUS 用（ADC2 通道 6），校正程序在 _ex 裡。
    "stm32h7rsxx_hal_adc.c", "stm32h7rsxx_hal_adc_ex.c",
]

BSP_SOURCES = [
    "stm32h7s78_discovery_lcd.c",
    "stm32h7s78_discovery_ts.c",
    "stm32h7s78_discovery_bus.c",
    "stm32h7s78_discovery_xspi.c",
    "stm32h7s78_discovery_sd.c",
]

COMPONENTS = [
    "gt911/gt911.c", "gt911/gt911_reg.c",
    "aps256xx/aps256xx.c", "mx66uw1g45g/mx66uw1g45g.c",
]

# FatFs 中介層。sd_diskio.c 把 FatFs 接到 BSP_SD_*。
FATFS_SOURCES = [
    ("diskio.c", "source/diskio.c"),
    ("ff.c", "source/ff.c"),
    ("ff_gen_drv.c", "source/ff_gen_drv.c"),
    ("ffsystem_template.c", "source/ffsystem_template.c"),
    ("ffunicode.c", "source/ffunicode.c"),
]

# 不使用 ST 的 fatfs.c：它的 MX_FATFS_Process 含 f_mkfs（格式化）與寫測試檔的
# 路徑。相簿只讀不寫，掛載那幾行自己寫，把誤格式化的可能性從根本移除。
APP_SOURCES = []

ALBUM_SOURCES = ["album_main.c", "photo.c", "video.c", "favorites.c",
                 "vbus.c",
                 "usbdrive.c",
                 "sd_bsp_diskio.c", "xspi_psram.c",
                 "gfx.c", "font_zh.c"]   # gfx/xspi_psram 來自 repo 的 shared/

# ST 的 YCbCr -> RGB 轉換工具，處理 JPEG 解碼器那種 MCU 區塊輸出格式。
UTIL_SOURCES = [("jpeg_utils.c", "JPEG/jpeg_utils.c")]

INCLUDES = [
    f"{'../' * 7}Drivers/BSP/STM32H7S78-DK",
    f"{'../' * 7}Drivers/BSP/Components/Common",
    f"{'../' * 7}Drivers/BSP/Components/gt911",
    f"{'../' * 7}Drivers/BSP/Components/rk050hr18",
    f"{'../' * 7}Drivers/BSP/Components/aps256xx",
    f"{'../' * 7}Drivers/BSP/Components/mx66uw1g45g",
    f"{'../' * 7}Middlewares/Third_Party/FatFs/source",
    f"{'../' * 7}Middlewares/Third_Party/FatFs/source/drivers/sd",
    f"{'../' * 7}Utilities/Fonts",
    f"{'../' * 7}Utilities/JPEG",
    "../../../Appli/Album",
]


def rename_project():
    """Template_XIP 複製過來的專案叫 Template_XIP_Appli，會跟 Tetris 撞名。

    同一個 CubeIDE workspace 不能有兩個同名專案，import 時會直接失敗。
    順便把產出的 elf 也改名，免得兩個專案的建置產物看起來一樣。
    """
    proj = os.path.join(APPLI, ".project")
    s = io.open(proj, encoding="utf-8").read()
    if "<name>Album_Appli</name>" in s:
        print("  專案名稱已改過")
        return
    s = s.replace("<name>Template_XIP_Appli</name>",
                  "<name>Album_Appli</name>", 1)
    io.open(proj, "w", encoding="utf-8").write(s)

    cproj = os.path.join(APPLI, ".cproject")
    c = io.open(cproj, encoding="utf-8").read()
    c = c.replace('artifactName="Template_XIP_Appli"',
                  'artifactName="Album_Appli"')
    io.open(cproj, "w", encoding="utf-8").write(c)
    print("  專案改名: Template_XIP_Appli -> Album_Appli")


def enable_hal_modules():
    path = os.path.join(PROJ, "Appli", "Inc", "stm32h7rsxx_hal_conf.h")
    s = io.open(path, encoding="utf-8").read()
    n = 0
    for m in HAL_MODULES:
        old = f"/* #define HAL_{m}_MODULE_ENABLED   */"
        if old in s:
            s = s.replace(old, f"#define HAL_{m}_MODULE_ENABLED")
            n += 1
    io.open(path, "w", encoding="utf-8").write(s)
    print(f"  HAL 模組啟用: {n}")


def patch_ffconf():
    """ST 的預設值對相簿不能用。

    FF_USE_LFN=0 會讓 IMG_20240101_120000.jpg 變成 IMG_20~1.JPG，
    中文資料夾名稱也會整個壞掉。字碼頁預設是 932（日文），要換成 950（繁中）。
    檔名用 UTF-8 讀出來，字型查表才好對。
    """
    path = os.path.join(PROJ, "Appli", "Inc", "ffconf.h")
    s = io.open(path, encoding="utf-8").read()
    changes = {
        r"#define FF_CODE_PAGE\s+\d+": "#define FF_CODE_PAGE    950",
        r"#define FF_USE_LFN\s+\d+": "#define FF_USE_LFN      1",
        # exFAT：大容量卡（>32GB）出廠幾乎都是 exFAT，關著就直接掛不起來。
        # 多約 10KB，而外部 Flash 有 128MB、目前只用 1.5MB，不痛。
        # 需要 FF_USE_LFN >= 1（上面已經開了）。
        r"#define FF_FS_EXFAT\s+\d+": "#define FF_FS_EXFAT     1",
        r"#define FF_LFN_UNICODE\s+\d+": "#define FF_LFN_UNICODE  2",
        # FF_FS_LOCK=2 表示同時最多只能開 2 個檔案或目錄。遞迴掃描時每一層
        # 都佔一個 DIR，第三層再開檔案讀 JPEG 檔頭就會拿到
        # FR_TOO_MANY_OPEN_FILES(18)。單執行緒唯讀不需要這個鎖，關掉。
        r"#define FF_FS_LOCK\s+\d+": "#define FF_FS_LOCK      0",
    }
    n = 0
    for pat, rep in changes.items():
        s, k = re.subn(pat, rep, s, count=1)
        n += k
    io.open(path, "w", encoding="utf-8").write(s)
    print(f"  ffconf.h 調整: {n} 項（長檔名、繁中字碼頁、UTF-8）")


def link(name, uri):
    return ("\t\t<link>\n"
            f"\t\t\t<name>{name}</name>\n"
            "\t\t\t<type>1</type>\n"
            f"\t\t\t<locationURI>{uri}</locationURI>\n"
            "\t\t</link>\n")


def add_sources():
    path = os.path.join(APPLI, ".project")
    s = io.open(path, encoding="utf-8").read()
    have = set(re.findall(r"<name>([^<]+\.c)</name>", s))

    items = []
    for f in ALBUM_SOURCES:
        items.append((f"Application/Album/{f}", f"{SRC}/Appli/Album/{f}"))
    for f in APP_SOURCES:
        items.append((f"Application/User/{f}", f"{SRC}/Appli/Src/{f}"))
    for f in HAL_SOURCES:
        items.append((f"Drivers/STM32H7RSxx_HAL_Driver/{f}",
                      f"{PKG}/Drivers/STM32H7RSxx_HAL_Driver/Src/{f}"))
    for f in BSP_SOURCES:
        items.append((f"Drivers/BSP/STM32H7S78-DK/{f}",
                      f"{PKG}/Drivers/BSP/STM32H7S78-DK/{f}"))
    for f in COMPONENTS:
        items.append((f"Drivers/BSP/Components/{f}",
                      f"{PKG}/Drivers/BSP/Components/{f}"))
    for name, rel in FATFS_SOURCES:
        items.append((f"Middlewares/FatFs/{name}",
                      f"{PKG}/Middlewares/Third_Party/FatFs/{rel}"))
    for name, rel in UTIL_SOURCES:
        items.append((f"Utilities/{name}", f"{PKG}/Utilities/{rel}"))

    block = "".join(link(n, u) for n, u in items if n not in have)
    added = sum(1 for n, _ in items if n not in have)
    if block:
        i = s.rindex("\t</linkedResources>")
        s = s[:i] + block + s[i:]
        io.open(path, "w", encoding="utf-8").write(s)
    print(f"  原始碼連結新增: {added}")


def add_includes():
    path = os.path.join(APPLI, ".cproject")
    s = io.open(path, encoding="utf-8").read()
    anchor = ('<listOptionValue builtIn="false" '
              f'value="{"../" * 7}Drivers/CMSIS/Include"/>')
    if anchor not in s:
        print("  警告: 找不到 include 錨點，略過")
        return
    missing = [i for i in INCLUDES if f'value="{i}"' not in s]
    if missing:
        s = s.replace(anchor, anchor + "".join(
            f'\n<listOptionValue builtIn="false" value="{i}"/>' for i in missing))
        io.open(path, "w", encoding="utf-8").write(s)
    print(f"  include 路徑新增: {len(missing)}")


def patch_boot_hse():
    """bootloader 的 SystemClock_Config 順便把 HSE 打開。

    PLL 來源仍然是 HSI，所以相簿的時序完全不變 —— 多開 HSE 純粹是為了 USB：
    USB HS 的 PHY 需要 RCC_USBPHYCCLKSOURCE_HSE，而隨身碟 app
    （MSC_Standalone）**沒有自己的 SystemClock_Config**，時脈完全繼承
    bootloader。它原廠的 bootloader 走 HSE 所以沒事，這顆走 HSI，HSE 從頭到尾
    沒人開 -> PHY 沒有時脈 -> USB 完全不列舉。

    症狀極度誤導：app 跑得好好的、中斷正常、SD 也讀得到，就是電腦看不到裝置。
    從 app 那邊事後補開沒有用（PLL 已經在跑，HAL 會拒絕），一定要在這裡。
    """
    path = os.path.join(PROJ, "Boot", "Src", "main.c")
    s = io.open(path, encoding="utf-8").read()

    if "RCC_OSCILLATORTYPE_HSE" in s:
        print("  Boot main.c 已改過")
        return

    old = ("  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;\n"
           "  RCC_OscInitStruct.HSIState = RCC_HSI_ON;\n")
    if old not in s:
        print("  !! Boot main.c 找不到振盪器設定，沒有套用 HSE")
        return

    s = s.replace(old,
        "  /* HSE 也一起開起來。PLL 來源仍是 HSI，相簿的時序完全不變 ——\n"
        "   * 多開 HSE 是給 USB 的 PHY 用（見 patch_project.py 的說明）。 */\n"
        "  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_HSE;\n"
        "  RCC_OscInitStruct.HSIState = RCC_HSI_ON;\n"
        "  RCC_OscInitStruct.HSEState = RCC_HSE_ON;\n")

    io.open(path, "w", encoding="utf-8").write(s)
    print("  Boot main.c 已補上 HSE（USB PHY 需要）")


def patch_main():
    """把範本的 LED 閃爍主迴圈換成呼叫相簿。"""
    path = os.path.join(PROJ, "Appli", "Src", "main.c")
    s = io.open(path, encoding="utf-8").read()
    before = s

    # 不用「已改過就整段跳出」的防護：那樣之後新增的植入永遠套不上去
    # （加隨身碟跳轉時就踩到）。每個 replace 的樣式在套用後都不再匹配，
    # 本身就是冪等的。

    s = s.replace(
        "/* USER CODE BEGIN Includes */\n\n/* USER CODE END Includes */",
        "/* USER CODE BEGIN Includes */\n"
        '#include "stm32h7s78_discovery_lcd.h"\n'
        '#include "stm32h7s78_discovery_ts.h"\n'
        "/* USER CODE END Includes */")

    s = s.replace(
        "/* USER CODE BEGIN 0 */\n\n/* USER CODE END 0 */",
        "/* USER CODE BEGIN 0 */\n"
        "/* 相簿進入點，定義在 album_main.c，不會返回。 */\n"
        "void album_run(void);\n"
        "/* USER CODE END 0 */")

    s = s.replace(
        "  /* Initialize LD1 */\n  BSP_LED_Init(LD1);\n  /* USER CODE END 2 */",
        "  /* Initialize LD1 */\n  BSP_LED_Init(LD1);\n\n"
        "  /* 交給相簿主迴圈，不會返回。 */\n  album_run();\n"
        "  /* USER CODE END 2 */")

    # 隨身碟模式的跳轉。**一定要是 main() 的第一件事** —— 再往下就會設定 MPU、
    # 快取與一堆周邊，而隨身碟 app 假設自己是從 bootloader 剛交棒的狀態開始跑。
    # 放在 album_run() 裡試過，跳轉會成功（VTOR 讀出來就是新位址、中斷也正常）
    # 但 USB 完全不列舉。
    s = s.replace(
        "  /* USER CODE BEGIN 1 */\n  MPU_Config();",
        "  /* USER CODE BEGIN 1 */\n"
        "  /* ---- 隨身碟模式：在動任何東西之前先判斷要不要讓位 ------------------\n"
        "   *\n"
        "   * 外部 Flash 放兩個 app：0x70000000 相簿、0x71000000 MSC_Standalone。\n"
        "   * 相簿偵測到 USB 線插上時（見 app_src/usbdrive.c），會在 AXI SRAM 頂端\n"
        "   * 留下暗號再重置；這裡看到暗號就跳過去。\n"
        "   *\n"
        "   * 暗號一次有效（跳之前就清掉），所以隨身碟 app 之後不管是拔線重置還是\n"
        "   * 當掉重置，下一次開機一定回到相簿，不會卡在隨身碟模式。\n"
        "   */\n"
        "  {\n"
        "    /* 暗號放 DTCM。**不能放 AXI SRAM** —— 原本放 0x24071BF0，那裡在\n"
        "     * bootloader 堆疊頂端（SP = 0x24072000）下面只有 1040 bytes，會被\n"
        "     * bootloader 的堆疊踩掉。症狀是「有時候回得來、有時候回不來」。\n"
        "     * DTCM 是緊耦合記憶體，bootloader 完全不碰，而且不經過快取。 */\n"
        "    volatile uint32_t *flag = (volatile uint32_t *)0x20004000u;\n"
        "\n"
        "    if (*flag == 0x55534244u)   /* \"USBD\" */\n"
        "    {\n"
        "      uint32_t sp     = *(volatile uint32_t *)0x71000000u;\n"
        "      uint32_t region = sp & 0xFF000000u;\n"
        "\n"
        "      *flag = 0u;\n"
        "      /* DTCM 不經過快取，寫進去就是寫進去了，不必 clean。\n"
        "       * 放 AXI SRAM 的時候就沒這麼單純：寫入會留在 D-Cache，而隨身碟\n"
        "       * app 的 SD 讀取路徑有 SCB_InvalidateDCache()，整體失效會把髒\n"
        "       * 快取行直接丟掉 -> SRAM 裡留的還是舊暗號 -> 下次開機又跳過去。 */\n"
        "      __DSB();\n"
        "\n"
        "      /* 那個位址真的有 app 才跳。沒燒過會讀到 0xFFFFFFFF，照跳直接進\n"
        "       * HardFault，症狀是「插上 USB 就變磚」。\n"
        "       * DTCM / AXI SRAM / AHB SRAM 都是合法的堆疊位置 —— 隨身碟 app 的\n"
        "       * 堆疊在 DTCM(0x20010000)，跟相簿不同，不要只認自己那一種。 */\n"
        "      if (region == 0x20000000u || region == 0x24000000u || region == 0x30000000u)\n"
        "      {\n"
        "        typedef void (*pFunction)(void);\n"
        "        pFunction jump_to_usb =\n"
        "            (pFunction)(*(volatile uint32_t *)(0x71000000u + 4u));\n"
        "\n"
        "        SCB->VTOR = 0x71000000u;\n"
        "        __set_MSP(sp);\n"
        "        jump_to_usb();\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "  /* -------------------------------------------------------------------- */\n"
        "\n"
        "  MPU_Config();")

    if s == before:
        print("  main.c 已改過")
        return

    io.open(path, "w", encoding="utf-8").write(s)
    print("  main.c 已接上 album_run() 與隨身碟跳轉")


def main():
    if not os.path.isdir(PROJ):
        print(f"找不到專案目錄: {PROJ}")
        print("請先執行 scripts/setup.sh")
        return 1
    print("套用專案設定：")
    rename_project()
    enable_hal_modules()
    patch_ffconf()
    add_sources()
    add_includes()
    patch_main()
    patch_boot_hse()
    return 0


if __name__ == "__main__":
    sys.exit(main())
