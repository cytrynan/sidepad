---
type: steering-requirements
generated:
  by: kuroko/claude-fable-5
  at: 2026-08-06
---

# 要求内容（T2: brief＋受入基準＋非スコープ）

## 概要

PC側パイプラインの未接続区間「エンコード済みビットストリーム → USBデータリンク」を埋める。
`pc_encode_spike` のエンコーダ出力（Annex-Bアクセスユニット）を、実測で確定したワイヤ形式
（probeプロトコル DATA メッセージ・16KiB単位のソケット書き込み）で **usbmux直結
（AMDS 127.0.0.1:27015 → デバイスポート7789）** に送る。iPad側は既存の
`USBTransportClient`＋`LinkStats` がそのまま受け口・計測器になる（変更しない）。

## 背景

- エンコード段は受入済み（60fps・p99 9.8ms・スライス中央値13.9KB。`.steering/20260806-amf-encoder/`）
- リンクは実測で成立（usbmux直結 209〜249Mbps・欠落0・40Mbps時16KB書きで片道上振れp99 2.5ms。
  `.steering/20260803-usb-datalink-probe/result.md`）
- ワイヤ形式は probe と共用（`ProbeProtocol.swift` のコメントどおり計測用と本実装を分けない）

## 受け入れ条件（correctness要件）

コード完了（1〜4。ローカルで無人検証可能）:
- [ ] `pwsh -File pc\build.ps1` が exit 0（既存3ターゲットもビルド維持）
- [ ] ローカル模擬受信（`--send tcp:...` モード＋`mock_receiver.py`）で `pc_test_source` 併走20秒:
      送信フレーム数＝受信フレーム数・seq欠落0・**受信側で再構成したストリームが同時ダンプの
      `.h264` とバイト一致**・ffprobe で復号可
- [ ] 送信有効時もエンコード段の受入水準を維持: fps≥55・エンコードレイテンシp99≤12ms・破棄統計出力
- [ ] フレーム送信所要時間（SendFrame内の壁時計）の avg/p99/max が統計に出る

最終受入（5。K作業=iPad接続＋Sidepad前面起動 の後にくろこが実施）:
- [ ] 実機20秒・40Mbps: iPadのREPORTで missingSeq=0・受信スループットが達成ビットレート±20%・
      片道上振れ owdExcessP99Ms ≤ 10ms（実測2.5msの4倍マージン。単発ヒッカップは maxArrivalGapMs で観察のみ）

## スコープ外

- iPad側の VideoDecoder / Metal表示への接続（次タスク。今回は受信・計測まで）
- IddCx仮想ディスプレイ・120Hz（その次）
- 自動再接続・輻輳制御・FEC等の高度化（切断はエラー明示して終了でよい）
- iPad側Swiftの変更（LinkStatsのCPUクランプ修正の反映は次回インストール時に別途）

## 参照

- リンク実測の正本: `.steering/20260803-usb-datalink-probe/result.md`
- usbmux直結手順の正本: `~/.claude/dev/ios-dev-without-mac.md` 追記7（16Bヘッダ+XML plist・htons）
- ワイヤ形式の正本: `Sources/Sidepad/ProbeProtocol.swift` / `pc/probe/usb_link_probe.py`
