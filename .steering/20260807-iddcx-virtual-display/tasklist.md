# タスクリスト: IddCx仮想ディスプレイ

- [x] 1. OSSドライバ選定（一次資料裏取り: VDD署名済み・winget可・XML書式） — 2026-08-07 くろこ
- [x] 2. 導入スクリプト整備 `pc/vdd/install-vdd.ps1`（管理者実行・winget→XML差し替え→デバイス再起動） — 2026-08-07
- [x] 3. 検証スクリプト整備 `pc/vdd/verify-vdd.ps1`（非管理者・全ディスプレイのモード列挙と2048x1536@120有無） — 2026-08-07
- [x] 3.5. 【先行実測】キャプチャ120fpsの成立確認 — 2026-08-07 くろこ。内蔵パネル(2880x1800)を
  ChangeDisplaySettingsExW で一時的に120Hz化し、novsyncソース＋pc_capture_spike で
  **fps=120.08・全フレームvsync間隔1x・coalesced=0・max_accum=1** を実測（終了後60Hzへ復帰済み）。
  副産物の罠2件（vsyncウィンドウソースの60律速・EnumDisplayDevicesのサンドボックス不可）は
  dev/screen-capture-windows.md §7 に記録
- [ ] 4. 【K】管理者PowerShellで install-vdd.ps1 実行（k-runbook.md 手順1）
- [ ] 5. verify-vdd.ps1 で仮想ディスプレイ＋120Hzモード確認（受入基準1）
- [ ] 6. pc_encode_spike --display <仮想> 30秒キャプチャで≈120fps実測（受入基準2）
- [ ] 7. E2E: iPad表示でデコードfps≈120・欠落0（受入基準3。統計固定表示の新ビルドで実施）
- [ ] 8. 物理60Hz回帰確認（受入基準4）
- [ ] 9. 実測値の記録（本ファイル振り返り＋memory更新。120fps時のフレーム予算見直し要否を判断）

GitHub issue起票: スキップ（gh 未認証。`gh auth login` はK作業のため。認証後に必要なら起票）

## 振り返り

（実測後に記入）
