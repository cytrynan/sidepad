---
type: steering
tier: T1
generated:
  by: kuroko/claude-opus-5
  at: 2026-08-03
---

# USBデータリンクの実測（probe兼USBTransportClient第一版）

- 目的: PC→iPad の実効スループット / 遅延ゆらぎ / チャンクサイズ耐性 / iPad側CPU を実測し、
  AMFエンコード（ビットレート・スライス粒度・ジッタバッファ深さ）の設計値を確定する。
- 方式: iPad側が TCP をlisten（NWListener, port 7789）、PC側が usbmux 経由で接続して負荷をかける。
  計測コードは捨てずに `USBTransportClient` の第一版として実装する（onFrameData の受け口はそのまま）。
- 主要指標: 実効MB/s、片道遅延ゆらぎ p50/p95/p99（最小値基準＝クロック同期不要）、
  RFC3550ジッタ、最大到着ギャップ、負荷下RTT、アプリCPU%。
- 経路は2つ。①WSL+usbip 経由（今すぐ可・大チャンクで壊れる既知の層を含む＝下限値）
  ②Windowsネイティブ usbmuxd 経由（要 AMDS インストール＝本番構成の値）。①で計測器を検証し②が本番。
- 非スコープ: エンコード実装・仮想ディスプレイ・デコード表示。
- 受入: 4K/16K/64K/256Kチャンクのスイープ結果がJSONで残り、64KB問題がネイティブ経路で
  再現するか否かに白黒がつくこと。
