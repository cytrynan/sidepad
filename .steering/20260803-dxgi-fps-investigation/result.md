---
type: steering
generated:
  by: kuroko/claude-fable-5
  at: 2026-08-03
---

# 結果: fps 劣化はキャプチャ側に存在しなかった

## 結論
**仮説A（ソース側が原因・キャプチャは無罪）で確定。** 修正すべきキャプチャ側の不具合は無い。

## 証拠
1. **vsync同期ソース（`pc_test_source`, `Present(1,0)`）に対して 30秒・1799フレーム・全窓 60.00fps ちょうど。**
   `coalesced=0` / `max_accum=1` / vsync gap は全部 `1x` / timeout 0回。取りこぼしが一度も無い。
   `AcquireNextFrame` 外の自前処理は 12〜19ms/1000ms（約1.5%）。
2. **旧ソース（WinForms 16msタイマ窓）を再現すると旧ログが再現。** fps 36.7〜43.9、
   平均 src_interval 25〜27ms。元ログ窓5（37.10fps / 26.671ms）とほぼ一致。
   vsync gap ヒストグラムが `1x:23〜29 / 2x:28〜36` に割れる＝**16ms と 16.667ms のビート**。
   この条件でも `max_accum=1`＝キャプチャはソースが乱れても遅れていない。
3. 旧ログ窓2（6.80fps / 8.8秒）は別原因で、単なるアイドル（60×avg_latency=1.93s に対し窓 8.818s、
   差 6.9秒はフレームが1枚も来ていない時間）。**「単調劣化」という読みが誤りで、無関係な2現象の混同だった。**

## 実装した変更
- `pc/src/main.cpp`: 壁時計内訳（acquire / idle_timeout / cursor_only / other）、`AccumulatedFrames` 統計、
  `LastPresentTime`(QPC) 由来のソース側 present 間隔、vsync 倍数ヒストグラムを追加。
  タイムアウトを 500ms→50ms（内訳の分解能のため）。引数 `[run_seconds] [output_index]`。
- `pc/src/testsource.cpp`（新規）: vsync 同期の合成ソース。自身の present レートと occluded 回数を報告。
- `pc/CMakeLists.txt`: 2ターゲット化、`UNICODE`/`_UNICODE` 定義。
- `pc/README.md`: 結果と結論を差し替え、旧ログは誤読の記録として保存。

## 止まった点（失敗の記録）
- **`Start-Process -WindowStyle Hidden` で合成ソースを起動し、「25秒で5フレーム」という偽の結果を1回作った。**
  GUI アプリの窓自体が出ない。刺激源の起動オプションが刺激を殺していないか、結果を解釈する前に確認する。
- `FindWindow` の P/Invoke 実装を誤り「窓が存在しない」と一度誤判定。`Get-Process | MainWindowTitle` で
  存在を確認して訂正。**否定の結論は別手段で確認する**（§3-B）が実際に効いた。
- リダイレクト先がファイルだと CRT が全バッファリングになり、`Stop-Process` したソースの stdout が
  丸ごと消えた。`setvbuf(_IONBF)` を両方に追加。

## 未検証（次の判断材料）
- **60fps を超えられるか未測定。** 今回のプライマリは 60Hz なので、60 が表示側の上限なのか
  キャプチャの上限なのか分離できていない。iPad の ProMotion 120Hz を活かすなら、
  120Hz 出力か 120Hz の仮想ディスプレイを用意して再測定が要る（＝仮想ディスプレイドライバ導入後に自然に測れる）。
- 実コンテンツ（動画・スクロール）での挙動。合成ソースは全画面均一色なので、
  ダーティ矩形が最大かつ単純な条件しか見ていない。
