# RaceDash Bridge

讀賽車遊戲的**官方遙測**，換成板子吃的 DSH1 格式，**每秒 60 次**經 ST-LINK 的虛擬
COM 埠送給儀表。取代 SimHub（免費版只有每秒 10 次，而且要手動選遊戲）。

- 自動偵測現在開的是哪套遊戲（看遊戲行程；F1、Forza 也看有沒有封包進來，所以 PS5 上
  玩 F1 也行），不用手動切
- 自動找 ST-LINK 的 COM 埠，板子拔掉再插回去會自己重連
- 開機自動在背景跑，右下角系統匣有圖示：滑鼠移上去看「遊戲｜COM 埠｜每秒幾筆」，
  右鍵可以開紀錄檔或結束
- 單一個 exe（約 110KB），不用安裝執行環境

## 支援的遊戲

| 遊戲 | 讀法 | 遊戲裡要設定 | 沒有的資料 |
|---|---|---|---|
| Assetto Corsa EVO | 官方共享記憶體 `acevo_pmf_*` | 不用 | — |
| Assetto Corsa Competizione | 官方共享記憶體 `acpmf_*` | 不用 | 總圈數（計時賽） |
| Assetto Corsa Rally | 同 ACC 的共享記憶體 | 不用 | TC/ABS 作動、單圈成績、油耗、名次（搶先體驗版沒發佈） |
| F1 25（也收 F1 24） | 官方 UDP | 設定 → Telemetry Settings：UDP Telemetry **開**、Port **20777 或 20888**（兩個都聽）、Send Rate **60Hz**（UDP Format 2024 / 2025 / 2026 都可以） | TC/ABS 作動（遊戲只給開關設定） |
| Forza Motorsport | 官方 Data Out（UDP） | SETTINGS → GAMEPLAY & HUD → UDP RACE TELEMETRY：Data Out **開**、IP **127.0.0.1**、Port **20777**、Packet Format **Dash** | TC/ABS、總圈數 |
| Forza Horizon 6 | 官方 Data Out（UDP） | SETTINGS → HUD AND GAMEPLAY：Data Out **開**、IP **127.0.0.1**、Port **20777** | TC/ABS、總圈數（只有開車時才送） |

沒有的資料在儀表上顯示 `--`。

**不支援 The Crew Motorfest**：遊戲沒有官方遙測輸出（前作有的輸出只有動態座椅用的物理
資料，沒有轉速、檔位、圈速；ForceSeatPM 也在 2025 年因為遊戲不再送資料而拿掉支援）。
只剩讀遊戲記憶體一途，遊戲更新就壞、也可能被反作弊當成外掛，所以不做。

**同一時間只能一個程式用 COM 埠與 UDP 20777 / 20888**：不要同時開 SimHub。

## 安裝

```bash
./build.sh                                                   # 編譯（w64devkit gcc）
powershell -ExecutionPolicy Bypass -File install.ps1         # 裝到 %USERPROFILE%\RaceDash、開機自動啟動、現在啟動
powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall
```

除錯：`racedash-bridge.exe --console`（開主控台看紀錄）、`--test`（不讀遊戲，送模擬資料
驗證每秒 60 筆）、`--com COM3`（指定 COM 埠）。紀錄檔在 exe 旁邊的 `racedash-bridge.log`。

## 送出的節奏

- 遊戲在跑：每秒 60 次
- 遊戲開著但在選單、暫停：每秒 10 次
- 沒有遊戲：每秒 2 次（「遊戲在跑」= 0，儀表顯示等待畫面）

用 `../tools/measure_fps.sh` 的方法可以從板子量每秒實際收到幾筆（DTCM BOX[3] 的增加速度）。

## 檔案

| 檔案 | 內容 |
|---|---|
| `main.c` | 偵測遊戲、送出節奏、油耗估算（每圈用量取最近三圈平均） |
| `dsh1.c` | 組 DSH1 字串，格式跟 `../simhub/racedash-serial.js` 一模一樣 |
| `serial_win.c` | 找 ST-LINK（登錄檔 USB VID_0483）、115200 8N1、斷線重開 |
| `udp.c` | UDP 20777 與 20888，依封包內容分給 F1 / Forza |
| `src_acevo.c` | AC EVO（Kunos 官方文件 2026-04-01，頁大小 800 / 4900 / 208） |
| `src_acc.c` | ACC 與 AC Rally（Kunos ACC 文件 v1.8.12，頁大小 800 / 1588 / 820；Rally 溫度是 Kelvin） |
| `src_f1.c` | F1 25（EA 規格 2025 與 2026 Season Pack 兩種格式） |
| `src_forza.c` | Forza Motorsport / Horizon（官方 Data Out 文件，依封包長度分） |
| `tray.c` | 系統匣圖示 |

共享記憶體只能用 `OpenFileMapping` 唯讀開：自己建立對應的話，遊戲啟動時發現大小不對
會整場不送遙測（AC EVO 會跳「A 3rd party tool created an outdated share memory」）。

## 驗證狀態

- F1 25（2025 格式 @20777、2026 格式 @20888）、Forza Horizon：用照官方格式組的假封包驗證過解析與每秒 60 筆（板子實收 60 筆、0 錯誤）
- AC EVO、ACC、AC Rally：需要遊戲開著才能驗證
