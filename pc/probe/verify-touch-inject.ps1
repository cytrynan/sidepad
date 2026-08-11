# pc/probe/verify-touch-inject.ps1
#
# タッチ→PC返送のローカル機械検証（iPad実機なし。design.md §3-4、
# レビュー2026-08-11 判定器修正 J1-J4 反映済み）。
#
# 手順:
#   0) [J4] pc_encode_spike --touch-selftest を先に実行し、プロトコル/純関数
#      (ParseTouchMsg・MapToVirtualDesktop・VerticalWheelDelta・
#      HorizontalWheelDelta) の自己診断がPASSすることを確認する。
#      これはSendInputを一切呼ばないので無人で回してよい。FAILなら
#      ここで打ち切り、マウスは一切動かさない。
#   1) mock_receiver.py --send-touch-script を PC 役として起動する。
#   2) pc_encode_spike --send tcp:127.0.0.1:PORT --seconds N をそれに向けて
#      走らせる。mock_receiver は接続後、四隅move→中央tap→scroll
#      (TOUCH_SCRIPT、mock_receiver.py と同じ座標をこのファイルで定数として
#      持つ = 実装の式を写さず独立に判定する[judge-design.mdの教訓])を
#      TOUCH_SCRIPT_INTERVAL_SEC間隔で送る。
#   3) [J2] 固定タイミングでの単発サンプリングをやめ、実行中ずっと
#      ~20Hz で GetCursorPos を採取して軌跡を作る。判定は「期待5点
#      (四隅+中央)に、送信順を保ったまま軌跡上で±2px以内に到達したか」を
#      軌跡から後追いで判定する(タイミング結合を排除)。
#   4) [J1] pc_encode_spike の終了時ログ "touch: moves=.. taps=.. scrolls=..
#      inject_errors=.." を TOUCH_SCRIPT の送信数(move=4/tap=1/scroll=1)と
#      突き合わせる。
#
# 【重要】手順1)以降はKのマウスカーソルが動く。
#   絶対に自動実行しない。走らせる前に必ずチャットで一言断ってから、
#   Kの許可を得たうえで手動実行すること（司令塔の指示書 検証3節）。
#   走行時間は既定15秒に抑えてある。手順0)のみは単独で無人実行してよい
#   (SendInputを呼ばないため)。
#
# 使い方（許可を得てから）:
#   pwsh -File pc\probe\verify-touch-inject.ps1
#   pwsh -File pc\probe\verify-touch-inject.ps1 -Port 7790 -Seconds 15
#   pwsh -File pc\probe\verify-touch-inject.ps1 -SelfTestOnly   # 手順0)のみ、無人実行可
#
# 前提: pwsh -File pc\build.ps1 で pc_encode_spike.exe がビルド済みであること。

param(
    [int]$Port = 7789,
    [int]$Seconds = 15,
    [string]$ExePath = "",
    [switch]$SelfTestOnly
)

$ErrorActionPreference = "Stop"

$RepoPc = Split-Path -Parent $PSScriptRoot
if (-not $ExePath) {
    $ExePath = Join-Path $RepoPc "build\pc_encode_spike.exe"
}
if (-not (Test-Path $ExePath)) {
    throw "pc_encode_spike.exe not found at '$ExePath'. Run pc\build.ps1 first."
}

# --- P/Invoke: GetCursorPos / GetSystemMetrics / DPI awareness --------------
Add-Type @"
using System;
using System.Runtime.InteropServices;
public struct POINT { public int X; public int Y; }
public static class Native {
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int n);
    [DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr dpiContext);
}
"@

# [J3] このpwshプロセス自体がDPI非対応のままだとGetCursorPos/GetSystemMetrics
# がスケーリングされた座標系(150%等)を返し、期待値計算が全部ズレて偽FAILになる。
# DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4 (winuser.h)。
# pc_encode_spike / sidepadd 側もSetProcessDpiAwarenessContextで同じ値を使っている
# (daemon_main.cpp / encode_main.cpp)ので、判定側もここで揃える。
$dpiResult = [Native]::SetProcessDpiAwarenessContext([IntPtr](-4))
if ($dpiResult -eq [IntPtr]::Zero) {
    Write-Warning "SetProcessDpiAwarenessContext failed (already set by host? continuing)"
} else {
    Write-Host "DPI awareness: PER_MONITOR_AWARE_V2 set for this process"
}

# --- [J4] touch self-test first: SendInputを一切呼ばない診断 ----------------
Write-Host "`n=== step 0: pc_encode_spike --touch-selftest (no SendInput calls) ==="
$selftestOutput = & $ExePath --touch-selftest 2>&1
$selftestExit = $LASTEXITCODE
$selftestOutput | ForEach-Object { Write-Host $_ }
if ($selftestExit -ne 0) {
    Write-Host "`n[FAIL] --touch-selftest exited $selftestExit -- aborting BEFORE touching the mouse."
    exit 2
}
Write-Host "`n[PASS] --touch-selftest exit 0"

if ($SelfTestOnly) {
    Write-Host "`n-SelfTestOnly: stopping here (no mouse movement)."
    exit 0
}

Write-Host "`n=== WARNING: the following steps move the mouse cursor. Confirm with K before running unattended. ==="

# --- 期待値の独立計算 -------------------------------------------------------
# mock_receiver.py の TOUCH_SCRIPT と同じ座標を「値として」ここに複製する
# (実装コードは一切参照しない)。式は design.md §3-1 の二段変換をこのスクリプト
# 側で書き下したもので、input_injector.cpp の MapToVirtualDesktop 実装とは
# 独立に計算する。送信順を保った軌跡照合の対象は四隅+中央の5点
# (tap/scrollはどちらも中央が目標なので、中央は1つの目標として扱う)。
$Targets = @(
    @{ Name = "top-left";     X = 0;     Y = 0     }
    @{ Name = "top-right";    X = 65535; Y = 0     }
    @{ Name = "bottom-left";  X = 0;     Y = 65535 }
    @{ Name = "bottom-right"; X = 65535; Y = 65535 }
    @{ Name = "center";       X = 32767; Y = 32767 }
)
# TOUCH_SCRIPT の送信数（moves/taps/scrolls）。[J1] の照合対象。
$ExpectedMoves = 4
$ExpectedTaps = 1
$ExpectedScrolls = 1

function Get-Display0Rect {
    # DXGIを直接叩く代わりに、pc_encode_spike --list-displays の出力の
    # "output 0" 行から矩形を読む(判定器を実装から独立させつつ、既存の
    # --list-displays出力という別経路を根拠にする)。
    $lines = & $ExePath --list-displays
    foreach ($line in $lines) {
        if ($line -match "output 0: \S+ attached=1 (\d+)x(\d+) @\((-?\d+),(-?\d+)\)") {
            $w = [int]$Matches[1]; $h = [int]$Matches[2]
            $x = [int]$Matches[3]; $y = [int]$Matches[4]
            return @{ Left = $x; Top = $y; Right = $x + $w; Bottom = $y + $h }
        }
    }
    throw "could not find 'output 0' in --list-displays output"
}

function Expected-CursorPos([hashtable]$target, [int]$x, [int]$y) {
    # design.md §3-1 の式をそのまま独立に書き下す(実装コードは参照しない)。
    $tw = $target.Right - $target.Left
    $th = $target.Bottom - $target.Top
    $px = $target.Left + ($x / 65535.0) * ($tw - 1)
    $py = $target.Top + ($y / 65535.0) * ($th - 1)
    # PC上のSendInputはABSOLUTE|VIRTUALDESKで一度0..65535に丸めてから元へ
    # マップし直すため、往復誤差(丸め)が乗る。px,pyがそのままGetCursorPosの
    # 期待値になる(仮想スクリーン全体への丸め往復が主な誤差源、許容±2px)。
    return @{ X = [math]::Round($px); Y = [math]::Round($py) }
}

$target = Get-Display0Rect
Write-Host "target rect (output 0): left=$($target.Left) top=$($target.Top) right=$($target.Right) bottom=$($target.Bottom)"

# --- 実行 --------------------------------------------------------------------

Write-Host "`n=== step 1: mock_receiver.py --send-touch-script on 127.0.0.1:$Port ==="
$mockStdout = Join-Path $env:TEMP "mock_receiver_stdout.log"
$mock = Start-Process -FilePath "python" `
    -ArgumentList @((Join-Path $PSScriptRoot "mock_receiver.py"), "--port", "$Port",
                    "--send-touch-script") `
    -PassThru -NoNewWindow -RedirectStandardOutput $mockStdout
Start-Sleep -Seconds 1

Write-Host "=== step 2: pc_encode_spike --send tcp:127.0.0.1:$Port --seconds $Seconds ==="
$spikeStdout = Join-Path $env:TEMP "pc_encode_spike_stdout.log"
$spike = Start-Process -FilePath $ExePath `
    -ArgumentList @("--send", "tcp:127.0.0.1:$Port", "--seconds", "$Seconds") `
    -PassThru -NoNewWindow -RedirectStandardOutput $spikeStdout

# --- [J2] 軌跡ベース判定: ~20Hz でGetCursorPosを採取し続ける ----------------
Write-Host "=== step 3: sampling GetCursorPos at ~20Hz for $($Seconds + 3)s ==="
$samples = New-Object System.Collections.Generic.List[object]
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$sampleUntilMs = ($Seconds + 3) * 1000
while ($sw.ElapsedMilliseconds -lt $sampleUntilMs) {
    $p = New-Object POINT
    [Native]::GetCursorPos([ref]$p) | Out-Null
    $samples.Add([pscustomobject]@{ ElapsedMs = $sw.ElapsedMilliseconds; X = $p.X; Y = $p.Y })
    Start-Sleep -Milliseconds 50
}
Write-Host "collected $($samples.Count) cursor samples"

Wait-Process -Id $spike.Id -Timeout 10 -ErrorAction SilentlyContinue
if (-not $spike.HasExited) { Stop-Process -Id $spike.Id -Force }
if (-not $mock.HasExited) { Stop-Process -Id $mock.Id -Force }
Start-Sleep -Milliseconds 300  # let RedirectStandardOutput flush to disk

# 送信順を保ったまま、各目標に±2px以内で到達したサンプルを前方探索で探す。
# 見つかった時点で探索開始位置を進める = 「順序どおりに到達したか」を
# タイミングに一切依存せず軌跡だけで判定する([J2])。
$searchFrom = 0
$targetResults = @()
foreach ($t in $Targets) {
    $exp = Expected-CursorPos -target $target -x $t.X -y $t.Y
    $foundIdx = -1
    for ($i = $searchFrom; $i -lt $samples.Count; $i++) {
        $s = $samples[$i]
        $dist = [math]::Sqrt([math]::Pow($s.X - $exp.X, 2) + [math]::Pow($s.Y - $exp.Y, 2))
        if ($dist -le 2.0) { $foundIdx = $i; break }
    }
    if ($foundIdx -ge 0) {
        $targetResults += [pscustomobject]@{
            Name = $t.Name; Found = $true; AtMs = $samples[$foundIdx].ElapsedMs
            ExpectedX = $exp.X; ExpectedY = $exp.Y
            ActualX = $samples[$foundIdx].X; ActualY = $samples[$foundIdx].Y
        }
        $searchFrom = $foundIdx + 1
    } else {
        $targetResults += [pscustomobject]@{
            Name = $t.Name; Found = $false; AtMs = $null
            ExpectedX = $exp.X; ExpectedY = $exp.Y; ActualX = $null; ActualY = $null
        }
    }
}

Write-Host "`n=== trajectory-based target check (order-preserving, timing-independent) ==="
$targetResults | Format-Table -AutoSize

# --- [J1] 注入カウント照合 ----------------------------------------------------
$spikeLog = if (Test-Path $spikeStdout) { Get-Content $spikeStdout -Raw } else { "" }
$countsOk = $false
$actualMoves = $null; $actualTaps = $null; $actualScrolls = $null; $actualErrors = $null
if ($spikeLog -match "touch: moves=(\d+) taps=(\d+) scrolls=(\d+) inject_errors=(\d+)") {
    $actualMoves = [int]$Matches[1]; $actualTaps = [int]$Matches[2]
    $actualScrolls = [int]$Matches[3]; $actualErrors = [int]$Matches[4]
    $countsOk = ($actualMoves -eq $ExpectedMoves) -and ($actualTaps -eq $ExpectedTaps) -and
                ($actualScrolls -eq $ExpectedScrolls) -and ($actualErrors -eq 0)
    Write-Host "`n=== injection count check ==="
    Write-Host "expected: moves=$ExpectedMoves taps=$ExpectedTaps scrolls=$ExpectedScrolls inject_errors=0"
    Write-Host "actual:   moves=$actualMoves taps=$actualTaps scrolls=$actualScrolls inject_errors=$actualErrors"
} else {
    Write-Host "`n=== injection count check ==="
    Write-Host "[FAIL] could not find 'touch: moves=... taps=... scrolls=... inject_errors=...' in pc_encode_spike output ($spikeStdout)"
}

$allTargetsFound = ($targetResults | Where-Object { -not $_.Found }).Count -eq 0
$allPass = $allTargetsFound -and $countsOk

if ($allPass) {
    Write-Host "`n[PASS] all $($Targets.Count) targets reached in order (+/-2px) AND injection counts match"
    exit 0
} else {
    if (-not $allTargetsFound) { Write-Host "`n[FAIL] one or more targets not reached in order within +/-2px" }
    if (-not $countsOk) { Write-Host "[FAIL] injection counts did not match TOUCH_SCRIPT send counts" }
    exit 2
}
