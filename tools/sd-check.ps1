<#
.SYNOPSIS
    快速判斷一張 SD 卡是好是壞。

.DESCRIPTION
    這個腳本的重點只有一句話：**寫入量要夠**。

    壞掉的卡在小寫入下完全正常 —— 刪個檔、建個資料夾、跑 chkdsk 都看不出來，
    讀取也可能完美無缺。實際踩過的那張 8GB：PC 上讀 4493 個檔案 3.0GB 零錯誤、
    23.4 MB/s，但連續寫入 188 秒（1.41GB）之後**整個從 USB 匯流排消失**，
    要拔插斷電才會回來。

    所以「在電腦上用起來沒事」不能當成卡片健康的證據 —— 除非你真的寫夠多。

.PARAMETER DriveLetter
    要測的磁碟機代號，例如 F。

.PARAMETER SizeMB
    寫入量，預設 2048（2GB）。那張壞卡死在 1.41GB，所以 2GB 是能抓到它的最小值。
    想更保險就調大。

.PARAMETER FillAll
    改成用光所有可用空間。順便驗容量真偽（假卡會在超過真實容量後開始對不上）。

.EXAMPLE
    .\sd-check.ps1 -DriveLetter F
    .\sd-check.ps1 -DriveLetter F -FillAll

.NOTES
    - **不會動到既有資料**，只在可用空間裡建一個暫存檔，測完刪掉
    - 不需要系統管理員權限
    - 詳細背景見 docs/board-notes.md 第二十章
#>
param(
    [Parameter(Mandatory = $true)][char]$DriveLetter,
    [int]$SizeMB = 2048,
    [switch]$FillAll
)

$ErrorActionPreference = 'Stop'
$blk  = 4MB
$fail = @()

function Vol { Get-Volume -DriveLetter $DriveLetter -ErrorAction SilentlyContinue }

$v = Vol
if (-not $v) {
    Write-Host ""
    Write-Host "找不到 ${DriveLetter}: 磁碟區。" -ForegroundColor Red
    Write-Host "如果它剛才還在，那本身就是壞卡的徵兆 —— 拔插斷電看看會不會回來。" -ForegroundColor Yellow
    exit 1
}

# 路徑用字串組，不要用 Join-Path：磁碟機不存在時 Join-Path 會直接丟例外，
# 那樣就跑不到上面那個友善訊息了。而「磁碟機突然不見」正是壞卡的典型症狀，
# 所以這條路徑一定要走得到。
$path = "$DriveLetter" + ":\_sdcheck.tmp"

Write-Host ""
Write-Host ("磁碟區 {0}:  {1}  容量 {2:N2} GB  可用 {3:N2} GB" -f `
    $DriveLetter, $v.FileSystem, ($v.Size/1GB), ($v.SizeRemaining/1GB)) -ForegroundColor Cyan

$bytes = if ($FillAll) { $v.SizeRemaining - 100MB } else { [int64]$SizeMB * 1MB }
if ($bytes -gt ($v.SizeRemaining - 100MB)) { $bytes = $v.SizeRemaining - 100MB }
$n = [int]($bytes / $blk)
if ($n -lt 1) { Write-Host "可用空間不足" -ForegroundColor Red; exit 1 }

Write-Host ("測試量 {0:N2} GB（{1} 個 4MB 區塊）" -f ($n*$blk/1GB), $n)

# 固定種子，驗證時重建同樣的內容；每塊開頭蓋上編號，抓得到「寫到別的地方去」
$buf = New-Object byte[] $blk
(New-Object Random 20260823).NextBytes($buf)

Write-Host ""
Write-Host "[1/3] 寫入..." -ForegroundColor Cyan
$sw = [Diagnostics.Stopwatch]::StartNew()
$wrote = 0L
try {
    # WriteThrough：確保真的落到裝置，不是停在作業系統的快取裡
    $fs = New-Object IO.FileStream($path, [IO.FileMode]::Create, [IO.FileAccess]::Write,
                                   [IO.FileShare]::None, $blk, [IO.FileOptions]::WriteThrough)
    for ($i = 0; $i -lt $n; $i++) {
        [BitConverter]::GetBytes([int]$i).CopyTo($buf, 0)
        $fs.Write($buf, 0, $blk)
        $wrote += $blk
        if ($i % 64 -eq 0) { Write-Host ("`r   {0:N2} / {1:N2} GB" -f ($wrote/1GB), ($n*$blk/1GB)) -NoNewline }
    }
    $fs.Flush(); $fs.Close()
} catch {
    $fail += "寫入在 {0:N2} GB 處失敗：{1}" -f ($wrote/1GB), $_.Exception.Message
    try { $fs.Close() } catch { }
}
$sw.Stop()
$wsec = $sw.Elapsed.TotalSeconds
Write-Host ""
Write-Host ("   寫入 {0:N2} GB，{1:N1} 秒，{2:N1} MB/s" -f ($wrote/1GB), $wsec, ($wrote/1MB/[Math]::Max($wsec,0.001)))

Write-Host "[2/3] 檢查裝置還在不在..." -ForegroundColor Cyan
if (-not (Vol)) {
    $fail += "**裝置在寫入後從系統上消失** —— 這是卡片壞掉最明確的徵兆，要拔插斷電才會回來"
    Write-Host "   裝置不見了" -ForegroundColor Red
} else {
    Write-Host "   還在"
}

if ($fail.Count -eq 0) {
    Write-Host "[3/3] 讀回來逐位元組比對..." -ForegroundColor Cyan
    $chk = New-Object byte[] $blk
    $sw2 = [Diagnostics.Stopwatch]::StartNew()
    $read = 0L
    try {
        $fs = [IO.File]::OpenRead($path)
        for ($i = 0; $i -lt $n; $i++) {
            $got = 0
            while ($got -lt $blk) {
                $k = $fs.Read($chk, $got, $blk - $got)
                if ($k -le 0) { break }
                $got += $k
            }
            if ($got -ne $blk) { $fail += "第 $i 塊讀不完整"; break }
            [BitConverter]::GetBytes([int]$i).CopyTo($buf, 0)
            if ([Linq.Enumerable]::SequenceEqual([byte[]]$chk, [byte[]]$buf) -eq $false) {
                $fail += "第 $i 塊內容對不上（位移 {0:N2} GB）—— 可能是假容量卡或壞區塊" -f ($i*$blk/1GB)
                break
            }
            $read += $blk
        }
        $fs.Close()
    } catch {
        $fail += "讀取在 {0:N2} GB 處失敗：{1}" -f ($read/1GB), $_.Exception.Message
        try { $fs.Close() } catch { }
    }
    $sw2.Stop()
    Write-Host ("   讀取 {0:N2} GB，{1:N1} 秒，{2:N1} MB/s" -f ($read/1GB), $sw2.Elapsed.TotalSeconds, ($read/1MB/[Math]::Max($sw2.Elapsed.TotalSeconds,0.001)))
} else {
    Write-Host "[3/3] 略過讀取比對（前面已經失敗）" -ForegroundColor Yellow
}

Remove-Item $path -Force -ErrorAction SilentlyContinue

Write-Host ""
if ($fail.Count -eq 0) {
    Write-Host "===== 通過：這張卡的寫入是可靠的 =====" -ForegroundColor Green
    Write-Host ("寫入 {0:N1} MB/s。想更嚴格就加 -FillAll 用光整張卡（順便驗容量真偽）。" -f ($wrote/1MB/[Math]::Max($wsec,0.001)))
    exit 0
} else {
    Write-Host "===== 失敗：這張卡不要拿來存重要的東西 =====" -ForegroundColor Red
    $fail | ForEach-Object { Write-Host ("  - " + $_) -ForegroundColor Red }
    Write-Host ""
    Write-Host "注意：卡片可能仍然「讀得很正常」。讀取沒問題不代表寫入沒問題。" -ForegroundColor Yellow
    exit 1
}
