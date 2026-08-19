#!/usr/bin/env python3
"""把 Template_XIP 改造成能跑遊戲的專案。

Template_XIP 只會閃 LED，要跑圖形程式必須補三件事：
  1. 開啟 LTDC / I2C / DMA2D / XSPI / LPTIM 等 HAL 模組
  2. 把 BSP、元件驅動、遊戲原始碼加進 .project 的 linked resources
  3. 補 include 路徑

CubeIDE 沒有命令列可以做這些，所以直接改專案檔。
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJ = os.path.join(ROOT, "cube", "Projects", "STM32H7S78-DK",
                    "Templates", "Tetris")
APPLI = os.path.join(PROJ, "STM32CubeIDE", "Appli")

# 專案檔用 PARENT-n-PROJECT_LOC 表示往上 n 層。Appli 專案在
# Templates/Tetris/STM32CubeIDE/Appli，往上 6 層是韌體包根目錄。
PKG = "PARENT-6-PROJECT_LOC"
SRC = "PARENT-2-PROJECT_LOC"

# 沒有這些模組，BSP 的 LCD/觸控/PSRAM 都編不過。
HAL_MODULES = ["LTDC", "I2C", "DMA2D", "XSPI", "LPTIM", "SDRAM",
               "GFXTIM", "RAMCFG", "MDF", "CRC"]

HAL_SOURCES = [
    "stm32h7rsxx_hal_ltdc.c", "stm32h7rsxx_hal_ltdc_ex.c",
    "stm32h7rsxx_hal_i2c.c", "stm32h7rsxx_hal_i2c_ex.c",
    "stm32h7rsxx_hal_dma2d.c", "stm32h7rsxx_hal_xspi.c",
    "stm32h7rsxx_hal_lptim.c",
]

BSP_SOURCES = [
    "stm32h7s78_discovery_lcd.c",
    "stm32h7s78_discovery_ts.c",
    "stm32h7s78_discovery_bus.c",
    "stm32h7s78_discovery_xspi.c",
]

COMPONENTS = [
    "gt911/gt911.c", "gt911/gt911_reg.c",
    "aps256xx/aps256xx.c", "mx66uw1g45g/mx66uw1g45g.c",
]

GAME_SOURCES = ["tetris.c", "gfx.c", "ui.c", "input.c",
                "font_zh.c", "game_main.c"]

INCLUDES = [
    f"{'../' * 7}Drivers/BSP/STM32H7S78-DK",
    f"{'../' * 7}Drivers/BSP/Components/Common",
    f"{'../' * 7}Drivers/BSP/Components/gt911",
    f"{'../' * 7}Drivers/BSP/Components/rk050hr18",
    f"{'../' * 7}Drivers/BSP/Components/aps256xx",
    f"{'../' * 7}Drivers/BSP/Components/mx66uw1g45g",
    f"{'../' * 7}Utilities/Fonts",
    "../../../Appli/Game",
]


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
    for f in GAME_SOURCES:
        items.append((f"Application/Game/{f}", f"{SRC}/Appli/Game/{f}"))
    for f in HAL_SOURCES:
        items.append((f"Drivers/STM32H7RSxx_HAL_Driver/{f}",
                      f"{PKG}/Drivers/STM32H7RSxx_HAL_Driver/Src/{f}"))
    for f in BSP_SOURCES:
        items.append((f"Drivers/BSP/STM32H7S78-DK/{f}",
                      f"{PKG}/Drivers/BSP/STM32H7S78-DK/{f}"))
    for f in COMPONENTS:
        items.append((f"Drivers/BSP/Components/{f}",
                      f"{PKG}/Drivers/BSP/Components/{f}"))

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
    """把範本的 LED 閃爍主迴圈換成呼叫遊戲。"""
    path = os.path.join(PROJ, "Appli", "Src", "main.c")
    s = io.open(path, encoding="utf-8").read()

    if "game_run();" in s:
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
        "/* 遊戲進入點，定義在 game_main.c，不會返回。 */\n"
        "void game_run(void);\n"
        "/* USER CODE END 0 */")

    s = s.replace(
        "  /* Initialize LD1 */\n  BSP_LED_Init(LD1);\n  /* USER CODE END 2 */",
        "  /* Initialize LD1 */\n  BSP_LED_Init(LD1);\n\n"
        "  /* 交給遊戲主迴圈，不會返回。 */\n  game_run();\n"
        "  /* USER CODE END 2 */")

    io.open(path, "w", encoding="utf-8").write(s)
    print("  main.c 已接上 game_run()")


def main():
    if not os.path.isdir(PROJ):
        print(f"找不到專案目錄: {PROJ}")
        print("請先執行 scripts/setup.sh")
        return 1
    print("套用專案設定：")
    enable_hal_modules()
    add_sources()
    add_includes()
    patch_main()
    return 0


if __name__ == "__main__":
    sys.exit(main())
