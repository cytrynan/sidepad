# pc/daemon/verify-soak.ps1 — 連続稼働ソークの判定（受入: 4時間で fps・メモリ横ばい）
#
# stats.csv（sidepaddが60秒周期で追記）の指定時刻以降の行を前半/後半に割り、
#   1. fps: 後半平均が前半平均の ±5% 以内
#   2. working set: 後半平均が前半平均の +10% かつ +30MB 以内（増加傾向の検出）
#   3. drops: 期間中の増分 0
#   4. 行数: 期待稼働時間ぶんの行が揃っている（欠けはハング/クラッシュの兆候）
# を判定する。
#
# 使い方:
#   pwsh -File pc\daemon\verify-soak.ps1 -Since "2026-08-08 16:00:00" -Hours 4
#   -StatsSeconds は sidepadd を --stats-seconds 短縮で走らせた時だけ合わせる

param(
    [Parameter(Mandatory = $true)][string]$Since,
    [double]$Hours = 4,
    [int]$StatsSeconds = 60,
    [string]$CsvPath = "$env:LOCALAPPDATA\sidepad\stats.csv"
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $CsvPath)) { Write-Host "NG: CSVが無い: $CsvPath"; exit 1 }

$sinceTime = [datetime]::ParseExact($Since, 'yyyy-MM-dd HH:mm:ss', $null)
$rows = Import-Csv $CsvPath | Where-Object { [datetime]$_.time -ge $sinceTime }
if ($rows.Count -lt 4) { Write-Host "NG: 対象行が $($rows.Count) 行しかない（4行以上必要）"; exit 1 }

$half = [int][math]::Floor($rows.Count / 2)
$first = $rows[0..($half - 1)]
$second = $rows[$half..($rows.Count - 1)]

$fpsFirst = ($first | Measure-Object -Property fps -Average).Average
$fpsSecond = ($second | Measure-Object -Property fps -Average).Average
$wsFirst = ($first | Measure-Object -Property working_set_mb -Average).Average
$wsSecond = ($second | Measure-Object -Property working_set_mb -Average).Average
$dropsDelta = ([long]$rows[-1].drops_input + [long]$rows[-1].drops_pool) -
              ([long]$rows[0].drops_input + [long]$rows[0].drops_pool)
# 5%マージン: 実周期は約60.1秒（next_stats再スケジュール+200msポーリング）で、
# マージンゼロだと正常なソークが1行差でFAILする
$expectedRows = [int][math]::Floor($Hours * 3600 / $StatsSeconds * 0.95)

# frames_total の単調増加 = セッションが途切れていない証明。stats.csv は
# 追記専用でデーモン再起動の行が混ざり、再起動すると frames_total と
# working set がリセットされて他の検査がむしろ通りやすくなるため必須
$monotonic = $true
for ($j = 1; $j -lt $rows.Count; $j++) {
    if ([long]$rows[$j].frames_total -lt [long]$rows[$j - 1].frames_total) { $monotonic = $false; break }
}

$fpsDrift = if ($fpsFirst -gt 0) { [math]::Abs($fpsSecond - $fpsFirst) / $fpsFirst } else { 1 }
$wsGrowthPct = if ($wsFirst -gt 0) { ($wsSecond - $wsFirst) / $wsFirst } else { 1 }
$wsGrowthMb = $wsSecond - $wsFirst

$checks = @(
    @{ Name = "fps 前半/後半差 <= 5%   (前 $([math]::Round($fpsFirst,2)) / 後 $([math]::Round($fpsSecond,2)))"; Pass = ($fpsDrift -le 0.05) },
    @{ Name = "メモリ増加 <= +10% かつ +30MB (前 $([math]::Round($wsFirst,1))MB / 後 $([math]::Round($wsSecond,1))MB)"; Pass = ($wsGrowthPct -le 0.10 -and $wsGrowthMb -le 30) },
    @{ Name = "drops 増分 == 0  (増分 $dropsDelta)"; Pass = ($dropsDelta -eq 0) },
    @{ Name = "行数 >= $expectedRows  (実際 $($rows.Count))"; Pass = ($rows.Count -ge $expectedRows) },
    @{ Name = "セッション連続 (frames_total 単調増加)"; Pass = $monotonic }
)
$all = $true
foreach ($c in $checks) {
    Write-Host ("[{0}] {1}" -f ($c.Pass ? 'PASS' : 'FAIL'), $c.Name)
    $all = $all -and $c.Pass
}
exit ($all ? 0 : 2)
