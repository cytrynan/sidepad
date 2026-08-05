---
type: steering-design
generated:
  by: kuroko/claude-fable-5
  at: 2026-08-06
---

# 設計書: AMFエンコード段（実測値駆動）

## 実測値 → 設計値のマッピング（本作業の核）

| 実測値（2026-08-06 usbmux直結） | 導かれる設計値 | 実装箇所 |
|---|---|---|
| 40Mbps・16KB書き込みで片道p99 2.5ms（最良） | **転送スライス=16KiB** を将来の送信単位とし、エンコーダの `SLICES_PER_FRAME = ceil(フレーム予算/16KiB)` で1スライスNAL≈16KBに揃える（40Mbps/60fps → 83KB/フレーム → 6スライス） | AmfEncoder::Init |
| 実効上限209〜249Mbps（40〜80Mbpsに余裕3倍） | **既定40Mbps CBR**（`--bitrate-mbps` で80まで想定。120も実測欠落0だが120Hz向けなので今回は既定にしない） | CLI＋Init |
| 約15〜30msの単発ヒッカップが常在／深いバッファ不適 | **VBVバッファ=1フレーム分(bitrate/fps)**（フレームサイズの尖りを予算内に抑える）＋**入力キュー深さ実質0**（`AMF_INPUT_FULL` なら即破棄=遅れフレームは捨てて最新を出す）＋**定期IDR禁止**（IDRバーストは16KBスライス×転送予算を吹き飛ばす）→ イントラリフレッシュで約1秒かけて面を回す | Init＋Submit |

## アーキテクチャ

```
[DXGI AcquireNextFrame] --GPU--> [CopyResource → AMFSurface(DX11/BGRA, プール4枚)]
        │ (main.cppの型を踏襲)              │ SubmitInput（多忙なら破棄+カウント）
        │                                   ▼
        │                     [AMF H.264エンコーダ (VCN/Radeon 780M)]
        │                                   │ QueryOutput（ポーラスレッド）
        ▼                                   ▼
   [統計: fps/レイテンシ/サイズ]   [Annex-B解析(スライスNAL統計) + .h264ダンプ]
```

- キャプチャとエンコードは**同一のID3D11Device**（ゼロコピー。CPUリードバックなし）
- スレッド2本: メイン=キャプチャ+Submit / ポーラ=QueryOutput+統計+ファイル書き
- レイテンシ計測はptsをキーにSubmit時刻を`unordered_map`で持ち、出力ptsで突き合わせ

## コンポーネント

### 1. `pc/third_party/AMF/`（ヘッダのベンダリング）
- GPUOpen `AMF` リポ **v1.5.2** の `amf/public/include` のみをコミット（MIT。LICENSE同梱・出所とバージョンをREADMEに明記）
- ランタイムは**リンクしない**: `LoadLibraryW(AMF_DLL_NAME)`（amfrt64.dll・Radeonドライバ同梱）→ `AMFInit` を手書きの最小ローダで取得（サンプルのpublic/commonは持ち込まない）

### 2. `pc/src/amf_encoder.h/.cpp` — AmfEncoderクラス
**Init手順（順序が仕様）**:
1. `AMFVideoEncoderVCE_AVC` を作成
2. `USAGE = ULTRA_LOW_LATENCY`（B無し等の低遅延既定を先に敷く。**USAGEは最初に設定**が公式規約）
3. 上書き: `RATE_CONTROL_METHOD=CBR` / `TARGET_BITRATE=PEAK_BITRATE=目標` /
   `VBV_BUFFER_SIZE = 目標/fps`（=1フレーム） / `FRAMESIZE` / `FRAMERATE` /
   `IDR_PERIOD=0`（定期IDR禁止） / イントラリフレッシュ `NUM_MBS_PER_SLOT = 総MB数/fps`（約1秒で全面） /
   `SLICES_PER_FRAME = ceil((目標bit/8/fps)/16384)` / `QUALITY_PRESET=SPEED` / `PROFILE=HIGH`
4. `Init(AMF_SURFACE_BGRA, w, h)` を試行 → 失敗時のみ `AMFVideoConverter`（BGRA→NV12）を前段に挿す
   （最近のAMFランタイムはエンコーダ直のBGRA入力を受ける想定。受けなければ文書化された変換経路に落とす）
- 存在しない可能性のあるプロパティ（LOWLATENCY_MODE等）は失敗を警告ログのみで続行（correctnessは上の必須項目だけ）

**Submit**: `AllocSurface(AMF_MEMORY_DX11, BGRA)` のプール4枚（`AMFSurfaceObserver`で返却）。
プール枯渇 or `AMF_INPUT_FULL` → **そのフレームを破棄しカウント**（設計値「深さ0」の実装）。
CopyResourceは複製フレームのロック中に発行→即ReleaseFrame。

### 3. `pc/src/encode_main.cpp` — pc_encode_spike
- キャプチャループはmain.cppの構造を踏襲（ACCESS_LOSTは報告して終了のまま）
- CLI: `--seconds N` `--bitrate-mbps N` `--fps N` `--output out.h264` `--display N`
- 60フレーム毎に報告: encode fps / レイテンシ avg・p99・max / フレームbytes avg・max /
  達成Mbps / スライス数・中央値サイズ / 破棄数 / IDR数
- 終了時に受入条件の各項目を PASS/FAIL 表示（判定閾値はrequirements.mdの数値をそのまま使う）
- Annex-B解析: 00 00 01 / 00 00 00 01 でNAL分割、type 1/5をスライスとして集計

## 検証（受入検証はくろこが実施）

1. `pwsh -File pc\build.ps1` → exit 0
2. `pc_test_source` 併走 + `pc_encode_spike --seconds 20 --output <scratchpad>\out.h264`
3. 標準出力のPASS/FAIL行を読む＋`ffprobe -count_frames out.h264` で復号可否・フレーム数・解像度を照合
4. 別コンテキストのレビュー1回（Opus。司令塔Fableと分離。correctness限定指摘）

## 実装順序

ベンダリング → CMake → amf_encoder → encode_main → ビルド → 実測 → レビュー → コミット
