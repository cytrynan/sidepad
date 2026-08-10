# verify-vdd.ps1 — 仮想ディスプレイの出現とモード確認（非管理者・くろこ実行用）
#
# DXGI 経由（pc_encode_spike --list-displays）で全アダプタ×出力・モード一覧・
# 判定行 "NATIVE MODE 2732x2048@120" / "FALLBACK MODE 2048x1536@120" の
# FOUND/NOT FOUND を別行で出す（合算判定はネイティブ喪失を隠すため廃止）。
#
# 注意: GDI(EnumDisplayDevices) の P/Invoke 実装は、くろこの自動化セッション
# （サンドボックス）では EnumDisplayDevices が常に FALSE を返し使えなかったため
# 廃止した（DXGI 経路は同環境で動作実測済み。2026-08-07）。
#
# 補足: DXGI はデスクトップに attach された出力しか列挙しないことがある。
# 仮想ディスプレイが出てこない場合は、Windows設定 > ディスプレイ で
# 「表示画面を拡張する」に切り替えてから再実行する。

$exe = Join-Path $PSScriptRoot '..\build\pc_encode_spike.exe'
if (-not (Test-Path $exe)) {
    Write-Host "pc_encode_spike.exe が無いのでビルドします..."
    pwsh -File (Join-Path $PSScriptRoot '..\build.ps1')
    if (-not (Test-Path $exe)) { Write-Host "NG: ビルド失敗"; exit 1 }
}
& $exe --list-displays
