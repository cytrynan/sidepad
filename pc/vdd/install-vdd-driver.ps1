# install-vdd-driver.ps1 — VDDドライバ本体の導入（管理者PowerShellで1回実行）
#
# 背景: winget パッケージ(25.7.26)が入れるのは制御アプリのみで、ドライバ本体は
# VDCアプリの Install Driver 相当の操作が必要。パッケージ同梱の署名済みドライバ
# (SignedDrivers\x86\VDD ※中身はNTamd64) と devcon.exe を使って直接導入する。
#   1. C:\VirtualDisplayDriver\vdd_settings.xml を sidepad 用モードで用意
#   2. pnputil /add-driver でドライバストアへ登録
#   3. devcon install で Root\MttVDD デバイスを作成（既存なら再起動のみ）
#
# 途中で「このデバイス ソフトウェアをインストールしますか?」が出たら「インストール」をクリック。
# 期待する出力: 最後に "=== RESULT ===" で各段の OK/NG。

$ErrorActionPreference = 'Continue'
$results = [ordered]@{}

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "NG: 管理者権限がありません。管理者PowerShellで実行してください。" -ForegroundColor Red
    exit 1
}

# --- 0. パッケージ位置 --------------------------------------------------------
$pkg = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\VirtualDrivers.Virtual-Display-Driver_*" -Directory |
    Select-Object -First 1
if (-not $pkg) {
    Write-Host "NG: winget パッケージが見つからない（先に install-vdd.ps1 を実行）" -ForegroundColor Red
    exit 1
}
$vddDir = Join-Path $pkg.FullName 'SignedDrivers\x86\VDD'   # フォルダ名はx86だがINFはNTamd64
$inf = Join-Path $vddDir 'MttVDD.inf'
$devcon = Join-Path $pkg.FullName 'Dependencies\devcon.exe'
$defaultXml = Join-Path $pkg.FullName 'Dependencies\vdd_settings.xml'

# --- 1. vdd_settings.xml -----------------------------------------------------
Write-Host "`n[1/3] C:\VirtualDisplayDriver\vdd_settings.xml を用意"
$xmlDir = 'C:\VirtualDisplayDriver'
$xmlPath = Join-Path $xmlDir 'vdd_settings.xml'
New-Item -ItemType Directory -Force -Path $xmlDir | Out-Null

# sidepad 用モード: iPad Pro 12.9 (4:3) 向け。<global> の 120Hz が全解像度へ複製される。
# 2732x2048 は H.264 L5.2 制約により 60Hz 運用（global複製で120も並ぶが選択しない運用）。
$sidepadResolutions = @"
<resolutions>
    <resolution><width>2048</width><height>1536</height><refresh_rate>120</refresh_rate></resolution>
    <resolution><width>2732</width><height>2048</height><refresh_rate>60</refresh_rate></resolution>
    <resolution><width>1366</width><height>1024</height><refresh_rate>120</refresh_rate></resolution>
    <resolution><width>1920</width><height>1080</height><refresh_rate>60</refresh_rate></resolution>
</resolutions>
"@

$srcXml = if (Test-Path $xmlPath) { $xmlPath } else { $defaultXml }
if (Test-Path $xmlPath) {
    if (-not (Test-Path "$xmlPath.bak")) { Copy-Item $xmlPath "$xmlPath.bak" }
}
$content = [IO.File]::ReadAllText($srcXml)
$rx = [regex]::new('(?s)<resolutions>.*?</resolutions>')
if ($rx.IsMatch($content)) {
    $newContent = $rx.Replace($content, $sidepadResolutions, 1)
    try {
        $null = [xml]$newContent
        [IO.File]::WriteAllText($xmlPath, $newContent, [Text.UTF8Encoding]::new($false))
        $results['settings.xml'] = "OK ($xmlPath)"
    } catch {
        $results['settings.xml'] = "NG (置換結果がXML不正: $($_.Exception.Message))"
    }
} else {
    $results['settings.xml'] = 'NG (resolutions ブロックが見つからない)'
}

# --- 2. ドライバストアへ登録 --------------------------------------------------
Write-Host "`n[2/3] pnputil /add-driver"
pnputil /add-driver $inf /install
$results['add-driver'] = if ($LASTEXITCODE -eq 0 -or $LASTEXITCODE -eq 259 -or $LASTEXITCODE -eq 3010) {
    "OK (exit $LASTEXITCODE)"
} else {
    "NG (exit $LASTEXITCODE)"
}

# --- 3. デバイス作成（既存なら再起動のみ） -------------------------------------
Write-Host "`n[3/3] Root\MttVDD デバイス"
$existing = Get-PnpDevice -Class Display -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -like 'ROOT\*' }
if ($existing) {
    foreach ($d in @($existing)) { pnputil /restart-device $d.InstanceId | Out-Null }
    $results['device'] = "OK (既存を再起動: $($existing.FriendlyName -join ', '))"
} else {
    & $devcon install $inf 'Root\MttVDD'
    $results['device'] = switch ($LASTEXITCODE) {
        0 { 'OK (作成)' }
        1 { 'OK (作成・再起動要求あり)' }
        default { "NG (devcon exit $LASTEXITCODE)" }
    }
}

Start-Sleep 3
$check = Get-PnpDevice -Class Display -ErrorAction SilentlyContinue |
    Where-Object { $_.FriendlyName -match 'Virtual|Mtt' -or $_.InstanceId -like 'ROOT\*' }
$results['出現確認'] = if ($check) {
    "OK ($(@($check | ForEach-Object { "$($_.FriendlyName)[$($_.Status)]" }) -join ', '))"
} else {
    'NG (Displayクラスに仮想デバイスが見えない)'
}

Write-Host "`n=== RESULT ==="
$results.GetEnumerator() | ForEach-Object { Write-Host ("{0,-14}: {1}" -f $_.Key, $_.Value) }
Write-Host "この画面は閉じる前にくろこへ完了報告だけでOK"
