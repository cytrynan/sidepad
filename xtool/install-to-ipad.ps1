# sidepad: ビルド→署名→iPad実機インストールを1コマンドで行う
# 使い方: pwsh -File install-to-ipad.ps1
# 前提: iPadがUSB接続済み・usbipdでbind済み(Shared)・WSL Ubuntu-24.04にxtool/pymobiledevice3環境構築済み
# 背景(なぜ横取り方式か): usbip経由のバルクOUT転送は大チャンクで死ぬため、xtool devの転送は
# 必ずConnectingでハングする。署名済みipaだけxtoolに作らせ、転送はchunked_install.py(4KB刻み)で行う。
# 詳細: ~/.claude/dev/ios-dev-without-mac.md 追記4

$ErrorActionPreference = "Stop"
$usbipd = "C:\Program Files\usbipd-win\usbipd.exe"
$distro = "Ubuntu-24.04"
$repoXtool = $PSScriptRoot  # このスクリプトの置き場所 = <repo>\xtool

function Get-IpadBusId {
    $line = & $usbipd list | Select-String "05ac:12ab" | Select-Object -First 1
    if (-not $line) { throw "iPad (05ac:12ab) が見つからない。USB接続とbind状態を確認 (usbipd list)" }
    return ($line.Line -split '\s+')[0]
}

function Reset-Link {
    param([string]$busid)
    & $usbipd detach --busid $busid 2>$null
    wsl -d $distro -u root bash -c "pkill -x usbmuxd; sleep 2; usbmuxd; sleep 2"
    & $usbipd attach --wsl --busid $busid
    $udid = wsl -d $distro -u root bash -c "sleep 3; timeout 15 idevice_id -l"
    if (-not $udid) { throw "リンク確立失敗 (idevice_id が空)" }
    Write-Host "LINK OK: $udid"
}

$busid = Get-IpadBusId
Write-Host "iPad busid: $busid"

# 1. リンク確立（xtoolのプロビジョニング/署名にもデバイス接続が必要）
Reset-Link $busid

# 2. WSL側: ビルド→署名→ipa横取り（build-grab-ipa.sh）
wsl -d $distro -u root bash -c "cp '$($repoXtool -replace '\\','/' -replace '^C:','/mnt/c')/build-grab-ipa.sh' /root/build-grab-ipa.sh && sed -i 's/\r$//' /root/build-grab-ipa.sh && bash /root/build-grab-ipa.sh"
if ($LASTEXITCODE -ne 0) { throw "build-grab-ipa.sh 失敗" }

# 3. リンクをリセット（kill後のlockdownd汚れを掃除）
Reset-Link $busid

# 4. 4KBチャンク転送＋インストール
wsl -d $distro -u root bash -c "cp '$($repoXtool -replace '\\','/' -replace '^C:','/mnt/c')/chunked_install.py' /root/chunked_install.py && sed -i 's/\r$//' /root/chunked_install.py && timeout 300 /root/pmd3/bin/python /root/chunked_install.py /root/rescue/Sidepad-signed.ipa 4096"
if ($LASTEXITCODE -ne 0) { throw "chunked_install.py 失敗" }

# 5. 受入検証: デバイスのアプリ一覧に居ることを確認
$check = wsl -d $distro -u root bash -c "timeout 30 ideviceinstaller -l 2>/dev/null | grep -i sidepad"
if (-not $check) { throw "インストール検証失敗 (アプリ一覧に無い)" }
Write-Host "VERIFIED: $check"
Write-Host "DONE - iPadでSidepadを起動してください"
