---
type: steering-design
generated:
  by: kuroko/claude-fable-5
  at: 2026-08-06
---

# 設計書: USB転送統合（エンコーダ出力→16KiBチャンク→usbmux直結）

## ワイヤ設計（既存プロトコルとの整合が核）

- **1エンコードフレーム = 1 DATAメッセージ**: `[u32 len][u8 type=3][u64 seq][u64 t_pc_ns][Annex-B AU]`
  - iPad側 `Framing.drain` がメッセージ境界を復元し `onFrameData` に**アクセスユニット丸ごと**が届く
    → 次タスク（VideoDecoder接続）でフレーム切り出しの追加実装が不要になる
  - seq=フレーム連番・t_pc_ns=送信開始時の単調ns → 既存 `LinkStats`（欠落・片道上振れ・ギャップ）が
    そのまま受入検証の計器になる
- **ソケット書き込みは16KiB単位**: probe実測の最良書きサイズ（40Mbpsで片道p99 2.5ms）に合わせ、
  `send()` を最大16384Bずつ発行。SO_SNDBUF・その他ソケットオプションは probe と同条件
  （TCP_NODELAY のみ設定・バッファ既定値）を保つ＝実測値がそのまま予測になる

## コンポーネント

### 1. `pc/src/stream_sender.h/.cpp` — StreamSender（新規・Winsock）

- `ConnectTcp(host, port)`: 平TCP接続（ローカル模擬受信・将来の有線LAN用）
- `ConnectUsbmux(device_port)`: AMDS(127.0.0.1:27015) へ2本のusbmux plistセッション
  - 1本目 ListDevices → `ConnectionType=USB` の最初の DeviceID（XML plistは正規の
    パーサを持ち込まず文字列走査。probeと同じ意味論・1台前提）
  - 2本目 Connect（**PortNumber は htons**・16Bヘッダ `<IIII: length,ver=1,type=8,tag>`）
    → 成功後は同じソケットが生パイプ（以降plistは話せない。正本: ios-dev-without-mac.md 追記7）
- `SendFrame(seq, t_ns, data, size)`: フレーミング→16KiBチャンクでブロッキング送信。
  所要時間(ns)を統計に蓄積。失敗は false（呼び元でfatal扱い＝黙って続けない）
- 受信スレッド: ACK/PONG/REPORT を排水（フレーム全長= **4+length**。5+lengthの既知バグ注意）。
  REPORTはJSON文字列として保存しイベントで通知
- `RequestReport(timeout)`: REPORT_REQUEST送信→JSON文字列を返す
- `SendReset(label)`: 計測リセット（ackInterval=64。ACKトラフィックを probe より抑える）

### 2. `pc/src/encode_main.cpp` への追加（既存動作は無変更で維持）

- CLI: `--send tcp:HOST:PORT` / `--send usb`（デバイスポート既定7789）。未指定なら従来どおり
- drainスレッドのsink内で同期送信（`EncodedFrame.data` はsink中のみ有効なのでコピー不要）。
  送信が詰まれば PollOutput が遅れ→AMF入力フル→キャプチャ側破棄、という**既存の
  「遅れフレームは捨てる」経路がそのままバックプレッシャになる**（新規キューは作らない）
- 統計に送信所要 avg/p99/max を追加。終了時に REPORT を取得して生JSONを表示し、
  `"missingSeq":0` の文字列判定と送信所要統計を PASS/FAIL 行に追加（--send時のみ）

### 3. `pc/probe/mock_receiver.py` — ローカル模擬受信（新規・検証専用）

- TCP listen（既定7789）・probeと同じFraming・seq連番検査・ACK(interval毎)・PONG応答
- 受信フレーム本体（16Bヘッダ除去後）を到着順に連結して `--out` へ書く
  → **PC側同時ダンプ `.h264` とのバイト一致（fc /b）が転送無欠損の直接証明**（差分照合）
- REPORT_REQUEST に LinkReport と同じキー（packets/missingSeq/throughputMBps/owdExcess*等）で応答

## 検証手順（受入検証はくろこが実施）

1. `pwsh -File pc\build.ps1` → exit 0
2. `mock_receiver.py --out reassembled.h264` 起動 → `pc_test_source` 併走 →
   `pc_encode_spike --seconds 20 --send tcp:127.0.0.1:7789 --output dump.h264`
3. PASS/FAIL行＋ `fc /b dump.h264 reassembled.h264` ＋ `ffprobe -count_frames`
4. 別コンテキストレビュー1回（Opus・correctness限定）→反映→再計測
5. 実機（K: iPad接続＋Sidepad前面）→ `--send usb` 20秒 → REPORT判定（requirements 5項）

## 実装順序

stream_sender → mock_receiver → encode_main配線 → CMake → ビルド → ローカル実測 → レビュー → 実機
