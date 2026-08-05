---
type: steering-requirements
generated:
  by: kuroko/claude-fable-5
  at: 2026-08-06
---

# 要求内容（T2: brief＋受入基準＋非スコープ）

## 概要

PC側のAMFハードウェアエンコード段を実装する。2026-08-06のUSBデータリンク実測値
（`.steering/20260803-usb-datalink-probe/result.md`）をそのままエンコーダの設計入力
（ビットレート・スライス粒度・バッファ深さ）として反映する。

## 背景

- キャプチャ段（DXGI Desktop Duplication）は検証済み・非ボトルネック（60fpsフラット）
- USBデータリンクは実測で成立（209〜249Mbps・欠落0・16KBスライス40Mbps時 片道p99 2.5ms）
- パイプラインの未実装区間「キャプチャ→エンコード済みビットストリーム」を埋めるのが本作業

## 実装対象

`pc_encode_spike`（新規実行ファイル）: DXGIキャプチャ → AMF H.264エンコード（実測値由来の設計値）
→ 統計出力＋Annex-B `.h264` ダンプ。転送はまだ繋がない（ファイル/統計で検証する）。

## 受け入れ条件（correctness要件）

- [ ] `pwsh -File pc\build.ps1` がexit 0（既存2ターゲットもビルド維持）
- [ ] `pc_test_source`（vsync同期・60fps変化源）併走で20秒実行し、エンコードfpsが55以上
- [ ] エンコードレイテンシ（Submit→出力取得）p99 ≤ 12ms
  （当初10ms。レビュー指摘H-1の修正=D3D11イミディエイトコンテキストのスレッド保護
  `SetMultithreadProtected(TRUE)` が直列化コストとして p99 +約3ms（7.5→10.8〜11.3ms、
  4回計測で安定）を要求するため 2026-08-06 に12msへ改定。保護なしはデータレース
  （DEVICE_REMOVED級）を許容することになり不可。安価な代替 `AMFContext::LockDX11` は
  公式APIリファレンスに「AMF内部スレッドが同じロックを取る」保証が明文化されておらず不採用。
  総遅延予算20〜35msに対しエンコード11msでも成立（K差し戻し可））
- [ ] 達成ビットレートが目標40Mbpsの±20%以内（変化源アクティブ時）
- [ ] フレームサイズp99 ≤ フレーム予算(bitrate/fps)の2倍（VBV=1フレームが効いている証拠）
- [ ] スライスNALの中央値が8〜24KB（16KB目標に整合）
- [ ] ダンプした `.h264` を ffprobe が h264 / 実解像度 として認識し、フレーム数が概ね fps×秒 に一致
- [ ] エンコーダ多忙時のフレーム破棄カウンタが実装され、統計に出る（発生0でも可）

## スコープ外

- USB(usbmux)転送への接続（次タスク。出力は今回ファイル/統計まで）
- IddCx仮想ディスプレイ・120Hz（その次のタスク）
- HEVC・AV1（H.264で成立確認後に必要なら比較）
- カーソル合成・マルチモニタ・HDR・ACCESS_LOST自動復旧（spike既知ギャップを踏襲）

## 参照

- 実測値の正本: `.steering/20260803-usb-datalink-probe/result.md`
- キャプチャ実測: `~/.claude/dev/screen-capture-windows.md` / `pc/src/main.cpp`
