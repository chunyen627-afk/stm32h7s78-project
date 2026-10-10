# 假 SimHub：不開遊戲也能測儀表。從電腦送模擬的跑圈遙測（DSH1 格式）到板子。
#
#   powershell -ExecutionPolicy Bypass -File tools\fake_simhub.ps1            # COM3，跑 60 秒
#   powershell -ExecutionPolicy Bypass -File tools\fake_simhub.ps1 -Port COM5 -Seconds 0   # 0 = 一直跑
#   ... -Bad    # 混入格式錯誤的行，看儀表會不會被弄亂（應該不會）
#
# 送的頻率跟 SimHub 免費版一樣是每秒 10 次。SimHub 跟這個工具不能同時開同一個 COM 埠。
param(
    [string]$Port = "COM3",
    [int]$Baud = 115200,
    [int]$Seconds = 60,
    [int]$Hz = 10,
    [switch]$Bad
)

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.NewLine = "`n"
$sp.Open()
Write-Host "送到 $Port @ $Baud，每秒 $Hz 次（Ctrl+C 停止）"

function Lap([double]$sec) {
    if ($sec -lt 0) { return "--" }
    $ms = [int][math]::Round($sec * 1000)
    return "{0:00}:{1:00}.{2:000}" -f [int][math]::Floor($ms / 60000), [int]([math]::Floor($ms / 1000) % 60), ($ms % 1000)
}

$seq = 0
$t0 = Get-Date
$lapLen = 84.0          # 一圈 84 秒
$best = 83.912
$last = 84.631
$fuel = 100.0
$gear = 1
$rpm = 3000.0

while ($true) {
    $t = ((Get-Date) - $t0).TotalSeconds
    if ($Seconds -gt 0 -and $t -ge $Seconds) { break }
    $seq++

    # 一個簡單的「加速換檔、偶爾煞車」循環，每 12 秒一次
    $phase = $t % 12.0
    if ($phase -lt 9.0) {
        $throttle = 100; $brake = 0
        $rpm += 900
        if ($rpm -gt 8200 -and $gear -lt 6) { $gear++; $rpm = 5600 }
        if ($rpm -gt 8400) { $rpm = 8400 }
    } else {
        $throttle = 0; $brake = 85
        $rpm -= 1200
        if ($rpm -lt 4200 -and $gear -gt 2) { $gear--; $rpm = 6800 }
        if ($rpm -lt 2000) { $rpm = 2000 }
    }
    $speed = [int]($gear * 38 + ($rpm - 2000) / 6500 * 40)
    $rpmPct = [int]([math]::Min(100, $rpm / 8500 * 100))
    $lapCur = $t % $lapLen
    $lapNo = [int][math]::Floor($t / $lapLen) + 1
    $fuel = [math]::Max(0, 100 - $t * 0.35)
    $fuelLaps = "{0:0.0}" -f ($fuel / 7.5)
    $tc = if ($phase -gt 2 -and $phase -lt 2.6) { 1 } else { 0 }
    $abs = if ($brake -gt 0 -and $phase -gt 9.4 -and $phase -lt 10.2) { 1 } else { 0 }
    $tyre = @(78.4, 81.2, 92.6, 104.3) | ForEach-Object { "{0:0.0}" -f ($_ + 6 * [math]::Sin($t / 7)) }

    $fields = @(
        "DSH1", $seq, 1, $speed, $gear, [int]$rpm, $rpmPct, 92,
        (Lap $lapCur), (Lap $last), (Lap $best),
        $fuelLaps, 5, $lapNo, 10, [int]$fuel, $throttle, $brake,
        $tc, $abs, 0
    ) + $tyre
    $line = ($fields -join ";")

    if ($Bad -and ($seq % 7 -eq 0)) { $line = "DSH1;$seq;1;GARBAGE" }   # 故意送壞的
    $sp.WriteLine($line)
    if ($seq % ($Hz * 5) -eq 1) { Write-Host $line }

    Start-Sleep -Milliseconds ([int](1000 / $Hz))
}

$sp.Close()
Write-Host "結束，共送 $seq 行"
