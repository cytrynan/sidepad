# pc/probe/verify-touch-inject.ps1
#
# タッチ→PC返送のローカル機械検証（iPad実機なし。design.md §3-4）。
#
#   mock_receiver.py --send-touch-script  を PC 役として起動し、
#   pc_encode_spike --send tcp:127.0.0.1:PORT --seconds N をそれに向けて走らせる。
#   mock_receiver は接続後、四隅move→中央tap→scroll(TOUCH_SCRIPT, mock_receiver.py
#   と同じ座標をこのファイルで定数として持つ = 実装の式を写さず独立に判定する
#   [judge-design.md の教訓]) を1秒間隔で送る。pc_encode_spike のプロセス内で
#   Pipeline -> StreamSender -> InputInjector が実際に SendInput を発火するので、
#   各moveの直後に GetCursorPos(P/Invoke) を読み、期待座標(このファイルが独立
#   計算)と ±2px で一致するかを判定する。
#
# 【重要】このスクリプトは実行するとKのマウスカーソルが動く。
#   絶対に自動実行しない。走らせる前に必ずチャットで一言断ってから、
#   Kの許可を得たうえで手動実行すること（司令塔の指示書 検証3節）。
#   走行時間は既定15秒に抑えてある。
#
# 使い方（許可を得てから）:
#   pwsh -File pc\probe\verify-touch-inject.ps1
#   pwsh -File pc\probe\verify-touch-inject.ps1 -Port 7790 -Seconds 15
#
# 前提: pwsh -File pc\build.ps1 で pc_encode_spike.exe がビルド済みであること。

param(
    [int]$Port = 7789,
    [int]$Seconds = 15,
    [string]$ExePath = ""
)

$ErrorActionPreference = "Stop"

$RepoPc = Split-Path -Parent $PSScriptRoot
if (-not $ExePath) {
    $ExePath = Join-Path $RepoPc "build\pc_encode_spike.exe"
}
if (-not (Test-Path $ExePath)) {
    throw "pc_encode_spike.exe not found at '$ExePath'. Run pc\build.ps1 first."
}

# --- 期待値の独立計算 -------------------------------------------------------
# mock_receiver.py の TOUCH_SCRIPT と同じ座標を「値として」ここに複製する
# (実装コードは一切参照しない)。式は design.md §3-1 の二段変換をこのスクリプト
# 側で書き下したもので、input_injector.cpp の MapToVirtualDesktop 実装とは
# 独立に計算する。
$TouchScript = @(
    @{ Kind = 1; X = 0;     Y = 0;     Dx = 0;   Dy = 0   }  # move: top-left
    @{ Kind = 1; X = 65535; Y = 0;     Dx = 0;   Dy = 0   }  # move: top-right
    @{ Kind = 1; X = 0;     Y = 65535; Dx = 0;   Dy = 0   }  # move: bottom-left
    @{ Kind = 1; X = 65535; Y = 65535; Dx = 0;   Dy = 0   }  # move: bottom-right
    @{ Kind = 2; X = 32767; Y = 32767; Dx = 0;   Dy = 0   }  # tap: center
    @{ Kind = 3; X = 32767; Y = 32767; Dx = -10; Dy = 300 }  # scroll
)

# GetCursorPos / GetSystemMetrics via P/Invoke.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public struct POINT { public int X; public int Y; }
public static class Native {
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int n);
}
"@

# SM_XVIRTUALSCREEN=76, SM_YVIRTUALSCREEN=77, SM_CXVIRTUALSCREEN=78, SM_CYVIRTUALSCREEN=79
$vLeft = [Native]::GetSystemMetrics(76)
$vTop = [Native]::GetSystemMetrics(77)
$vWidth = [Native]::GetSystemMetrics(78)
$vHeight = [Native]::GetSystemMetrics(79)
Write-Host "virtual screen: left=$vLeft top=$vTop width=$vWidth height=$vHeight"

# ターゲット矩形（VDDのDesktopCoordinates）は pc_encode_spike --list-displays
# のNATIVE MODE判定と同じ幾何。ここではPC上の唯一のプライマリ以外の出力を
# 使わず、mock_receiver は "tcp" 経路なので実ディスプレイのVDD矩形をそのまま
# 使うのが正しい対応（sidepaddと同じ InitCapture -> SetTargetRect 経路）。
# --display の対象出力は既定(0)固定なので、その出力のDesktopCoordinatesを
# 独立に取得する。
Add-Type @"
using System;
using System.Runtime.InteropServices;
public struct RECT { public int Left, Top, Right, Bottom; }
"@

function Get-Display0Rect {
    # DXGIを直接叩く代わりに、pc_encode_spike --list-displays の出力の
    # "output 0" 行から矩形を読む(判定器を実装から独立させつつ、既存の
    # ---list-displays出力という別経路を根拠にする)。
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

# --- 実行 --------------------------------------------------------------------

Write-Host "starting mock_receiver.py --send-touch-script on 127.0.0.1:$Port"
$mock = Start-Process -FilePath "python" `
    -ArgumentList @((Join-Path $PSScriptRoot "mock_receiver.py"), "--port", "$Port",
                    "--send-touch-script") `
    -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $env:TEMP "mock_receiver_stdout.log")
Start-Sleep -Seconds 1

$target = Get-Display0Rect
Write-Host "target rect (output 0): left=$($target.Left) top=$($target.Top) right=$($target.Right) bottom=$($target.Bottom)"

Write-Host "WARNING: this moves the mouse cursor. Confirm with K before running unattended."
Write-Host "starting pc_encode_spike --send tcp:127.0.0.1:$Port --seconds $Seconds"
$spike = Start-Process -FilePath $ExePath `
    -ArgumentList @("--send", "tcp:127.0.0.1:$Port", "--seconds", "$Seconds") `
    -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $env:TEMP "pc_encode_spike_stdout.log")

# TOUCH_SCRIPTは1秒間隔・全6件なので、各move直後(概ね1.2秒後)にサンプルする。
$results = @()
for ($i = 0; $i -lt $TouchScript.Count; $i++) {
    Start-Sleep -Milliseconds 1200
    $msg = $TouchScript[$i]
    $p = New-Object POINT
    [Native]::GetCursorPos([ref]$p) | Out-Null
    if ($msg.Kind -eq 1 -or $msg.Kind -eq 2) {
        $exp = Expected-CursorPos -target $target -x $msg.X -y $msg.Y
        $dist = [math]::Sqrt([math]::Pow($p.X - $exp.X, 2) + [math]::Pow($p.Y - $exp.Y, 2))
        $pass = $dist -le 2.0
        $results += [pscustomobject]@{
            Index = $i; Kind = $msg.Kind; ExpectedX = $exp.X; ExpectedY = $exp.Y
            ActualX = $p.X; ActualY = $p.Y; DistancePx = [math]::Round($dist, 2); Pass = $pass
        }
    } else {
        # scroll: カーソル位置は中央のままのはず(移動+ホイールのみ、座標判定は
        # move/tapと同様center基準で行う)。
        $exp = Expected-CursorPos -target $target -x $msg.X -y $msg.Y
        $dist = [math]::Sqrt([math]::Pow($p.X - $exp.X, 2) + [math]::Pow($p.Y - $exp.Y, 2))
        $pass = $dist -le 2.0
        $results += [pscustomobject]@{
            Index = $i; Kind = $msg.Kind; ExpectedX = $exp.X; ExpectedY = $exp.Y
            ActualX = $p.X; ActualY = $p.Y; DistancePx = [math]::Round($dist, 2); Pass = $pass
        }
    }
}

Wait-Process -Id $spike.Id -Timeout ($Seconds + 10) -ErrorAction SilentlyContinue
if (-not $spike.HasExited) { Stop-Process -Id $spike.Id -Force }
if (-not $mock.HasExited) { Stop-Process -Id $mock.Id -Force }

Write-Host "`n=== per-message cursor check ==="
$results | Format-Table -AutoSize

# 送信数とPC側のカウント一致は pc_encode_spike の標準出力に
# device report 等は出ないため、代わりに sidepadd/pc_encode_spike の
# stats経路(将来ここにCSV/ログの読み取りを追加できる余地を残す)。
# 現状は「四隅move x4 + tap x1 + scroll x1」の座標一致のみで判定する。
$allPass = ($results | Where-Object { -not $_.Pass }).Count -eq 0
if ($allPass) {
    Write-Host "`n[PASS] all $($results.Count) touch messages landed within 2px of the independently-computed expected position"
    exit 0
} else {
    Write-Host "`n[FAIL] one or more touch messages missed the expected position by more than 2px"
    exit 2
}
