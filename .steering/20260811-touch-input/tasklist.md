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

- [ ] フェーズ3: レビューと受入 (#15)
  - [ ] レビュー1回（Opus・別コンテキスト・判定器レビュー込み）→必須指摘の修正
  - [ ] iPad再インストール（K: usbipd bind切替＋接続）→ install-to-ipad.ps1
  - [ ] K実機受入（タップ/ドラッグ/2本指スクロール/四隅座標）＋E2E 30秒・verify-replug 3回
  - [ ] 記録（振り返り・orchestration.md・技術知見はdev/へ）

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
