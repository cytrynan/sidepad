# タスクリスト: IddCx仮想ディスプレイ

- [x] 1. OSSドライバ選定（一次資料裏取り: VDD署名済み・winget可・XML書式） — 2026-08-07 くろこ
- [x] 2. 導入スクリプト整備 `pc/vdd/install-vdd.ps1`（管理者実行・winget→XML差し替え→デバイス再起動） — 2026-08-07
- [x] 3. 検証スクリプト整備 `pc/vdd/verify-vdd.ps1`（非管理者・全ディスプレイのモード列挙と2048x1536@120有無） — 2026-08-07
- [x] 3.5. 【先行実測】キャプチャ120fpsの成立確認 — 2026-08-07 くろこ。内蔵パネル(2880x1800)を
  ChangeDisplaySettingsExW で一時的に120Hz化し、novsyncソース＋pc_capture_spike で
  **fps=120.08・全フレームvsync間隔1x・coalesced=0・max_accum=1** を実測（終了後60Hzへ復帰済み）。
  副産物の罠2件（vsyncウィンドウソースの60律速・EnumDisplayDevicesのサンドボックス不可）は
  dev/screen-capture-windows.md §7 に記録
- [x] 4. 【K】ドライバ導入 — 2026-08-08。**winget段(install-vdd.ps1)はVDCアプリを置くだけでドライバ本体が
  入らない**と判明 → パッケージ同梱の署名済みMttVDD.inf＋devcon.exeで直接導入する
  `install-vdd-driver.ps1` を追加し、K管理者実行1回で完了（hwid Root\MttVDD）
- [x] 5. 受入基準1 PASS — 仮想ディスプレイが **adapter 0（AMD実GPU）配下** に出現し、
  現在モード 2048x1536@120 で自動アタッチ（クロスアダプタ対応は不要と確定）
- [x] 6. 受入基準2 PASS — 仮想ディスプレイのキャプチャ **119.9〜120.1fps フラット・全1x vsync間隔・
  coalesced=0・max_accum=1**（15秒1800フレーム）
- [x] 7. 受入基準3 PASS — E2E（仮想DP→AMF→usbmux→iPad）2回走行とも **受入11項目全PASS**。
  代表値: 20秒2391/2391・missingSeq=0・outOfOrder=0・エンコードfps≈120・latency p99 6.3ms・
  片道上振れp99 3.4ms・ジッタ0.26ms・iPad CPU 37%・スループット37.9Mbps(40Mbps CBR)
- [x] 8. 受入基準4 PASS — 物理ディスプレイのキャプチャ従来通り（注記: 外部モニタ取り外しで
  出力indexが変動する。走行前に --list-displays で確認する運用）
- [x] 9. 記録 — dev/screen-capture-windows.md §7・dev/ios-dev-without-mac.md 追記11・memory更新

GitHub issue起票: スキップ（gh 未認証。`gh auth login` はK作業のため。認証後に必要なら起票）

## 振り返り

- **計画との差分1**: winget導入=ドライバ導入ではなかった（RESULTのNGで検出）。VDCアプリのGUI操作へ
  誘導せず、パッケージ内容を調べて同梱devconによる直接導入へ切替→K作業は追加1コマンドで済んだ。
  「GUIありきの導入物は、パッケージの中身を見るとCLI化できることが多い」。
- **計画との差分2**: 事前不明だった「仮想出力がどのアダプタに載るか」は最良側（実GPU配下）に確定。
  --list-displays を先に作っておいたので確認が一瞬だった（不確実性には観測手段を先に作る）。
- **止まった点**: install-to-ipad.ps1 の chunked_install.py 段が2回連続失敗→同一コマンドの直接再実行で
  EXIT=0（間欠故障。Reset-Link直後のlockdownd汚れ疑い）。以降は「chunked_install失敗は1回は再実行」。
  また、E2E再走行時に外部モニタ取り外しで --display 番号がずれ EnumOutputs NOT_FOUND で1回落ちた。
- **120fps時のフレーム予算**: 40Mbps/120fps=41.7KB/frame で p99 46.7KB(1.12x)・スライス中央値13.8KB
  =16KiB設計内。ビットレート再設計は不要と判断（60fps時と同じCBR予算で120fpsが成立）。
