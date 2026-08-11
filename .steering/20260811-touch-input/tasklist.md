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

- [~] フェーズ3: レビューと受入 (#15)
  - [x] レビュー1回（Opus・別コンテキスト・判定器レビュー込み）→必須指摘の修正（必須3件+判定器J1-J4全修正・selftest 17/17・ローカル注入検証PASS 全5目標±0px）
  - [x] iPad再インストール（K: usbipd bind切替＋接続）→ install-to-ipad.ps1（v3・2026-08-11。1回目は既知の間欠失敗→再実行で成功）
  - [x] K実機受入 基本項目（タップ/1本指移動/2本指スクロール/四隅/3本指トグル 全部OK 2026-08-11。実機カウンタ move=159 tap=10 scroll=235 inject_errors=0）
  - [ ] E2E 30秒・verify-replug 3回（フェーズ4の再インストールとまとめて実施）
  - [ ] 記録（振り返り・orchestration.md・技術知見はdev/へ）

- [~] フェーズ4: 長押しドラッグ（K実機受入からの追加承認 2026-08-11） (#16)
  - [ ] iPad側: longPress recognizer＋kind=4/5送信・コーデック拡張・touchwireテスト（G3含む）・WSLビルド
  - [ ] PC側: kind=4/5注入（LEFTDOWN/UP）・押しっぱなしフェイルセーフ（ReleaseDragIfActive）・drags_カウンタ・selftest拡張
  - [ ] レビュー（Opus・差分）→修正→再インストール→K受入（長押しでウィンドウ移動/範囲選択・離すと解放・速く動かすと従来のカーソル移動）

---

## 実装後の振り返り

### 実装完了日
{未実施}

### 計画と実績の差分
{未実施}

### 止まった点・失敗した点（必須・1件以上）
{未実施}

### 学んだこと
{未実施}

### 次回への改善提案
{未実施}
