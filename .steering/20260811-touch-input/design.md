# 設計（タッチ→PC返送）

実装の正本。実装エージェントはこのファイルと requirements.md を直接読むこと。

## 1. ワイヤプロトコル（両側で厳密一致させる）

既存フォーマット `[u32 length][u8 type][payload]`（リトルエンディアン、length = type+payload）に
**type = 8 (touch, iPad→PC)** を追加する。payload は固定10バイト:

```
[u8 kind][u8 source][u16 x][u16 y][i16 dx][i16 dy]
kind:   1=move（1本指ドラッグ中のカーソル移動。左ボタンドラッグ中の移動もこれ）
        2=tap （タップ確定。x,yはタップ位置）
        3=scroll（2本指スクロール。x,yは2指の重心、dx,dyは前回からの移動量）
        4=dragBegin（長押し確定。x,yで左ボタン押下。2026-08-11 K承認の追加）
        5=dragEnd  （長押しドラッグ終了。x,yへ移動して左ボタン解放）
source: 0=finger, 1=pencil（PC側は当面同一扱い。ログ区別のためだけに送る）
x,y:    0..65535 の正規化座標。iPadのフルスクリーンビュー左上=(0,0)、右下=(65535,65535)
dx,dy:  スクロール移動量（iPadのポイント単位、符号付き）。kind=3以外は0
```

- 正規化はビューのbounds基準（映像はフルスクリーン・アスペクト一致=2732x2048なのでletterboxなし。
  将来letterboxが入る場合もビュー基準のまま送り、PC側マップは変えない方針＝ズレたらiPad側で映像矩形基準に直す）
- Swift側は `ProbeMessage` enum に `case touch = 8` を追加。**iPad→PC専用**（受信側caseは `break`）
- C++側は `stream_sender.cpp` の受信ループに `case 8` を追加（下記 §3）

### ゴールデンバイト列（両側のテストにこの16進をそのまま直書きして突き合わせる）

```
G1 tap:    kind=2 source=0 x=0x1234 y=0xABCD dx=0 dy=0
   wire =  0B 00 00 00 08 02 00 34 12 CD AB 00 00 00 00
G2 scroll: kind=3 source=1 x=0x8000 y=0x8000 dx=-10 dy=300
   wire =  0B 00 00 00 08 03 01 00 80 00 80 F6 FF 2C 01
G3 dragBegin: kind=4 source=0 x=0x0001 y=0xFFFE dx=0 dy=0
   wire =  0B 00 00 00 08 04 00 01 00 FE FF 00 00 00 00
```
（length=0x0000000B=type1+payload10、全てリトルエンディアン）

### 長押しドラッグ（2026-08-11 K承認の追加・実機受入「ドラッグできない」対応）

- iPad: `UILongPressGestureRecognizer(minimumPressDuration: 0.35, numberOfTouchesRequired: 1)` を
  TouchCatcher の同一ビューに追加。began=dragBegin送信／changed=kind=1 move（8msレート制限共通）／
  ended・cancelled=dragEnd送信。**UIKit既定の排他に任せる**: 指が静止→longPress beganでpan(1)とtap(1)は
  fail、以降の移動はlongPressのchangedで届く。早く動かせばpanが先に走りlongPressがfail（従来のカーソル移動）
- PC: kind=4 → 座標へ move＋LEFTDOWN（1回のSendInputバッチ）／kind=5 → move＋LEFTUP。
  `drags_` カウンタ追加（CSV・サマリ・verify照合も1列/1項目追加）
- **フェイルセーフ（correctness）**: 押しっぱなし事故の防止。`InputInjector` に `drag_active_` を持ち、
  ①dragBegin重複は無視 ②dragEnd未着のまま切断・teardownしたら **Pipeline::Stop() から
  `injector_.ReleaseDragIfActive()` を呼んで LEFTUP を注入**（左ボタンが押しっぱなしのままPCが
  操作不能になるのが最悪の故障形。判定器: selftestに「dragBegin→ReleaseDragIfActiveでactiveが落ちる」
  状態遷移テストを追加。SendInput自体は発火しない形で状態だけ検証）

## 2. iPad側（Sources/Sidepad/）

### 2-1. TouchInput.swift（新規）

- `struct TouchEvent { kind, source, x, y, dx, dy }` と
  `enum TouchWire { static func encode(_ e: TouchEvent) -> Data; static func decode(_ payload: Data) -> TouchEvent? }`
  を定義（ProbeProtocol.swift のバイトヘルパを利用）。**エンコード/デコードは純関数**にして単体テスト対象にする
- `TouchCatcher: UIViewRepresentable`（新規・ConnectionView の ThreeFingerTapCatcher と同様のラップ）:
  - `UITapGestureRecognizer(numberOfTouchesRequired: 1)` → kind=tap
  - `UIPanGestureRecognizer(minimumNumberOfTouches: 1, maximumNumberOfTouches: 1)` → kind=move
    （`location(in:)` の絶対位置を送る。差分でなく絶対座標＝取りこぼしても累積誤差が出ない）
  - `UIPanGestureRecognizer(minimumNumberOfTouches: 2, maximumNumberOfTouches: 2)` → kind=scroll
    （`translation(in:)` の前回差分を dx,dy に。送信後 `setTranslation(.zero, in:)` はせず自前で前回値を保持=
    setTranslationはvelocity計算を壊すため）
  - 3本指は一切扱わない（既存トグルと共存）。`allowedTouchTypes` はdirect+pencil
  - move/scroll は**最小間隔8msに自前レート制限**（最後の送信時刻を保持。最終イベント(ended)は必ず送る）
- 座標正規化: `x = UInt16(clamping: Int(point.x / bounds.width * 65535))`（yも同様）。bounds=0は送らない

### 2-2. USBTransportClient.swift（変更）

- `func sendTouch(_ event: TouchEvent)` を追加: `queue.async` で現在の `connection` に
  `send(.touch, TouchWire.encode(event), on: conn)`。未接続なら黙って捨てる（touchは損失許容）
- 既存の receive 側 switch に `.touch: break` を追加（iPad が受け取ることはない）

### 2-3. ConnectionView.swift（変更）

- **捕捉層は1枚に統合する**（レビュー2026-08-11 必須1で改訂。ZStack の兄弟ビューは
  hitTest で前面が全タッチを取り、背面のレイヤにはジェスチャが一切届かない＝
  「背面に置けば3本指が先取り」という初版の前提はUIKitの挙動と不一致）:
  `TouchCatcher` の同一 UIView に tap(3本指=トグル)・tap(1本指)・pan(1本指)・pan(2本指) の
  4つの recognizer を全部載せ、`ThreeFingerTapCatcher` は廃止する。
  tap(1) には `require(toFail: tap3)` を付ける（3本指トグル時の誤クリック防止。
  タップ確定は指を離した時なので実用上の遅延は生じない）
- `LinkViewModel` に `func sendTouch(_ e: TouchEvent)`（transport へ委譲）を追加し、
  TouchCatcher のコールバックから呼ぶ。**state == .receiving のときだけ送る**（待ち受け画面での誤送信防止）

### 2-4. 単体テスト（xtool/tests/ 既存のannexbテストと同じ形）

- `xtool/tests/touchwire_main.swift`: encode→decodeラウンドトリップ（全kind・境界値0/65535・負のdx,dy）、
  および**ワイヤバイト列のゴールデン照合**（type=8, 固定payloadの16進を直書きしてC++実装と独立に固定）
- 実行はWindows swiftc（annexbテストのrunスクリプトに倣う。`xtool/tests/` 内のREADMEか既存ps1を確認）

## 3. PC側（pc/src/）

### 3-1. input_injector.{h,cpp}（新規）

- `struct TouchMsg { uint8_t kind, source; uint16_t x, y; int16_t dx, dy; };`
  `bool ParseTouchMsg(const uint8_t* payload, size_t size, TouchMsg* out);`（純関数・10バイト厳密）
- `class InputInjector`:
  - `void SetTargetRect(RECT desktop_rect)`（VDD出力の DesktopCoordinates。streaming開始時に設定）
  - `void OnTouch(const TouchMsg& m)`: kindごとに `SendInput`
    - 共通の座標変換（純関数 `POINT MapToVirtualDesktop(uint16_t x, uint16_t y, RECT target, RECT vscreen)` に
      切り出して机上テスト可能にする）:
      `px = target.left + x/65535.0 * (target.right-target.left-1)`（yも同様）→
      仮想スクリーン正規化 `nx = (px - vx) * 65535 / (vw - 1)`（vx,vw は
      `GetSystemMetrics(SM_XVIRTUALSCREEN / SM_CXVIRTUALSCREEN)`。MOUSEEVENTF_ABSOLUTE|VIRTUALDESK は
      仮想スクリーン全体の0..65535を取るため、**target矩形→仮想スクリーンの二段変換が必須**）
    - move: MOUSEEVENTF_MOVE|ABSOLUTE|VIRTUALDESK
    - tap: 移動→LEFTDOWN→LEFTUP の3イベントを1回の `SendInput(3, ...)` で
    - scroll: 移動（重心へ）→ `MOUSEEVENTF_WHEEL`（wheelDelta = pt × 4）。
      **ナチュラル方向（指と同方向にコンテンツが動く）の極性は縦横で逆**
      （レビュー2026-08-11 必須2で確定。初版の本節は自己矛盾していた）:
      縦 = **+dy×4**（WHEEL正=前方回転=コンテンツは下へ動く。指が下dy>0→コンテンツ下→正）／
      横 = **−dx×4**（HWHEEL正=右回転=コンテンツは左へ動く。指が右dx>0→コンテンツ右→負）。
      定数 `kNaturalScroll` と `kWheelPerPoint=4` はファイル冒頭で定数化して後のconfig化に備える
  - 統計: `moves_/taps_/scrolls_` カウンタ（atomic）とアクセサ
- 注入失敗（SendInput戻り値<イベント数）はカウンタ `inject_errors_` に記録（ログ洪水にしない）

### 3-2. stream_sender.{h,cpp}（変更）

- `void SetTouchHandler(std::function<void(const uint8_t*, size_t)> handler)` を追加
  （ReceiverLoop のフレーム解析に `case 8:` を追加し、payload をそのままハンドラへ。
  ハンドラは**受信スレッドで呼ばれる**と明記。SendInputはスレッドフリーなのでそのまま呼んでよい）
- 既存の ack/pong/report 処理は変更しない

### 3-3. pipeline.{h,cpp} / daemon_main.cpp（配線）

- Pipeline に `InputInjector injector_` を持たせ、`InitOutputs` で
  `sender_.SetTouchHandler(...)` → ParseTouchMsg → `injector_.OnTouch`
- `InitCapture` 後に `injector_.SetTargetRect(output_desc_.DesktopCoordinates)`
  （wait_display 後にネイティブへモード変更した場合も InitCapture は変更後なので矩形は正しい）
- daemon の60秒CSVに `touch_moves,touch_taps,touch_scrolls,inject_errors` の4列を追加
  （**stats.csv はヘッダ変更でローテーションする既存機構があるのでヘッダ文字列を更新するだけ**）
- encode spike（CLI）経路でも同じ配線が生きる（--send usb / tcp 両方）

### 3-4. ローカル機械検証（iPad実機なし）

- `pc/probe/mock_receiver.py` に `--send-touch-script` モードを追加:
  接続受理後、映像DATAを読み捨てつつ、スクリプト化したtouchメッセージ列
  （四隅move→中央tap→scroll）を1秒間隔で送る
- 判定スクリプト `pc/probe/verify-touch-inject.ps1`（新規）:
  `pc_encode_spike --send tcp:127.0.0.1:7789 --seconds 15` をモックに向けて走らせ、
  各move後の `GetCursorPos`（P/Invoke）が期待座標±2pxか、終了時ログの
  taps/scrolls カウントがスクリプト送信数と一致するかを判定して PASS/FAIL を出す
  - 期待座標の計算は**判定スクリプト側で独立に**行う（実装と同じ式を写さず、
    ターゲット矩形と正規化座標から自前計算。judge-design.md の教訓=判定器を実装から独立させる）
  - 注意: 実行中はKのマウスが動く。走行時間は15秒に抑え、実行前にチャットで一言断る

## 4. 実装の分担（司令塔メモ）

- エージェントA（Sonnet）: iPad側 §2 一式＋単体テスト
- エージェントB（Sonnet）: PC側 §3 一式＋モック/判定スクリプト
- 共有点はこのファイルの §1 のみ。**ゴールデンバイト列**（touchwire_main.swift と
  verify-touch-inject.ps1 の両方に同じ16進を書く）でプロトコル不一致を機械検出する
- レビュー1回（Opus・判定器レビュー込み）→ ローカル機械検証 → iPad再インストール（K作業）→ K実機受入

## 5. リスク・割り切り

- UIPanGestureRecognizer(1本指) と Tap の同時認識: タップは pan の began 前に確定する短接触なので
  既定の排他で成立する想定。実機で「タップがmoveに化ける」場合は tap 側の
  `require(toFail:)` 調整をiPad側タスクで行う
- スクロール方向の好みは実機で判断が変わりうる → 定数化して1行で反転できる形にしておく
- 旧iPadアプリ×新デーモンは無影響（touchを送らないだけ）。新iPad×旧デーモンは
  未知type無視で無害（Framing.drainは型不明をスキップ…C++側は case default: break を確認すること）
