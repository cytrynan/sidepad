---
type: steering-requirements
generated:
  by: kuroko/claude-fable-5
  at: 2026-08-06
---

# 要求内容（T2: brief＋受入基準＋非スコープ）

## 概要

iPad側パイプラインの未実装区間「受信済みAnnex-B AU → 画面表示」を埋める。
`USBTransportClient.onFrameData` に**アクセスユニット丸ごと**が届く形は前タスクで成立済みなので、
今回は VideoToolbox ハードウェアデコード（`VTVideoDecoder`）と Metal 描画（`MetalFrameRenderer`）を
実装し、ConnectionView を「映像全画面＋統計オーバーレイ」に組み替える。

## 背景

- PC→iPad転送は実機受入済み（実機20秒 1201/1201・欠落0・片道上振れp99 4.3ms。
  `.steering/20260806-usb-transport-integration/`）
- ストリームは H.264 High・Bフレーム無し・**IDRは先頭1回のみ＋イントラリフレッシュ**
  （SPS/PPSは先頭IDRに付属。`pc/src/amf_encoder.cpp`）
  → 表示順=デコード順、ただし**エンコード済みフレームは1枚も捨てられない**（全フレームがP参照）。
  「遅れフレームは捨てて最新表示」は**デコード後（表示段）**でのみ行う

## 受け入れ条件（correctness要件）

コード完了（1〜2。ローカルで無人検証可能）:
- [ ] WSL側 `xtool dev build` が成功（rsync同期後。Swiftのコンパイル証明）
- [ ] Annex-B解析（NALU分割・SPS/PPS抽出・AVCC変換）の検証スクリプトが WSL で exit 0
      （AnnexB.swift＋テストmainを連結して swift インタプリタで実行。実機なしで検証できる
      唯一のロジック部。VT/Metalはテスト対象外）

最終受入（3〜5。K作業=usbipd bind(管理者)→インストール→unbind(管理者)→Sidepad起動 の後にくろこが実施）:
- [ ] 実機20秒・40Mbps・60fps送信で、画面統計に **decodedFrames == 受信フレーム数**・
      **decodeErrors == 0**・デコードfps ≥ 55 が出る（既存REPORTの missingSeq=0 と併読）
- [ ] 受信→デコード完了の壁時計 p99 ≤ 10ms（HWデコードの実力確認。予算20〜35msの内訳把握用）
- [ ] K目視: iPadにPC画面（pc_test_sourceの変化）が滑らかに表示される

## スコープ外

- IddCx仮想ディスプレイ・120Hz実測（次タスク。今回はPCソースが60Hzのため60fps表示まで）
- HEVC/AV1・色域厳密化（BT.709 video range 固定。視覚破綻があれば次回対応）
- 途中参加への耐性（IDRが先頭のみ＝アプリ再起動時はPC側の送信再起動が前提。
  SPS/PPS未着フレームは破棄カウントのみ）
- 自動再接続・回転/レイアウト作り込み（アスペクトフィット固定）

## 参照

- ワイヤ形式の正本: `Sources/Sidepad/ProbeProtocol.swift`（DATA payload = [u64 seq][u64 t_pc_ns][Annex-B AU]）
- エンコーダ設定の正本: `pc/src/amf_encoder.cpp` / `.steering/20260806-amf-encoder/`
- 実機インストール手順の正本: `~/.claude/dev/ios-dev-without-mac.md` 追記4・5（install-to-ipad.ps1）
