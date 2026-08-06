---
type: steering-tasklist
generated:
  by: kuroko/claude-fable-5
  at: 2026-08-06
---

# タスクリスト: iPad側デコード表示（VideoToolbox + Metal）

## フェーズ1: 純ロジック（ホスト検証可能）

- [x] AnnexB.swift（NALU分割・NAL type判定・SPS/PPS抽出・AVCC変換）を実装
- [x] AnnexB検証スクリプト（xtool/tests/annexb_main.swift + xtool/run-annexb-tests.sh）を実装し PASS
      （Windows swiftc で実行。swift JIT は Windows で FORCE_LOAD シンボル欠落→swiftc方式に変更。WSLでも同スクリプト可）

## フェーズ2: デコード・描画

- [x] VTVideoDecoder.swift（VTDecompressionSession・同期デコード・セッション再生成・DecodeStats）を実装
- [x] MetalFrameRenderer.swift（CVMetalTextureCache・NV12→BT.709シェーダ実行時コンパイル・最新のみ描画・VideoRenderView）を実装
      （LatestFrameStore を decodeキュー↔メイン の境界役として追加）

## フェーズ3: 配線・ビルド

- [x] ConnectionView/LinkViewModel 改修（decodeキュー配線・全画面映像＋統計オーバーレイ・切断時reset）
      （VideoPipeline.swift を新設し decodeキュー・統計集計を分離）
- [x] WSLへ rsync → `xtool dev build` exit 0（警告0まで解消。エラー1件=ネスト閉包のcapture明示、警告2件=Sendable箱で解決）

## フェーズ4: レビュー・実機受入

- [x] 別コンテキストレビュー（Opus・correctness限定）→指摘反映→再ビルド
      （必須4件反映: M1=peek/commitRendered分離・M2=統計窓2048+max追加・M3=RESET時の統計同期リセット・
        M4=セッション再生成の500msバックオフ復帰。任意はA1〜A4のみ採用（保険Wait・NV12明示・cacheFlush・プレーン実寸）。
        A5〜A12は見送り=診断性・微最適化でcorrectness要件に非該当。再ビルド警告0）
- [ ] 実機インストール（K: usbipd bind → くろこ: install-to-ipad.ps1 → K: unbind・Sidepad起動）
- [ ] 実機20秒受入（decodedFrames一致・decodeErrors 0・fps≥55・recv→decode p99≤10ms・K目視）
- [ ] 振り返り記録＋memory更新（ipad-extend-display-project）

## 実装後の振り返り

（完了後に記入）
