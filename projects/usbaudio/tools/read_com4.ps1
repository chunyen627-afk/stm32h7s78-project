# 讀 COM4（ST-LINK 的虛擬序列埠）把治具的輸出存成檔案。
#
# **順序很重要：先開埠，再重置板子。** 反過來的話開機訊息 ——
# 包含描述元傾印 —— 已經送完了，什麼都收不到。
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/read_com4.ps1 -Seconds 35 &
#   sleep 3
#   STM32_Programmer_CLI -c port=SWD mode=UR -rst
#
# mode=HOTPLUG 在這塊板子上連不上（board-notes 23.5），一律用 UR。

param(
    [string]$Port = 'COM4',
    [int]$Seconds = 30,
    [string]$Out = 'C:\Users\user\usbaudio_com4.log'
)

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$sp.ReadTimeout = 500
$sp.Open()

$sw = New-Object System.IO.StreamWriter $Out, $false
$sw.AutoFlush = $true
$deadline = (Get-Date).AddSeconds($Seconds)

while ((Get-Date) -lt $deadline) {
    try {
        $n = $sp.BytesToRead
        if ($n -gt 0) {
            $buf = New-Object byte[] $n
            $sp.Read($buf, 0, $n) | Out-Null
            $sw.Write([System.Text.Encoding]::UTF8.GetString($buf))
        } else {
            Start-Sleep -Milliseconds 20
        }
    } catch {
        Start-Sleep -Milliseconds 20
    }
}

$sw.Close()
$sp.Close()
Write-Host "done"
