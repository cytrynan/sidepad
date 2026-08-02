---
type: steering
generated:
  by: kuroko/claude-fable-5
  at: 2026-08-03
---

# DXGI キャプチャ fps 劣化の切り分け（T1）

- 事象: `pc_capture_spike` の 15 秒走行で fps 59.93 → 6.80 → 53.02 → 41.40 → 37.10。単調劣化と記録されていたが再読で非単調。
- 既存ログからの推定: `60 × avg_latency` と `window_seconds` の差＝「フレームが来ない空白時間」。窓2で 6.9s、窓4で 0.7s、窓3/5 は差ほぼ0。**空白型と間隔伸長型の2種が混在**。
- 仮説A: 合成ソース（WinForms 16ms タイマ窓）側の停止・遅延で、キャプチャは正常。
- 仮説B: キャプチャ側が取りこぼし、DXGI が複数フレームを合体（`AccumulatedFrames` > 1）。
- 切り分け手段（correctness 要件）:
  1. spike に計測を追加 — timeout/カーソルのみの回数と消費時間、`AccumulatedFrames`、`LastPresentTime`(QPC) 差から求めた**ソース側 present 間隔**、間隔の vsync 倍数ヒストグラム。
  2. 合成ソースを**vsync 同期の D3D11 present アプリ**に置換し、ソース自身にも present 実測 fps と occluded 回数を出させる。
- 判定: ソース実測 fps とキャプチャ fps が一致すれば仮説A（キャプチャは無罪）。乖離すれば仮説B。
- 非スコープ: エンコード・転送・ACCESS_LOST 復帰・出力選択の是正。
