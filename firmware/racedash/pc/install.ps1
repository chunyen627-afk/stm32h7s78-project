# 安裝 RaceDash Bridge：複製到 %USERPROFILE%\RaceDash、登入 Windows 時自動啟動、現在就啟動。
# 不放 AppData：從封裝過的程式（例如 Claude 桌面版）裡執行這個腳本時，寫進 AppData 的檔案
# 會被 Windows 轉到那個程式自己的資料夾，開機啟動和捷徑都找不到。
#   powershell -ExecutionPolicy Bypass -File install.ps1
# 解除安裝：install.ps1 -Uninstall
param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
$dst  = Join-Path $env:USERPROFILE 'RaceDash'
$exe  = Join-Path $dst 'racedash-bridge.exe'
$lnk  = Join-Path ([Environment]::GetFolderPath('Startup')) 'RaceDash Bridge.lnk'

# 先停掉正在跑的（不然檔案被佔用、COM 埠也被佔）
Get-Process racedash-bridge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

if ($Uninstall) {
    Remove-Item $lnk -ErrorAction SilentlyContinue
    Remove-Item $dst -Recurse -ErrorAction SilentlyContinue
    Write-Output '已移除 RaceDash Bridge'
    return
}

New-Item -ItemType Directory -Force $dst | Out-Null
# exe 在同一層（firmware/racedash/pc 的備份）或 build\ 底下（原始碼目錄）
$src = Join-Path $PSScriptRoot 'racedash-bridge.exe'
if (-not (Test-Path $src)) { $src = Join-Path $PSScriptRoot 'build\racedash-bridge.exe' }
Copy-Item $src $exe -Force

$sh = New-Object -ComObject WScript.Shell
$s = $sh.CreateShortcut($lnk)
$s.TargetPath = $exe
$s.WorkingDirectory = $dst
$s.Description = 'RaceDash Bridge：賽車遊戲遙測送到儀表板'
$s.Save()

Start-Process $exe -WorkingDirectory $dst
Write-Output "已安裝到 $dst，開機自動啟動：$lnk"
