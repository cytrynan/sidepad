---
type: steering-design
generated:
  by: kuroko/claude-fable-5
  at: 2026-08-06
---

# 設計書: iPad側デコード表示（VideoToolbox + Metal）

## データフロー（スレッド境界が核）

```
transportキュー(既存)          decodeキュー(新規・直列)         メインスレッド/表示リンク
onFrameData(AU) ──enqueue──▶ AnnexB→AVCC → VTDecode(同期) ──▶ renderer.latest = pixelBuffer
                              全フレーム必ずデコード             MTKView 60〜120Hz draw で
                              (P参照のため間引き禁止)            「最新だけ」描画（遅れは表示段で捨てる）
```

- **transportキューでデコードしない**: 受信ループを塞ぐと到着統計（LinkStats）が汚れる。
  `onFrameData` は decodeキューへの enqueue だけ（Data は Framing.drain が返す独立コピーなので安全）
- **decodeキューは無界の直列キュー**: HWデコードは数ms/枚で60fpsに余裕。ただし健全性監視として
  待ち行列深さ（enqueue−done）を統計に出す（深さが伸び続ける＝デコード律速の検知器）

## コンポーネント

### 1. `Sources/Sidepad/AnnexB.swift` — Annex-B解析（新規・純ロジック）

- `splitNALUs(Data) -> [Data]`: 3/4バイト開始コード両対応でNALU分割（emulation prevention はVT側が処理するので触らない）
- NALtype判定（5=IDR, 7=SPS, 8=PPS, 1=non-IDR。type&0x1F）
- `toAVCC([Data]) -> Data`: 各NALUに4バイトBE長プレフィックス（VCL NALのみ集める。SPS/PPS/SEI/AUDは
  formatDescription側で扱い、サンプルバッファには入れない）
- **ここだけホスト検証対象**: `xtool/run-annexb-tests.sh`（WSL）が AnnexB.swift＋`Tests/annexb_main.swift`
  を連結し swift インタプリタで実行（assert群・exit 0が合格）。
  Package.swift にテストターゲットは**足さない**（`swift test` は全ターゲットをビルドするため
  SwiftUI依存のアプリターゲットが Linux で壊れる）。AnnexB.swift は Foundation のみ import

### 2. `Sources/Sidepad/VTVideoDecoder.swift` — VideoDecoder実装（新規）

- SPS/PPS が届くまで: フレームを破棄し `droppedPreSPS` をカウント（先頭IDRに付属する想定なので通常0）
- SPS/PPS から `CMVideoFormatDescriptionCreateFromH264ParameterSets` → `VTDecompressionSession` 生成
  - `kVTDecompressionPropertyKey_RealTime = true`
  - 出力属性: `kCVPixelBufferMetalCompatibilityKey = true`（ピクセル形式はVT既定=NV12に任せる）
- **同期デコード**（flags = []。B無しなので表示順=デコード順、非同期にする理由がない。
  completion はデコード完了後に同一スレッドで呼ばれる）
- SPS/PPS 変化を検出したらセッション再生成。`kVTInvalidSessionErr`（バックグラウンド復帰等）も再生成
- 統計 `DecodeStats`: decodedFrames / decodeErrors / droppedPreSPS / デコード壁時計 p50/p99（リングバッファ256）
  / 受信→デコード完了 p50/p99（onFrameData で recv時刻を添えて渡す）
- reset(): セッション破棄＋SPS/PPS破棄（切断→再接続で新ストリームが来る時に呼ぶ）

### 3. `Sources/Sidepad/MetalFrameRenderer.swift` — FrameRenderer実装＋表示View（新規）

- `MTKView` サブクラスまたは保持型。`CVMetalTextureCache` で NV12 の2プレーンを
  r8Unorm / rg8Unorm テクスチャとしてゼロコピー取得
- フラグメントシェーダで BT.709 video range → RGB 変換。**シェーダは実行時コンパイル**
  （`device.makeLibrary(source:)`。xtool/SwiftPM の .metal リソースコンパイルは未検証の可動部品なので持ち込まない）
- `latest: CVPixelBuffer` を lock で差し替え、draw で最新だけ描画（＝表示段の「遅れは捨てる」）。
  描画は aspect-fit の全画面クアッド。`preferredFramesPerSecond = 120`（ProMotion追従・60Hzソースでも害なし）
- renderedFrames カウント（同一バッファの再描画は数えない）→ 統計へ
- SwiftUI へは `UIViewRepresentable`（`VideoRenderView`）で出す

### 4. 配線・UI（`ConnectionView.swift` / `SidepadApp.swift` 改修）

- LinkViewModel に decoder/renderer を追加し `transport.onFrameData` を接続
- 画面構成: **映像を全画面**、統計＋ログは半透明オーバーレイ（タップで表示/非表示）。
  受信前は従来の接続待ち画面
- 統計オーバーレイに追加: decoded fps / decodedFrames / decodeErrors / droppedPreSPS /
  decode p99 / recv→decode p99 / queueDepth / renderedFrames
- 切断時（state が .receiving から落ちた時）に decoder.reset()

## 検証手順（受入検証はくろこが実施）

1. WSLで `run-annexb-tests.sh` exit 0 → rsync → `xtool dev build` exit 0
2. 別コンテキストレビュー1回（Opus・correctness限定: スレッド境界・AVCC変換・VTセッション管理）→反映→再ビルド
3. K作業①（管理者PowerShell）: `usbipd bind` → くろこ: `install-to-ipad.ps1` 実走
4. K作業②: `usbipd unbind`（管理者）→ iPad接続確認 → Sidepad起動・前面
5. くろこ: `pc_encode_spike --seconds 20 --send usb` → REPORT＋K読み上げ/写真の画面統計で受入判定

## 実装順序

AnnexB＋テスト → VTVideoDecoder → MetalFrameRenderer → 配線・UI → ビルド → レビュー → 実機
