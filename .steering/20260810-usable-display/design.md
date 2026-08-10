# 設計（画面拡張としての完成度）

## 1. カーソル合成

### 方針: キャプチャテクスチャへの GPU 合成（ゼロコピー経路を維持）

```
AcquireNextFrame → (フレーム更新 or カーソル変化)
  → 作業テクスチャへ CopyResource
  → カーソルスプライトを D3D11 描画でブレンド
  → encoder_.SubmitFrame(作業テクスチャ)
```

- **作業テクスチャ**: 2048x1536(または2732x2048) BGRA RENDER_TARGET 1枚を Pipeline が保持。
  毎フレーム CopyResource してから合成（キャプチャテクスチャ自体は読み取り専用のため直接描けない）
- **カーソルスプライト**: `GetFramePointerShape` の生データを CPU で BGRA に正規化し、
  変化時のみ小テクスチャ(≤256x256)を更新
  - `DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR`: そのまま（ストレートアルファ）
  - `MONOCHROME`: AND/XOR マスク → XOR画素は「白・黒縁取り」の可視近似に変換（I-beam対策）
  - `MASKED_COLOR`: XOR画素を同様に近似
- **描画**: 頂点シェーダ＝フルスクリーン四角形をカーソル矩形に配置、ピクセルシェーダ＝サンプルのみ、
  ブレンド＝通常のSrcAlpha/InvSrcAlpha。MetalFrameRenderer と同様に実行時コンパイル（ビルド依存を増やさない）
- **カーソル位置**: `PointerPosition.Visible` が真の間だけ描く。Visible=false（別モニタへ移動）は素通し

### カーソルのみの更新でのフレーム発行

- `LastPresentTime==0` でも `PointerPosition`/`PointerShape` が変化していれば合成→Submit する
- レート制限: カーソル起因の Submit は最小間隔 ~8ms（≈120Hz上限に整合）。
  マウスを振り回しても帯域・エンコーダを飽和させない
- 直近のデスクトップ画像が必要になるため、作業テクスチャとは別に**最新デスクトップのコピー1枚**を保持
  （フレーム更新時に CopyResource で更新し、カーソルのみ更新時はそこから合成）

## 2. ネイティブ 2732x2048@120

- `C:\VirtualDisplayDriver\vdd_settings.xml` に 2732x2048@120 を追加（K管理者・手順はこちらで用意）
- `FindVirtualDisplayOutput` の 2048/1536/120 直書きを定数 `kVddWidth/kVddHeight/kVddHz` に集約
  （daemon_main.cpp と --list-displays 判定の2箇所）
- エンコーダ・転送・iPadデコーダは解像度非依存（既存実装のまま）。実測で確認
- 実測順: ①エンコード単体（fps・p99・フレームサイズ予算） ②E2E受入 ③必要なら 60Mbps
- **リスク**: AMF/VDD が 2732x2048@120 を出せない場合 → 2048x1536 に留めて記録・K判断

## 検証手段（先に渡す）

- カーソル: K目視（追従・I-beam）＋「静止画面でマウスだけ動かして stats.csv の fps が上がる」ことの機械確認
- レイテンシ: 既存 pc_encode_spike 受入の p99 と比較（+2ms 以内）
- 回帰: verify-replug.ps1 を短サイクル(5回)で再走
- 差分ファジング相当: カーソル非表示時の出力が従来とバイト一致であることは**求めない**
  （作業テクスチャ経由に変わるため）。代わりに ffprobe 復号数一致で機能保存を確認

## 非スコープ（設計上の割り切り）

- XOR の厳密再現（背景反転）: 近似で開始し、K が気になったら v1.5 で
- カーソルの iPad 側合成（メタデータ送信方式）: 帯域は削れるがプロトコル拡張が要る。
  まず PC 側合成で成立させる（シンプル・iPad変更ゼロ）
