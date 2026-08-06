# install-vdd.ps1 — Virtual Display Driver (VDD) 導入（管理者PowerShellで1回実行）
#
# やること:
#   1. winget で VirtualDrivers.Virtual-Display-Driver を導入
#   2. C:\VirtualDisplayDriver\vdd_settings.xml の <resolutions> ブロックを
#      sidepad 用モードセットに差し替え（原本は .bak に退避）
#   3. 仮想ディスプレイデバイスを再起動して設定を反映
#
# 期待する出力: 最後に "=== RESULT ===" 以下で各段の OK/NG が出る。
# NG があってもそのまま閉じてよい（後続の verify はくろこが実施する）。

$ErrorActionPreference = 'Continue'
$results = [ordered]@{}

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "NG: 管理者権限がありません。管理者PowerShellで実行してください。" -ForegroundColor Red
    exit 1
}

# --- 1. winget install -------------------------------------------------------
Write-Host "`n[1/3] winget install VirtualDrivers.Virtual-Display-Driver"
winget install --id=VirtualDrivers.Virtual-Display-Driver -e `
    --accept-source-agreements --accept-package-agreements
$installExit = $LASTEXITCODE
# 導入済みの再実行では winget が非ゼロを返すので、exit code でなく実体で判定する
winget list --id=VirtualDrivers.Virtual-Display-Driver -e *> $null
$results['winget install'] = if ($LASTEXITCODE -eq 0) { 'OK (list で実在確認)' } else { "NG (install exit $installExit / list でも未検出)" }

# --- 2. vdd_settings.xml -----------------------------------------------------
Write-Host "`n[2/3] vdd_settings.xml 差し替え"
$xmlPath = 'C:\VirtualDisplayDriver\vdd_settings.xml'

# sidepad 用モード: iPad Pro 12.9 (4:3) 向け。2732x2048@120 は H.264 L5.2 超のため定義しない。
$sidepadResolutions = @"
<resolutions>
    <resolution><width>2048</width><height>1536</height><refresh_rate>120</refresh_rate></resolution>
    <resolution><width>2048</width><height>1536</height><refresh_rate>60</refresh_rate></resolution>
    <resolution><width>2732</width><height>2048</height><refresh_rate>60</refresh_rate></resolution>
    <resolution><width>1366</width><height>1024</height><refresh_rate>120</refresh_rate></resolution>
    <resolution><width>1366</width><height>1024</height><refresh_rate>60</refresh_rate></resolution>
    <resolution><width>1920</width><height>1080</height><refresh_rate>60</refresh_rate></resolution>
</resolutions>
"@

# インストール直後にファイル生成が遅れることがあるため最大30秒待つ
$waited = 0
while (-not (Test-Path $xmlPath) -and $waited -lt 30) { Start-Sleep 2; $waited += 2 }

if (Test-Path $xmlPath) {
    # 原本の退避は初回のみ（毎回上書きすると2回目の実行で原本が失われる）
    if (-not (Test-Path "$xmlPath.bak")) { Copy-Item $xmlPath "$xmlPath.bak" }
    # ReadAllText は BOM 自動判別・既定 UTF-8。PowerShell 5.1 の Get-Content の
    # ANSI 解釈 → BOM付きUTF-8 書き戻しで非ASCIIが化ける事故を避ける
    $content = [IO.File]::ReadAllText($xmlPath)
    $rx = [regex]::new('(?s)<resolutions>.*?</resolutions>')
    $matchCount = $rx.Matches($content).Count
    if ($matchCount -ge 1) {
        # 先頭の1ブロックだけ置換（モニタごとに複数ブロックを持つスキーマを壊さない）
        $newContent = $rx.Replace($content, $sidepadResolutions, 1)
        try {
            $null = [xml]$newContent  # 整形式チェック。失敗したら書き込まない
            [IO.File]::WriteAllText($xmlPath, $newContent, [Text.UTF8Encoding]::new($false))
            $note = if ($matchCount -gt 1) { " (注: resolutionsブロックが${matchCount}個あり先頭のみ置換。くろこに伝達)" } else { '' }
            $results['xml 差し替え'] = "OK (原本は vdd_settings.xml.bak)$note"
        } catch {
            $results['xml 差し替え'] = "NG (置換結果がXMLとして不正: $($_.Exception.Message)。原本未変更)"
        }
    } else {
        # スキーマが想定と違う場合は壊さない（くろこが原本を読んで手当てする）
        $results['xml 差し替え'] = 'NG (resolutions ブロックが見つからない。原本未変更)'
    }
} else {
    $results['xml 差し替え'] = 'NG (vdd_settings.xml が生成されていない。VDCアプリで Install Driver が必要かも)'
}

# --- 3. デバイス再起動で設定反映 ---------------------------------------------
Write-Host "`n[3/3] 仮想ディスプレイデバイスの再起動"
$dev = Get-PnpDevice -Class Display -ErrorAction SilentlyContinue |
    Where-Object { $_.FriendlyName -match 'Virtual Display' }
if ($dev) {
    $failed = @()
    foreach ($d in @($dev)) {
        pnputil /restart-device $d.InstanceId | Out-Null
        if ($LASTEXITCODE -ne 0) { $failed += "$($d.FriendlyName) (exit $LASTEXITCODE)" }
    }
    $results['デバイス再起動'] = if ($failed.Count -eq 0) {
        "OK ($($dev.FriendlyName -join ', '))"
    } else {
        "NG ($($failed -join '; '))"
    }
} else {
    $results['デバイス再起動'] = 'NG (仮想ディスプレイデバイス未検出。VDCアプリで Install Driver を押してから再実行)'
}

# --- 結果 --------------------------------------------------------------------
Write-Host "`n=== RESULT ==="
$results.GetEnumerator() | ForEach-Object { Write-Host ("{0,-14}: {1}" -f $_.Key, $_.Value) }
Write-Host "この画面はくろこに読ませるのでそのままでOK（スクショ不要、閉じる前に報告だけ）"
