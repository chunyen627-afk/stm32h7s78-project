# 安裝 RaceDash Bridge：複製到 %LOCALAPPDATA%\RaceDash、登入 Windows 時自動啟動、現在就啟動。
#   powershell -ExecutionPolicy Bypass -File install.ps1
# 解除安裝：install.ps1 -Uninstall
param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
$dst  = Join-Path $env:LOCALAPPDATA 'RaceDash'
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
Copy-Item (Join-Path $PSScriptRoot 'build\racedash-bridge.exe') $exe -Force

$sh = New-Object -ComObject WScript.Shell
$s = $sh.CreateShortcut($lnk)
$s.TargetPath = $exe
$s.WorkingDirectory = $dst
$s.Description = 'RaceDash Bridge：賽車遊戲遙測送到儀表板'
$s.Save()

Start-Process $exe -WorkingDirectory $dst
Write-Output "已安裝到 $dst，開機自動啟動：$lnk"
