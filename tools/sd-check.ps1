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

# 讀回來比對一定要繞過作業系統的快取，否則**讀到的是 RAM 不是卡片**。
# 這個 bug 我自己踩過：一張讀取只有 23 MB/s 的卡，驗證階段卻跑出 139 MB/s ——
# 3GB 全部命中快取，等於什麼都沒驗到。假容量卡會直接騙過去。
#
# .NET 的 FileStream 沒有安全的方式開 FILE_FLAG_NO_BUFFERING（緩衝區必須
# 對齊磁區，而 byte[] 不保證），所以直接叫 Win32。
if (-not ('Native.IO' -as [type])) {
Add-Type -Namespace Native -Name IO -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
public static extern IntPtr CreateFileW(string p, uint access, uint share, IntPtr sec,
                                        uint disp, uint flags, IntPtr tmpl);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern bool ReadFile(IntPtr h, IntPtr buf, uint n, out uint got, IntPtr ov);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern bool CloseHandle(IntPtr h);
'@
}
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
    Write-Host "[3/3] 讀回來逐位元組比對（繞過快取）..." -ForegroundColor Cyan

    # PowerShell 會把 0x80000000 當成有號 Int32（=-2147483648），轉不成 UInt32，
    # 所以直接寫十進位的無號值。
    $GENERIC_READ           = [uint32]2147483648   # 0x80000000
    $FILE_SHARE_READ        = [uint32]1
    $OPEN_EXISTING          = [uint32]3
    $FILE_FLAG_NO_BUFFERING = [uint32]536870912    # 0x20000000

    # 緩衝區要對齊磁區，所以多配一頁再自己對齊到 4096
    $raw = [Runtime.InteropServices.Marshal]::AllocHGlobal($blk + 4096)
    $al  = [IntPtr]((([int64]$raw) + 4095) -band (-4096))
    $chk = New-Object byte[] $blk
    $h   = [Native.IO]::CreateFileW($path, $GENERIC_READ, $FILE_SHARE_READ,
                                    [IntPtr]::Zero, $OPEN_EXISTING,
                                    $FILE_FLAG_NO_BUFFERING, [IntPtr]::Zero)
    if ($h -eq [IntPtr](-1)) {
        $fail += "無法以不快取模式開檔（Win32 錯誤 {0}）" -f [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    } else {
        $sw2 = [Diagnostics.Stopwatch]::StartNew()
        $read = 0L
        for ($i = 0; $i -lt $n; $i++) {
            $got = 0
            if (-not [Native.IO]::ReadFile($h, $al, [uint32]$blk, [ref]$got, [IntPtr]::Zero)) {
                $fail += "讀取在 {0:N2} GB 處失敗（Win32 錯誤 {1}）" -f ($read/1GB), [Runtime.InteropServices.Marshal]::GetLastWin32Error()
                break
            }
            if ($got -ne $blk) { $fail += "第 $i 塊讀不完整"; break }
            [Runtime.InteropServices.Marshal]::Copy($al, $chk, 0, $blk)
            [BitConverter]::GetBytes([int]$i).CopyTo($buf, 0)
            if (-not [Linq.Enumerable]::SequenceEqual([byte[]]$chk, [byte[]]$buf)) {
                $fail += "第 {0} 塊內容對不上（位移 {1:N2} GB）—— 可能是假容量卡或壞區塊" -f $i, ($i*$blk/1GB)
                break
            }
            $read += $blk
        }
        [void][Native.IO]::CloseHandle($h)
        $sw2.Stop()
        $rsec = [Math]::Max($sw2.Elapsed.TotalSeconds, 0.001)
        Write-Host ("   讀取 {0:N2} GB，{1:N1} 秒，{2:N1} MB/s（未經快取，這才是卡片的真實速度）" -f `
            ($read/1GB), $sw2.Elapsed.TotalSeconds, ($read/1MB/$rsec))
    }
    [Runtime.InteropServices.Marshal]::FreeHGlobal($raw)
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
