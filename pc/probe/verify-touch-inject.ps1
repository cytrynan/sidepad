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
#      走らせる。mock_receiver は接続後、四隅move→中央tap→scroll→
#      長押しドラッグ(dragBegin→move×2→dragEnd、2026-08-11 K承認の追加)
#      (TOUCH_SCRIPT、mock_receiver.py と同じ座標をこのファイルで定数として
#      持つ = 実装の式を写さず独立に判定する[judge-design.mdの教訓])を
#      TOUCH_SCRIPT_INTERVAL_SEC間隔で送る。
#   3) [J2] 固定タイミングでの単発サンプリングをやめ、実行中ずっと
#      ~20Hz で GetCursorPos(+GetAsyncKeyStateで左ボタン状態も) を採取して
#      軌跡を作る。判定は「期待9点(四隅+中央+drag begin/move×2/end)に、
#      送信順を保ったまま軌跡上で±2px以内に到達したか」を軌跡から後追いで
#      判定する(タイミング結合を排除)。
#   4) [J1] pc_encode_spike の終了時ログ "touch: moves=.. taps=.. scrolls=..
#      drags=.. inject_errors=.." を TOUCH_SCRIPT の送信数
#      (move=6/tap=1/scroll=1/drag=1)と突き合わせる。
#   5) [判定器補強1] drag-begin到達〜drag-end到達の間のサンプルで左ボタンが
#      DOWNだったこと、走行終了時にUPであることを GetAsyncKeyState(VK_LBUTTON)
#      で直接確認する（「押しっぱなし事故」そのものを実測。座標一致だけでは
#      ボタン状態は測れていなかった、というレビュー指摘への対応）。
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
    [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vKey);
}
"@
# VK_LBUTTON = 0x01 (winuser.h). GetAsyncKeyState's high bit (0x8000) means
# "down right now" -- [判定器補強1] this is the DIRECT measurement of the
# "left button stuck down" failure mode the drag fail-safe (必須1-3) exists
# to prevent; coordinate-only checks below cannot see button state at all.
$VK_LBUTTON = 0x01

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
# 独立に計算する。送信順を保った軌跡照合の対象は四隅+中央+drag begin/move×2/end
# の9点(tap/scrollはどちらも中央が目標なので、中央は1つの目標として扱う。
# drag関連の座標はmock_receiver.pyのTOUCH_SCRIPTと同じ座標をここに複製)。
$Targets = @(
    @{ Name = "top-left";     X = 0;     Y = 0     }
    @{ Name = "top-right";    X = 65535; Y = 0     }
    @{ Name = "bottom-left";  X = 0;     Y = 65535 }
    @{ Name = "bottom-right"; X = 65535; Y = 65535 }
    @{ Name = "center";       X = 32767; Y = 32767 }
    @{ Name = "drag-begin";   X = 16384; Y = 16384 }
    @{ Name = "drag-move-1";  X = 27307; Y = 16384 }
    @{ Name = "drag-move-2";  X = 38229; Y = 27307 }
    @{ Name = "drag-end";     X = 49151; Y = 49151 }
)
# TOUCH_SCRIPT の送信数（moves/taps/scrolls/drags）。[J1] の照合対象。
# movesは四隅4 + drag中の移動2 = 6（判定器補強2: dragBegin~dragEnd間に
# kind=1を2発挟んだ「掴んだまま動く」の実測を反映）。
$ExpectedMoves = 6
$ExpectedTaps = 1
$ExpectedScrolls = 1
$ExpectedDrags = 1

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

# --- [J2] 軌跡ベース判定: ~20Hz でGetCursorPos+左ボタン状態を採取し続ける ---
Write-Host "=== step 3: sampling GetCursorPos + VK_LBUTTON state at ~20Hz for $($Seconds + 3)s ==="
$samples = New-Object System.Collections.Generic.List[object]
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$sampleUntilMs = ($Seconds + 3) * 1000
while ($sw.ElapsedMilliseconds -lt $sampleUntilMs) {
    $p = New-Object POINT
    [Native]::GetCursorPos([ref]$p) | Out-Null
    # [判定器補強1] 高位ビット(0x8000)が立っていれば「今まさに押されている」。
    $btnDown = (([Native]::GetAsyncKeyState($VK_LBUTTON)) -band 0x8000) -ne 0
    $samples.Add([pscustomobject]@{
        ElapsedMs = $sw.ElapsedMilliseconds; X = $p.X; Y = $p.Y; ButtonDown = $btnDown
    })
    Start-Sleep -Milliseconds 50
}
Write-Host "collected $($samples.Count) cursor samples"
# サンプラ自身の瞬き検出: 最大採取間隔が送信間隔の半分を超えていたら、
# ターゲット未到達FAILは「カーソルが行かなかった」ではなく「サンプラが
# 見ていなかった」可能性がある（2026-08-11 実走1回目: PSの採取が~1.5s
# 途切れ、右下だけ偽FAILした実例）。判定器は自分の盲目時間を申告する。
$maxGapMs = 0
for ($i = 1; $i -lt $samples.Count; $i++) {
    $gap = $samples[$i].ElapsedMs - $samples[$i - 1].ElapsedMs
    if ($gap -gt $maxGapMs) { $maxGapMs = $gap }
}
Write-Host ("max sample gap: {0}ms {1}" -f $maxGapMs, `
    ($maxGapMs -gt 750 ? '** WARNING: sampler blind spot exceeds half the send interval -- a target miss below may be a sampler blink, re-run before trusting a FAIL **' : '(ok)'))

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
            SampleIndex = $foundIdx
            ExpectedX = $exp.X; ExpectedY = $exp.Y
            ActualX = $samples[$foundIdx].X; ActualY = $samples[$foundIdx].Y
        }
        $searchFrom = $foundIdx + 1
    } else {
        $targetResults += [pscustomobject]@{
            Name = $t.Name; Found = $false; AtMs = $null; SampleIndex = $null
            ExpectedX = $exp.X; ExpectedY = $exp.Y; ActualX = $null; ActualY = $null
        }
    }
}

Write-Host "`n=== trajectory-based target check (order-preserving, timing-independent) ==="
$targetResults | Format-Table -AutoSize

# --- [判定器補強1] 左ボタン押しっぱなし事故そのものの直接測定 ----------------
# 座標一致だけでは「ボタンを離し忘れた」事故は原理的に見えない
# (dragEndのmoveターゲットに±2px着地しても、LEFTUPが実際に注入されたとは
# 限らない)。GetAsyncKeyStateで直接ボタン状態を見て2点をassertする:
#   1) drag-begin到達サンプル〜drag-end到達サンプルの間のどこかでDOWNが
#      観測されること（「掴んだまま動く」＝ドラッグが実際に成立した証拠）
#   2) 採取した最後のサンプルでUPであること（走行終了時に押しっぱなしで
#      ないこと。必須1-3のフェイルセーフが機能した最終確認）
$dragBeginResult = $targetResults | Where-Object { $_.Name -eq 'drag-begin' } | Select-Object -First 1
$dragEndResult = $targetResults | Where-Object { $_.Name -eq 'drag-end' } | Select-Object -First 1
$buttonWasDownDuringDrag = $false
if ($dragBeginResult.Found -and $dragEndResult.Found) {
    for ($i = $dragBeginResult.SampleIndex; $i -le $dragEndResult.SampleIndex; $i++) {
        if ($samples[$i].ButtonDown) { $buttonWasDownDuringDrag = $true; break }
    }
}
$buttonUpAtEnd = $samples.Count -gt 0 -and -not $samples[$samples.Count - 1].ButtonDown

Write-Host "`n=== VK_LBUTTON physical-state check (direct measurement of the stuck-button failure mode) ==="
if ($dragBeginResult.Found -and $dragEndResult.Found) {
    Write-Host ("button observed DOWN between drag-begin (sample {0}) and drag-end (sample {1}): {2}" `
        -f $dragBeginResult.SampleIndex, $dragEndResult.SampleIndex, $buttonWasDownDuringDrag)
} else {
    Write-Host "[FAIL] cannot check button-down-during-drag: drag-begin and/or drag-end target was not reached"
}
Write-Host "button UP at the last sampled point (no stuck button at run end): $buttonUpAtEnd"

# --- [J1] 注入カウント照合 ----------------------------------------------------
$spikeLog = if (Test-Path $spikeStdout) { Get-Content $spikeStdout -Raw } else { "" }
$countsOk = $false
$actualMoves = $null; $actualTaps = $null; $actualScrolls = $null
$actualDrags = $null; $actualErrors = $null
if ($spikeLog -match "touch: moves=(\d+) taps=(\d+) scrolls=(\d+) drags=(\d+) inject_errors=(\d+)") {
    $actualMoves = [int]$Matches[1]; $actualTaps = [int]$Matches[2]
    $actualScrolls = [int]$Matches[3]; $actualDrags = [int]$Matches[4]
    $actualErrors = [int]$Matches[5]
    $countsOk = ($actualMoves -eq $ExpectedMoves) -and ($actualTaps -eq $ExpectedTaps) -and
                ($actualScrolls -eq $ExpectedScrolls) -and ($actualDrags -eq $ExpectedDrags) -and
                ($actualErrors -eq 0)
    Write-Host "`n=== injection count check ==="
    Write-Host "expected: moves=$ExpectedMoves taps=$ExpectedTaps scrolls=$ExpectedScrolls drags=$ExpectedDrags inject_errors=0"
    Write-Host "actual:   moves=$actualMoves taps=$actualTaps scrolls=$actualScrolls drags=$actualDrags inject_errors=$actualErrors"
} else {
    Write-Host "`n=== injection count check ==="
    Write-Host "[FAIL] could not find 'touch: moves=... taps=... scrolls=... drags=... inject_errors=...' in pc_encode_spike output ($spikeStdout)"
}

$allTargetsFound = ($targetResults | Where-Object { -not $_.Found }).Count -eq 0
$buttonStateOk = $buttonWasDownDuringDrag -and $buttonUpAtEnd
$allPass = $allTargetsFound -and $countsOk -and $buttonStateOk

if ($allPass) {
    Write-Host "`n[PASS] all $($Targets.Count) targets reached in order (+/-2px) AND injection counts match AND button state correct (down during drag, up at end)"
    exit 0
} else {
    if (-not $allTargetsFound) { Write-Host "`n[FAIL] one or more targets not reached in order within +/-2px" }
    if (-not $countsOk) { Write-Host "[FAIL] injection counts did not match TOUCH_SCRIPT send counts" }
    if (-not $buttonWasDownDuringDrag) { Write-Host "[FAIL] left button was never observed DOWN during the drag (dragBegin's LEFTDOWN may not have landed)" }
    if (-not $buttonUpAtEnd) { Write-Host "[FAIL] left button STILL DOWN at the end of the run -- STUCK BUTTON, the exact failure mode 必須1-3 exist to prevent" }
    exit 2
}
