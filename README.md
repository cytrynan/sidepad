# sidepad

iPad Pro を、USB-C ケーブル 1 本で Windows PC の拡張ディスプレイにする自作ツール。PC 側の常駐プログラムと iPad アプリの 2 つで動く。K の自分用と学習用に作っている（`docs/ideas/initial-requirements.md`）。

## できること

- iPad をケーブルで PC に挿すと、PC 側の常駐プログラム `sidepadd` が仮想ディスプレイを有効にして、その画面を iPad に送る。抜くと仮想ディスプレイを片付ける（再接続に備えて 10 秒は残す）。
- iPad の解像度そのままの 2732x2048、120Hz で表示する。使えない環境では 2048x1536 に落とす。
- マウスカーソルを映像に描き込む。iPad 側の画面に移ったカーソルも見える。
- iPad へのタッチを PC のマウス操作として返す。タップは左クリック、1 本指ドラッグはカーソル移動、2 本指ドラッグはスクロール、長押ししてからのドラッグは左ボタンを押したままの移動になる。
- 3 本指タップで、通信の統計を iPad の画面に重ねて表示する（もう一度で消える）。
- ケーブルの抜き差しや PC のスリープ復帰のあとも、自動で配信を再開する。状態の変化はすべてログに残る。

## 使い方

前提: Windows PC（AMD の GPU とドライバ）、Visual Studio 2022 の C++ ツール、iPad Pro。iPad アプリの導入には WSL の Ubuntu-24.04、usbipd、xtool、pymobiledevice3 の環境が要る（`xtool/install-to-ipad.ps1` 冒頭の前提）。

1. 仮想ディスプレイのドライバを入れる（管理者 PowerShell で 1 回）。`install-vdd.ps1` が winget でパッケージを入れ、`install-vdd-driver.ps1` がドライバ本体を入れる。

   ```powershell
   pwsh -File pc\vdd\install-vdd.ps1
   pwsh -File pc\vdd\install-vdd-driver.ps1
   ```

2. PC 側をビルドする。`pc\build\` に `sidepadd.exe` などができる。

   ```powershell
   pwsh -File pc\build.ps1
   ```

3. iPad アプリをビルドして実機に入れる（iPad を USB でつなぎ、usbipd で bind 済みにしておく）。

   ```powershell
   pwsh -File xtool\install-to-ipad.ps1
   ```

4. 常駐を登録する（管理者 PowerShell）。ログオン時に自動で起動する。

   ```powershell
   pwsh -File pc\daemon\install-task.ps1            # 登録して即起動
   pwsh -File pc\daemon\install-task.ps1 -Status    # 状態を見る（管理者でなくてよい）
   pwsh -File pc\daemon\install-task.ps1 -Uninstall # 解除
   ```

5. iPad で Sidepad アプリを開き、ケーブルを挿す。

ログは `%LOCALAPPDATA%\sidepad\sidepadd.log`、配信中の統計は `%LOCALAPPDATA%\sidepad\stats.csv` に出る。映らないときの対処と受入判定スクリプトの使い方は [pc/README.md](pc/README.md) の「sidepadd の運用」節にある。

注意: `.steering/20260810-usable-display/tasklist.md` の振り返りに、`pc/vdd/install-vdd*.ps1` を再実行すると 2732x2048@120 のモード定義が落ちる、という未修正の指摘が残っている。ドライバを入れ直したあとは解像度を確かめること。

## 技術の詳細

### 映像とタッチの経路

| 区間 | 実装 | 場所 |
|---|---|---|
| 仮想ディスプレイ | Virtual Display Driver（IddCx）を devcon で有効/無効にする | `pc/src/vdd_control.*`, `pc/vdd/` |
| 画面の取り込み | DXGI Desktop Duplication。カーソルは別に受け取って合成する | `pc/src/pipeline.*`, `pc/src/cursor_compositor.*` |
| エンコード | AMD AMF の H.264 ハードウェアエンコード（40 Mbps CBR が既定） | `pc/src/amf_encoder.*` |
| 転送 | usbmux 経由の USB 直結 TCP。`[u32 length][u8 type][payload]` 形式 | `pc/src/stream_sender.*`, `pc/src/usbmux_events.*` |
| 表示（iPad） | VideoToolbox でデコードし、Metal で描く | `Sources/Sidepad/VTVideoDecoder.swift`, `MetalFrameRenderer.swift` |
| タッチの返送 | 同じ接続の逆向きに type=8 のメッセージを送り、PC 側で SendInput する | `Sources/Sidepad/TouchInput.swift`, `pc/src/input_injector.*` |

### ディレクトリ

| パス | 中身 |
|---|---|
| `pc/` | PC 側（C++17、CMake + Ninja を `build.ps1` が Visual Studio 同梱版で動かす）。`sidepadd` のほか、計測用の `pc_capture_spike` / `pc_encode_spike` / `pc_test_source` をビルドする。計測結果と各ツールの説明は [pc/README.md](pc/README.md) |
| `pc/daemon/` | 常駐の登録（`install-task.ps1`）と受入判定（`verify-replug.ps1`, `verify-soak.ps1`） |
| `pc/vdd/` | 仮想ディスプレイドライバの導入と確認 |
| `pc/probe/` | USB リンクの計測、iPad 側の受信モック、タッチ注入の機械検証 |
| `pc/third_party/AMF/` | AMF のヘッダ（同梱。ランタイムは AMD ドライバから読む） |
| `Sources/Sidepad/` | iPad アプリ（Swift、iOS 17 以上、`Package.swift`） |
| `xtool/` | Mac なしで iPad アプリをビルドして入れるスクリプトと、Swift の単体テスト（`run-annexb-tests.sh`, `run-touchwire-tests.sh`） |
| `.steering/` | 作業単位ごとの計画・計測値・受入結果 |
| `docs/ideas/` | 企画メモ。未実装の構想（設定ファイル化、再署名の自動化など）を含み、正式な仕様ではない |

iPad アプリは無料の Apple ID 署名で入れている。署名には期限があり、切れたら入れ直す（`.steering/20260801-xtool-verify/k-runbook.md` では 7 日としているが、xtool の公式に数値の記載はない）。実機に入れるビルドを変えたら `App-Info.plist` の `CFBundleVersion` を上げる（`xtool/install-to-ipad.ps1` 冒頭の運用メモ）。

このリポは claude-code-template から作った。`CLAUDE.md` の技術スタック節は未設定のまま。

## ライセンス

MIT License（`LICENSE` の著作権表記は「Copyright (c) 2025 Generative Agents」）。
