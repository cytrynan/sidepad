# タスクリスト（タッチ→PC返送）

tracking: #12

## 🚨 タスク完全完了の原則

- `- [ ]` 未着手 / `- [~]` 着手中 / `- [x]` 完了。`[x]`は完了だけに使う
- 全タスクが `[x]` になるまで作業を継続。「時間の都合で後回し」は禁止
- スキップは技術的理由がある場合のみ（理由を明記して `~~取り消し線~~`）

---

## タスク

- [x] フェーズ1: iPad側（タッチ捕捉→送信） (#13)
  - [x] TouchInput.swift（TouchEvent/TouchWire純関数コーデック・TouchCatcher）
  - [x] USBTransportClient.sendTouch / ConnectionView配線（receiving時のみ送信・3本指トグル共存）
  - [x] 単体テスト touchwire_main.swift（ラウンドトリップ＋ゴールデンバイト列）Windows swiftcでPASS（annexb回帰もPASS）
  - [x] WSLビルド（xtool dev build）PASS（2026-08-11 13.8s。注意: `wsl -d Ubuntu-24.04 -u root bash -lc` 必須＝-d無しは既定distroの26.04に入り偽の環境故障に見える・-l無しはswiftがPATHに無い）

- [x] フェーズ2: PC側（受信→SendInput注入） (#14)
  - [x] input_injector.{h,cpp}（ParseTouchMsg・二段座標変換の純関数・move/tap/scroll注入・カウンタ）
  - [x] stream_sender SetTouchHandler / pipeline・daemon配線・stats.csv 4列追加
  - [x] mock_receiver.py --send-touch-script / verify-touch-inject.ps1（判定は実装から独立に計算・軌跡ベース）
  - [x] pc/build.ps1 PASS＋verify-touch-inject.ps1 ローカルPASS（2026-08-11 K立会実走: 全5目標±0px・カウント4/1/1/0一致。1回目はPSサンプラの~1.5s瞬きで右下のみ偽FAIL→判定器にmax sample gap自己申告を追加）

- [x] フェーズ3: レビューと受入 (#15)
  - [x] レビュー1回（Opus・別コンテキスト・判定器レビュー込み）→必須指摘の修正（必須3件+判定器J1-J4全修正・selftest 17/17・ローカル注入検証PASS 全5目標±0px）
  - [x] iPad再インストール（K: usbipd bind切替＋接続）→ install-to-ipad.ps1（v3・2026-08-11。1回目は既知の間欠失敗→再実行で成功）
  - [x] K実機受入 基本項目（タップ/1本指移動/2本指スクロール/四隅/3本指トグル 全部OK 2026-08-11。実機カウンタ move=159 tap=10 scroll=235 inject_errors=0）
  - [x] E2E 30秒・verify-replug 3回（2026-08-12実施。replug=**PASS** 4/4サイクル・最大1.6s・途中死0（判定範囲より前にアプリ未起動状態のattach1件あり＝デーモンは仕様通りbackoff再試行・故障ではない）。E2E=転送系全PASS（欠落0・packets==sent・リンク維持・fps73-85・IDR1・スライス帯内）。**latency p99>12msとビットレート帯外の2件はフェーズ2でK承認済みのネイティブ2732x2048特性**（12.8ms・27.6Mbps相当の再現）でタッチ起因ではない→spike閾値のネイティブ対応はv1.5相談事項へ）
  - [x] 記録（振り返り・orchestration.md・dev/ios-dev-without-mac.md 追記13・judge-design.md 追補）

- [x] フェーズ4: 長押しドラッグ（K実機受入からの追加承認 2026-08-11） (#16)
  - [x] iPad側: longPress recognizer＋kind=4/5送信・コーデック拡張・touchwireテスト（G3含む）PASS・WSLビルドPASS
  - [x] PC側: kind=4/5注入・押しっぱなしフェイルセーフ・drags_カウンタ・selftest 38/38 PASS（レビュー必須3件=状態遷移とSendInputの非アトミック/最終解放が受信スレッド停止前/失敗時武装解除→全修正 b95fb93）
  - [x] レビュー（Opus・差分）→修正→再インストール（v4）→K実機受入 **全部OK**（2026-08-12: 長押しドラッグ/範囲選択/解放/速い移動は従来動作・タップ/スクロール再確認込み）。ローカル機械検証もPASS（9点軌跡±1px・**VK_LBUTTONでドラッグ区間DOWN/終了UP実測**・カウント6/1/1/1/0一致）

---

## 実装後の振り返り

### 実装完了日
2026-08-12（計画〜基本実装〜K受入=2026-08-11、長押しドラッグ追加〜最終受入・回帰=2026-08-12未明）

### 計画と実績の差分
- 長押しドラッグ（フェーズ4）は計画外の追加。K実機受入での「ドラッグできない」フィードバックから同一作業単位内で承認→実装→受入まで実施（v1.5案の前倒し）
- iPad再インストールは計画2回→実績2回だが、v4インストールが**デバイス側の広域デーモン固着**で約1.5時間難航（下記）
- レビューは2ラウンド（基本実装で必須3件+判定器J1-J4、ドラッグ差分で必須3件+判定器盲点2件）。判定器レビューを独立項目にする型が2連続で機能した

### 止まった点・失敗した点（必須・1件以上）
1. **設計文書自体が2件の必須バグの根**: ①ZStack兄弟ビューへの「背面配置で3本指共存」はUIKitのhitTest挙動と不一致（前面ビューが全タッチを取る） ②スクロール極性の記述が同一括弧内で自己矛盾→実装者は文面通り実装して縦反転。**設計の正本を書き切る方式は、正本の誤りがそのまま並行実装2系に複製される**（レビューが唯一の防波堤だった）
2. **v4インストール難航（約1.5時間）**: installd固着（正本追記12の既知症状）に対し、K手動再起動→効かず。実際はtrustd・containermanagerd等を含む広域固着＋usbmuxdソケット不整合＋**私の即席判定ハーネスがパイプ先の終了コードを拾って「EXIT=0」と嘘をつき続けた**三重奏で診断が迷走。`pymobiledevice3 diagnostics restart`→初回ロック解除→即インストールで解決（正本の手順が最初から正解だった。追記13に記録）
3. **注入テストの偽FAIL 2回**: 物理マウス入力の混入（1回目・2回目）とPSサンプラの~1.5s瞬き。軌跡ダンプとmax gap自己申告を判定器に追加して切り分け可能にした

### 学んだこと
- **フェイルセーフはそれ自体が競合の当事者**: 「切断時に左ボタンを解放する」だけでは、①状態遷移と注入の非アトミック窓 ②解放後に受信スレッドがまだ生きている窓 ③解放注入自体の失敗、の3経路で貫通する。**解放の最終呼び出しは「もうイベントが来ないことが保証された時点」（受信スレッドjoin後）に置く**のが型。→ dev/judge-design.md 追補・実装は input_injector.cpp の TransitionDragLocked
- **判定器は自分の観測経路の損失も申告する**: サンプラの瞬き・パイプによる終了コード差し替え・DPI座標系ずれの3つを今回実地で踏んだ。`$LASTEXITCODE`を使う（`| tail`後の`$?`は嘘）・max gap出力・PMv2宣言
- pmd3はこの環境で**usbmuxアドレス明示が必須**（`/var/run/usbmuxd`。CLIは`--usbmux`をサブコマンド後に）→ 正本 dev/ios-dev-without-mac.md 追記13
- WSL操作は `wsl -d Ubuntu-24.04 -u root bash -lc` を崩さない（-d無し=既定distroの26.04で偽の環境故障・-l無し=swiftがPATHに無い）

### 次回への改善提案
- **spike受入のlatency p99≦12ms・ビットレート±20%はネイティブ2732x2048非対応のまま**（フェーズ2承認済み特性でFAILする）→ v1.5で閾値をネイティブ基準に改定
- ドラッグ再接続時の劣化（切断を跨ぐと黙ってカーソル移動化）はレビュー任意指摘のまま→実用で気になったらdragBegin再送をiPad側に
- installd固着の一次対応を install-to-ipad.ps1 に組み込む価値あり（診断→`diagnostics restart`案内の自動化）
