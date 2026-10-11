# pc/daemon/install-task.ps1 — sidepadd のログオン時自動起動（管理者PowerShellで実行）
#
# タスクスケジューラに「sidepadd」を登録する。最上位特権(RunLevel Highest)なのは
# devcon による仮想ディスプレイの有効/無効に管理者権限が要るため。
#
# 使い方:
#   pwsh -File pc\daemon\install-task.ps1            # 登録（既存なら置き換え）
#   pwsh -File pc\daemon\install-task.ps1 -Uninstall # 解除
#   pwsh -File pc\daemon\install-task.ps1 -Status    # 状態表示（非管理者可）
#
# ログ:  %LOCALAPPDATA%\sidepad\sidepadd.log / stats.csv

param(
    [switch]$Uninstall,
    [switch]$Status
)

$ErrorActionPreference = 'Stop'
$taskName = 'sidepadd'
$exe = Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) 'build\sidepadd.exe'

if ($Status) {
    $t = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    if (-not $t) { Write-Host "未登録"; exit 0 }
    $info = $t | Get-ScheduledTaskInfo
    Write-Host "登録済み: State=$($t.State) LastRun=$($info.LastRunTime) LastResult=$($info.LastTaskResult)"
    Write-Host "実行ファイル: $($t.Actions[0].Execute) $($t.Actions[0].Arguments)"
    exit 0
}

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "NG: 管理者PowerShellで実行してください（devcon昇格のため）" -ForegroundColor Red
    exit 1
}

if ($Uninstall) {
    $running = Get-Process -Name sidepadd -ErrorAction SilentlyContinue
    if ($running) { Stop-Process -Id $running.Id -Force -Confirm:$false }
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "OK: タスク解除（プロセスも停止）"
    exit 0
}

if (-not (Test-Path $exe)) {
    Write-Host "NG: $exe が無い（先に pwsh -File pc\build.ps1 でビルド）" -ForegroundColor Red
    exit 1
}

# 二重起動防止: 手動テスト起動や前回タスク分が残っていると2プロセスが
# VDDトグルを取り合う（本体側にも Local\sidepadd-singleton mutex あり）
$running = Get-Process -Name sidepadd -ErrorAction SilentlyContinue
if ($running) { $running | Stop-Process -Force -Confirm:$false; Start-Sleep 1 }

# コンソール窓を出さない（2026-10-12 K指示）: sidepadd.exe は CONSOLE サブシステムなので、
# タスクから直接起動すると黒い窓がデスクトップに出続ける。run-hidden.vbs 経由で隠す。
# stdout は %USERPROFILE%\.claude\logs\hidden\sidepadd.log へ（本体ログは従来どおり %LOCALAPPDATA%\sidepad）。
$launcher = Join-Path $env:USERPROFILE '.claude\scripts\run-hidden.vbs'
if (-not (Test-Path $launcher)) { Write-Host "NG: $launcher が無い" -ForegroundColor Red; exit 1 }
$action = New-ScheduledTaskAction -Execute "$env:WINDIR\System32\wscript.exe" `
    -Argument "//B //Nologo `"$launcher`" sidepadd `"$exe`"" -WorkingDirectory (Split-Path -Parent $exe)
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
# 電源接続に関係なく動かす・時間制限なし（常駐のため）
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest

Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
    -Settings $settings -Principal $principal -Force | Out-Null
Write-Host "OK: タスク登録（ログオン時・最上位特権）"

# その場で1回起動しておく（次回ログオンを待たない）
Start-ScheduledTask -TaskName $taskName
Start-Sleep 2
$p = Get-Process -Name sidepadd -ErrorAction SilentlyContinue
Write-Host ($p ? "OK: sidepadd 起動中 (PID $($p.Id))" : "WARN: 起動確認できず（ログを確認）")
