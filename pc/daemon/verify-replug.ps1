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
#           かつ 各サイクルの初回セッションが MinStreamSec 以上継続
#           （streaming到達直後に死んで再試行ループする状態をPASSにしない。
#             レビュー2026-08-11 B-4。抜くまで挿しっぱなしにする試験手順が前提。
#             判定時点でまだ配信中＝teardown行なしはOK扱い）

param(
    [string]$Since = '',
    [int]$Cycles = 0,
    [double]$MinStreamSec = 3,
    [string]$LogPath = "$env:LOCALAPPDATA\sidepad\sidepadd.log"
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $LogPath)) { Write-Host "NG: ログが無い: $LogPath"; exit 1 }

$sinceTime = if ($Since) { [datetime]::ParseExact($Since, 'yyyy-MM-dd HH:mm:ss', $null) } else { [datetime]::MinValue }

# ログ行: [2026-08-08 15:59:40.866] STATE streaming: ... (0.2s after attach)
$streamRx = [regex]'^\[(?<ts>[\d\-]+ [\d:\.]+)\] STATE streaming: .*\((?<sec>[\d\.]+)s after attach\)'
$attachRx = [regex]'^\[(?<ts>[\d\-]+ [\d:\.]+)\] EVENT attached:'
# teardown の秒数 = そのセッションの配信継続時間（daemon_main の streamed_s）
$teardownRx = [regex]'^\[(?<ts>[\d\-]+ [\d:\.]+)\] STATE teardown after (?<sec>[\d\.]+)s'

# attach → その後最初の streaming をペアリングして1サイクルとする。
# セッション内の自動復帰（同一attachに対する2本目以降の streaming 行）は
# サイクルに数えない（件数一致で弾くと復帰できたのにFAILになる。2026-08-10）。
# streaming に到達しなかった attach（取り残し）は unreached として即FAIL。
$attachCount = 0
$streams = @()
$unreached = 0
$pendingAttach = $false
$awaitTeardown = $null  # 直近サイクルの初回セッション（teardown待ち）
foreach ($line in Get-Content $LogPath) {
    $m = $streamRx.Match($line)
    if ($m.Success) {
        $ts = [datetime]::ParseExact($m.Groups['ts'].Value, 'yyyy-MM-dd HH:mm:ss.fff', $null)
        if ($ts -ge $sinceTime -and $pendingAttach) {
            $cycle = [pscustomobject]@{
                Time = $ts
                AttachToStreamSec = [double]$m.Groups['sec'].Value
                StreamedSec = $null  # 初回セッションの継続時間（末尾なら$nullのまま=配信中）
            }
            $streams += $cycle
            $awaitTeardown = $cycle
            $pendingAttach = $false
        }
        continue
    }
    $t = $teardownRx.Match($line)
    if ($t.Success) {
        # サイクルの「初回セッション」の teardown だけ拾う（同一attach内の
        # 自動復帰で生じる2本目以降のセッションは対象外）
        if ($null -ne $awaitTeardown) {
            $awaitTeardown.StreamedSec = [double]$t.Groups['sec'].Value
            $awaitTeardown = $null
        }
        continue
    }
    $a = $attachRx.Match($line)
    if ($a.Success) {
        $ts = [datetime]::ParseExact($a.Groups['ts'].Value, 'yyyy-MM-dd HH:mm:ss.fff', $null)
        if ($ts -ge $sinceTime) {
            if ($pendingAttach) { $unreached++ }  # 前のattachがstreaming未到達のまま
            $attachCount++
            $pendingAttach = $true
            $awaitTeardown = $null  # 新しい抜き差し。前サイクルのteardown待ちは打ち切り
        }
    }
}
if ($pendingAttach) { $unreached++ }  # ログ末尾で未到達のまま終わった

Write-Host "対象範囲: $(if ($Since) { $Since + ' 以降' } else { 'ログ全体' })"
Write-Host "attachイベント: $attachCount 件 / streaming到達: $($streams.Count) 件"
$i = 0
foreach ($s in $streams) {
    $i++
    $mark = if ($s.AttachToStreamSec -gt 10) { 'OVER' }
            elseif ($null -ne $s.StreamedSec -and $s.StreamedSec -lt $MinStreamSec) { 'DIED' }
            else { 'OK ' }
    $dur = if ($null -ne $s.StreamedSec) { ('{0,7:N1}s' -f $s.StreamedSec) } else { ' (配信中)' }
    Write-Host ("  cycle {0,2}: {1}  attach->stream {2,6:N1}s  streamed {3}  ({4})" -f `
        $i, $mark, $s.AttachToStreamSec, $dur, $s.Time.ToString('HH:mm:ss'))
}
if ($streams.Count -gt 0) {
    $max = ($streams | Measure-Object -Property AttachToStreamSec -Maximum).Maximum
    Write-Host ("attach->stream 最大: {0:N1}s" -f $max)
}

if ($Cycles -gt 0) {
    $under10 = @($streams | Where-Object { $_.AttachToStreamSec -le 10 }).Count
    # 初回セッションが MinStreamSec 未満で死んだサイクル（到達直後に死んで
    # 再試行ループしていても従来判定はPASSしてしまう穴。teardown行が無い=
    # まだ配信中はOK扱い）
    $died = @($streams | Where-Object { $null -ne $_.StreamedSec -and $_.StreamedSec -lt $MinStreamSec }).Count
    # 到達しなかった抜き差し（unreached）を無視してPASSにしない
    # （受入は「20回**連続**で自動復帰」）
    $pass = ($streams.Count -ge $Cycles) -and ($under10 -eq $streams.Count) -and
            ($unreached -eq 0) -and ($died -eq 0)
    Write-Host ("[{0}] attach {1} 件 / 到達サイクル {2} 件（要求 {3}）・未到達 {4} 件・全て10秒以内={5}・{6}s未満で死んだ初回セッション {7} 件" -f `
        ($pass ? 'PASS' : 'FAIL'), $attachCount, $streams.Count, $Cycles, $unreached, ($under10 -eq $streams.Count), $MinStreamSec, $died)
    exit ($pass ? 0 : 2)
}
exit 0
