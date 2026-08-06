# 要求: IddCx仮想ディスプレイの導入（120Hz実測まで）

## brief

- 目的: iPadを「PCの拡張ディスプレイ」にする完成形。物理画面の複製ではなく専用の仮想モニタを
  Windowsに立て、それをキャプチャ→AMFエンコード→USB転送→iPad表示に接続する。
  あわせて未検証の「60fps超え」（20260803-dxgi-fps-investigation の積み残し）を120Hzで実測する。
- 方式: 自作せず既存OSS **VirtualDrivers/Virtual-Display-Driver (VDD) 25.x** を導入する。
  リリースは署名済みで **テスト署名モード不要**（2026-08-07 一次資料確認: GitHub releases /
  公式docs）。設定は `C:\VirtualDisplayDriver\vdd_settings.xml`（width/height/refresh_rate）。
- 選定理由: 完全OSS（MIT）・任意解像度/レート定義（2048x1536@120等の4:3非標準モードが必要）・
  winget導入可。次点 parsec-vdd は制御コードのみMITでドライバ本体がParsec専有バイナリのため見送り。
- K作業は原理的に不可能な部分のみ: 管理者PowerShellで `pc\vdd\install-vdd.ps1` を1回実行
  （＋VDCアプリでの Install Driver クリックが要る可能性あり）。以降の検証・実測はくろこが実施。
- PC側コードは変更不要の見込み: `pc_encode_spike --display N` が出力選択と実レート追従
  （DisplayRefreshHz）を実装済み。

## 受入基準（correctness）

1. 仮想ディスプレイがWindowsに拡張モニタとして出現し、**2048x1536@120** モードを選択できる
   （`pc\vdd\verify-vdd.ps1` の出力で確認）。
2. `pc_encode_spike --display <仮想>` が30秒キャプチャで **フラット≈120fps**
   （判定規律は 20260803-dxgi-fps-investigation と同じ: vsync倍数ヒストグラム・AccumulatedFrames）。
3. E2E: 仮想ディスプレイ→AMF→USB→iPad表示が成立し、**デコードfps≈120・欠落0**
   （測定値・未達なら原因を tasklist.md 振り返りに記録）。
4. 回帰なし: 物理プライマリ（60Hz）の従来キャプチャ・送信が引き続き動く。

## 非スコープ

- IddCxドライバの自作（学習目的で将来やる場合は別作業単位）
- HEVC化・120fps時のビットレート/フレーム予算の再設計（実測を見てから別途判断）
- タッチ入力・オーディオ・複数仮想ディスプレイ

## メモ（一次資料）

- https://github.com/VirtualDrivers/Virtual-Display-Driver/releases （25.7.23 Beta, 署名済み）
- https://virtualdrivers-virtual-display-driver.mintlify.app/ （winget id:
  `VirtualDrivers.Virtual-Display-Driver`、vdd_settings.xml の書式）
- https://github.com/nomi-san/parsec-vdd （比較対象。SignPath署名・レジストリ設定・MIT＋専有ドライバ）
- H.264レベル制約: 2048x1536@120 ≈ 377Mpx/s → L5.2内。2732x2048@120 はL5.2超のため定義しない。
