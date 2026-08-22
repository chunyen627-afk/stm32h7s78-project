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

PROJ = os.path.join(CUBE, "Projects", "STM32H7S78-DK", "Templates", "Video")
APPLI = os.path.join(PROJ, "STM32CubeIDE", "Appli")

# 專案檔用 PARENT-n-PROJECT_LOC 表示往上 n 層。Appli 專案在
# Templates/Video/STM32CubeIDE/Appli，往上 6 層是韌體包根目錄。
PKG = "PARENT-6-PROJECT_LOC"
SRC = "PARENT-2-PROJECT_LOC"

# 沒有這些模組，BSP 的 LCD/觸控/PSRAM/SD 都編不過。
HAL_MODULES = ["LTDC", "I2C", "DMA2D", "XSPI", "LPTIM", "SDRAM",
               "GFXTIM", "RAMCFG", "MDF", "CRC", "SD", "JPEG", "IWDG"]

HAL_SOURCES = [
    "stm32h7rsxx_hal_ltdc.c", "stm32h7rsxx_hal_ltdc_ex.c",
        "stm32h7rsxx_hal_dma2d.c", "stm32h7rsxx_hal_xspi.c",
    "stm32h7rsxx_hal_lptim.c",
            "stm32h7rsxx_hal_jpeg.c",
    "stm32h7rsxx_hal_dma.c", "stm32h7rsxx_hal_dma_ex.c",
    ]

BSP_SOURCES = [
    "stm32h7s78_discovery_lcd.c",
        "stm32h7s78_discovery_bus.c",
    "stm32h7s78_discovery_xspi.c",
    ]

COMPONENTS = [
    
    "aps256xx/aps256xx.c", "mx66uw1g45g/mx66uw1g45g.c",
]

# FatFs 中介層。sd_diskio.c 把 FatFs 接到 BSP_SD_*。
FATFS_SOURCES = []

# 不使用 ST 的 fatfs.c：它的 MX_FATFS_Process 含 f_mkfs（格式化）與寫測試檔的
# 路徑。相簿只讀不寫，掛載那幾行自己寫，把誤格式化的可能性從根本移除。
APP_SOURCES = []

VIDEO_SOURCES = ["video_main.c", "xspi_psram.c"]   # gfx/xspi_psram 來自 repo 的 shared/

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
    "../../../Appli/Video",
]


def rename_project():
    """Template_XIP 複製過來的專案叫 Template_XIP_Appli，會跟 Tetris 撞名。

    同一個 CubeIDE workspace 不能有兩個同名專案，import 時會直接失敗。
    順便把產出的 elf 也改名，免得兩個專案的建置產物看起來一樣。
    """
    proj = os.path.join(APPLI, ".project")
    s = io.open(proj, encoding="utf-8").read()
    if "<name>Video_Appli</name>" in s:
        print("  專案名稱已改過")
        return
    s = s.replace("<name>Template_XIP_Appli</name>",
                  "<name>Video_Appli</name>", 1)
    io.open(proj, "w", encoding="utf-8").write(s)

    cproj = os.path.join(APPLI, ".cproject")
    c = io.open(cproj, encoding="utf-8").read()
    c = c.replace('artifactName="Template_XIP_Appli"',
                  'artifactName="Video_Appli"')
    io.open(cproj, "w", encoding="utf-8").write(c)
    print("  專案改名: Template_XIP_Appli -> Video_Appli")


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


def _unused_patch_ffconf():
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
    for f in VIDEO_SOURCES:
        items.append((f"Application/Video/{f}", f"{SRC}/Appli/Video/{f}"))
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


def patch_main():
    """把範本的 LED 閃爍主迴圈換成呼叫相簿。"""
    path = os.path.join(PROJ, "Appli", "Src", "main.c")
    s = io.open(path, encoding="utf-8").read()

    if "video_run();" in s:
        print("  main.c 已改過")
        return

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
        "  /* 交給相簿主迴圈，不會返回。 */\n  video_run();\n"
        "  /* USER CODE END 2 */")

    io.open(path, "w", encoding="utf-8").write(s)
    print("  main.c 已接上 video_run()")


def main():
    if not os.path.isdir(PROJ):
        print(f"找不到專案目錄: {PROJ}")
        print("請先執行 scripts/setup.sh")
        return 1
    print("套用專案設定：")
    rename_project()
    enable_hal_modules()

    add_sources()
    add_includes()
    patch_main()
    return 0


if __name__ == "__main__":
    sys.exit(main())
