# pc/daemon/verify-replug.ps1 — 抜き差し試験の判定（受入: 20回連続で自動復帰）
#
# sidepadd.log から attach -> streaming のサイクルを抽出し、各サイクルの
# 接続→配信開始秒数と成否を集計する。物理的な抜き差しはKが行い、判定は
# このスクリプトが行う（人間の目視カウントを信用しない）。
#
# 使い方:
#   pwsh -File pc\daemon\verify-replug.ps1 -Since "2026-08-08 16:00:00" -Cycles 20
#   -Since 省略時はログ全体、-Cycles 省略時は見つかった数を報告のみ（判定なし）
#
# PASS条件: streaming到達サイクル数 >= Cycles かつ 全サイクル attach->stream <= 10s

param(
    [string]$Since = '',
    [int]$Cycles = 0,
    [string]$LogPath = "$env:LOCALAPPDATA\sidepad\sidepadd.log"
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $LogPath)) { Write-Host "NG: ログが無い: $LogPath"; exit 1 }

$sinceTime = if ($Since) { [datetime]::ParseExact($Since, 'yyyy-MM-dd HH:mm:ss', $null) } else { [datetime]::MinValue }

# ログ行: [2026-08-08 15:59:40.866] STATE streaming: ... (0.2s after attach)
$streamRx = [regex]'^\[(?<ts>[\d\-]+ [\d:\.]+)\] STATE streaming: .*\((?<sec>[\d\.]+)s after attach\)'
$attachRx = [regex]'^\[(?<ts>[\d\-]+ [\d:\.]+)\] EVENT attached:'

$attachCount = 0
$streams = @()
foreach ($line in Get-Content $LogPath) {
    $m = $streamRx.Match($line)
    if ($m.Success) {
        $ts = [datetime]::ParseExact($m.Groups['ts'].Value, 'yyyy-MM-dd HH:mm:ss.fff', $null)
        if ($ts -ge $sinceTime) { $streams += [pscustomobject]@{ Time = $ts; AttachToStreamSec = [double]$m.Groups['sec'].Value } }
        continue
    }
    $a = $attachRx.Match($line)
    if ($a.Success) {
        $ts = [datetime]::ParseExact($a.Groups['ts'].Value, 'yyyy-MM-dd HH:mm:ss.fff', $null)
        if ($ts -ge $sinceTime) { $attachCount++ }
    }
}

Write-Host "対象範囲: $(if ($Since) { $Since + ' 以降' } else { 'ログ全体' })"
Write-Host "attachイベント: $attachCount 件 / streaming到達: $($streams.Count) 件"
$i = 0
foreach ($s in $streams) {
    $i++
    $mark = if ($s.AttachToStreamSec -le 10) { 'OK ' } else { 'OVER' }
    Write-Host ("  cycle {0,2}: {1}  attach->stream {2,6:N1}s  ({3})" -f $i, $mark, $s.AttachToStreamSec, $s.Time.ToString('HH:mm:ss'))
}
if ($streams.Count -gt 0) {
    $max = ($streams | Measure-Object -Property AttachToStreamSec -Maximum).Maximum
    Write-Host ("attach->stream 最大: {0:N1}s" -f $max)
}

if ($Cycles -gt 0) {
    $under10 = @($streams | Where-Object { $_.AttachToStreamSec -le 10 }).Count
    # attach == streaming の一致も要求: 到達しなかった抜き差しを無視して
    # PASSにしない（受入は「20回**連続**で自動復帰」）
    $pass = ($streams.Count -ge $Cycles) -and ($under10 -eq $streams.Count) -and ($attachCount -eq $streams.Count)
    Write-Host ("[{0}] attach {1} 件 / streaming到達 {2} 件（要求 {3}）・全て10秒以内={4}" -f `
        ($pass ? 'PASS' : 'FAIL'), $attachCount, $streams.Count, $Cycles, ($under10 -eq $streams.Count))
    exit ($pass ? 0 : 2)
}
exit 0
