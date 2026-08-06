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
- [x] 実機インストール（K: usbipd bind → くろこ: install-to-ipad.ps1 → K: unbind・Sidepad起動）
      （AMDS導入後初＝`bind --force` が必要になった＋stale exported はケーブル抜き差しで復旧。詳細は振り返り）
- [x] 実機受入 2026-08-07（20秒: PC側11項目PASS・1202/1202欠落0 ＋ 60秒: 3600/3600欠落0・
      画面統計=デコードfps60.0・失敗0・recv→done p50/p99/max=4.4/6.1/20.9ms・K目視「映像がちゃんと動いて見えた」）
- [x] 振り返り記録＋memory更新（ipad-extend-display-project）

## 実装後の振り返り

**実装完了日**: 2026-08-07（実装・レビューは2026-08-06、実機受入は2026-08-07未明）

**計画と実績の差分**
- GitHub issue起票をスキップ（gh CLI未導入→winget導入したが認証はK作業のため未了。tasklist.mdのみで運用）
- AnnexB検証は「WSLで実行」計画を「Windows swiftcで実行」に変更（swift JITがWindowsで壊れているため
  スクリプトをswiftc方式にし、両環境対応にした）
- 受入は20秒に加えて60秒runを追加実施（下記の統計UI設計ギャップの回避のため）

**学んだこと（具体＋抽象）**
- **統計オーバーレイを「受信中のみ表示」にしたため、送信終了＝切断で最終値が画面から消えた**
  （ConnectionView.swift の `model.state != .receiving` 分岐）。60秒runの終盤にKへ撮影してもらい回避したが、
  K作業を1回増やした。→ **受入で読む計器は「テスト終了後に読める」ことまで含めて設計する**
  （PC側REPORTが終了後に取れるのと同じ性質を画面統計にも持たせる）。次回インストール時に
  「切断後も最終値を固定表示」へ修正する
- **AMDS導入（2026-08-05）以降、開発インストールは `usbipd bind` だけでは不可**。Appleドライバが
  デバイスを掴むため `bind --force` が必要。さらに初回attach失敗後は usbipd サービス側に
  「exported」のstale状態が残り、detach・WSL再起動でも消えない → **ケーブル抜き差しで再列挙させると
  一発で直る**。→ install-to-ipad.ps1 の前提が変わった（正本 dev/ios-dev-without-mac.md 追記10）
- **案内文言をコードで確認せずKに伝えた**（「接続待ちと表示される」と案内したが実表示は「接続中」）。
  K が異常と誤認して1往復無駄にした。→ **Kに「画面にXと出る」と案内する時は displayLabel 等の実文字列を
  grep してから書く**（書く前に正本をgrepの変種）
- 60秒runでPC側 bitrate/slice が FAIL したのはテストパターンの動きが小さかったため（ソース条件・
  iPad側受入には無関係と切り分け済み）。**受入判定に使わないrunでもFAIL行が出ると紛らわしい**ので、
  PC側PASS/FAIL行は「ソース条件依存」の注記が既にあるのが効いた

**止まった点（必ず1件）**
- 実機インストール1回目: usbip attach が `Device busy (exported)` で2連続失敗。真因は
  ①AMDSがデバイスを掴んでいた（→bind --force）②失敗後のstale exported状態（→抜き差し）の2段。
  復旧に管理者操作2回＋抜き差し2回のK作業を要した。既知レシピ（detach→usbmuxd再起動→attach）では
  直らない新パターンだった

**次回への改善提案**
- 次回インストール時に統計固定表示のUI修正を同梱する（コード修正は軽微）
- install-to-ipad.ps1 に「bind状態がShared(通常)ならbind --forceを案内する」事前チェックを足す
  （AMDS環境が今後の標準のため）
