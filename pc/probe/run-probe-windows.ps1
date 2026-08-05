# Windowsネイティブ経路（usbip を挟まない本番構成）でリンク実測を回す。
#
# 経路: probe(Python) -> 127.0.0.1:5555 -> pymobiledevice3 usbmux forward
#       -> AppleMobileDeviceService (Windows版usbmuxd) -> USB -> iPad:7789
#
# 【要管理者】usbipd の bind を解除しないと Windows 側のAppleドライバがデバイスを掴めない。
# 使い方（管理者 PowerShell）:
#   pwsh -File run-probe-windows.ps1 -Sweep
#   pwsh -File run-probe-windows.ps1 -Rebind        # 計測後、WSLインストール用に bind し直す
param(
    [switch]$Sweep,
    [int]$Chunk = 65536,
    [double]$Duration = 6,
    [double]$Bitrate = 0,
    [int]$LocalPort = 5555,
    [int]$DevicePort = 7789,
    [string]$Out = "",
    [switch]$Rebind
)

$ErrorActionPreference = "Stop"
$usbipd = "C:\Program Files\usbipd-win\usbipd.exe"
$probe = Join-Path $PSScriptRoot "usb_link_probe.py"
$venv = Join-Path $env:LOCALAPPDATA "sidepad-probe-venv"

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltinRole]::Administrator)
}

# 1. 前提: Apple Mobile Device Service（Windows版 usbmuxd）
$amds = Get-Service -Name "Apple Mobile Device Service" -ErrorAction SilentlyContinue
if (-not $amds) {
    Write-Error @"
Apple Mobile Device Service が見つからない。Windowsネイティブの usbmux 経路が存在しない状態。
  導入: winget install --id Apple.iTunes --silent   （管理者・AMDSが同梱される）
  導入後にこのスクリプトを再実行する。
"@
    exit 1
}
if ($amds.Status -ne "Running") { Start-Service $amds.Name; Start-Sleep 2 }
Write-Host "AMDS: $($amds.Status)"

# 2. usbipd の bind を外して Windows 側にデバイスを返す
if (Test-Path $usbipd) {
    $line = & $usbipd list | Select-String "05ac:12ab" | Select-Object -First 1
    if ($line) {
        $busid = ($line.Line -split '\s+')[0]
        if ($line.Line -match "Attached|Shared") {
            if (-not (Test-Admin)) { Write-Error "usbipd の解除に管理者権限が必要（bind解除しないとAppleドライバが掴めない）"; exit 1 }
            & $usbipd detach --busid $busid 2>$null
            & $usbipd unbind --busid $busid
            Write-Host "usbipd unbind: $busid（Windows側にデバイスを返した）"
            Start-Sleep 5   # Appleドライバの再バインド待ち
        }
    }
}

# 3. probe 用の Python 環境（pymobiledevice3 は v4系。v10系は async API で CLI 挙動が変わる）
if (-not (Test-Path (Join-Path $venv "Scripts\python.exe"))) {
    Write-Host "venv 作成中: $venv"
    python -m venv $venv
    & (Join-Path $venv "Scripts\python.exe") -m pip install --quiet --upgrade pip
    & (Join-Path $venv "Scripts\python.exe") -m pip install --quiet "pymobiledevice3<5"
}
$venvPython = Join-Path $venv "Scripts\python.exe"

& $venvPython -m pymobiledevice3 usbmux list 2>&1 | Select-Object -First 5

# 4. usbmux ポートフォワードを裏で起動
$forward = Start-Process -FilePath $venvPython `
    -ArgumentList @("-m", "pymobiledevice3", "usbmux", "forward", "$LocalPort", "$DevicePort") `
    -PassThru -WindowStyle Hidden
Start-Sleep 3
if ($forward.HasExited) { Write-Error "usbmux forward が即終了した（デバイス未認識の可能性）"; exit 1 }
Write-Host "forward ready: 127.0.0.1:$LocalPort -> device:$DevicePort (pid $($forward.Id))"

try {
    $args = @($probe, "--host", "127.0.0.1", "--port", "$LocalPort", "--wait", "900")
    if ($Sweep) { $args += "--sweep" }
    else { $args += @("--chunk", "$Chunk", "--duration", "$Duration", "--bitrate", "$Bitrate") }
    if ($Out) { $args += @("--out", $Out) }
    & python @args
} finally {
    if (-not $forward.HasExited) { Stop-Process -Id $forward.Id -Force }
    if ($Rebind -and (Test-Path $usbipd)) {
        if (Test-Admin) {
            $line = & $usbipd list | Select-String "05ac:12ab" | Select-Object -First 1
            if ($line) { & $usbipd bind --busid ($line.Line -split '\s+')[0]; Write-Host "usbipd bind 復帰（WSLインストール用）" }
        } else {
            Write-Warning "-Rebind は管理者権限が必要。未実施。"
        }
    }
}
