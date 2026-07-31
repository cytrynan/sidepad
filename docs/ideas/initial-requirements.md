# sidepad - アイデアメモ

## このドキュメントの位置づけ

このドキュメントは壁打ち・ブレストの成果物で、正式な仕様書ではありません。
アイデア段階の考えを自由に書き留める場所です。
正式なドキュメントは `/setup-project` コマンドで作成してください。

---

## プロダクトビジョン

**一言で言うと**: iPad Pro を Windows PC の低遅延な拡張ディスプレイにする自作アプリ（自分用＋学習目的）。

---

## 解決したい課題

1. 外出先・省スペース環境で、手持ちの iPad Pro 12.9 をセカンドモニターとして使いたい
2. 既存品（spacedesk 等）の Wi-Fi 経由では遅延・ジッタが大きく、作業用途に不満
3. ドライバ・キャプチャ・動画エンコード・低遅延通信・iOSネイティブ開発を一気通貫で学びたい

---

## ターゲットユーザー

### ペルソナ

- **職業/役割**: オーナー本人のみ（配布しない）
- **困っていること**: HDMI接続に近い体感遅延のワイヤレス/ワイヤード拡張画面が欲しい
- **欲しいもの**: USB-C ケーブル1本（データ通信）で繋がる遅延20〜35msの拡張ディスプレイ

---

## 主要機能の候補

### P0 (MVP で絶対必要)

1. Windows側: 仮想ディスプレイ生成（IddCx 既存OSSドライバ・テスト署名）＋ Desktop Duplication API キャプチャ
2. Windows側: AMF (Radeon 780M) ハードウェアエンコード（低遅延設定: Bフレーム禁止・スライス送信）
3. 転送: USB C-C データ通信（フォールバック: USB-C→Ethernetアダプタの有線LAN）。UDP系・ジッタバッファ最小
4. iPad側: ネイティブアプリ（Swift + VideoToolbox HWデコード + Metal描画・ProMotion 120Hz）

### P1 (重要だが後回し可)

1. タッチ / Apple Pencil 入力の PC への返送
2. 解像度・リフレッシュレート・ビットレートの設定UI

### P2 (あったら嬉しい)

1. Wi-Fi モード（有線が使えない場面用・遅延は妥協）
2. クリップボード共有

---

## 技術的な検討事項

- 遅延目標: **20〜35ms**（HDMI並み≦16msは原理的に困難と合意済み）
- ビルド環境: **Mac不使用**。WSL2 + xtool（github.com/xtool-org/xtool）でビルド→無料Apple ID署名→実機インストール
- 7日毎再署名の自動化: AltServer(AltStore) Windows常駐
- CI: GitHub Actions macOSランナーはクリーンビルド検証のみ（macOS分は消費10倍）
- 検証状況・経緯の正本: `~/.claude/dev/ios-dev-without-mac.md` と memory `ipad-extend-display-project`
- 言語スタック: PC側は C++/Rust 想定（未確定・/init-stack は保留）、iPad側は Swift(SwiftPM/SwiftUI)

---

## 次のステップ

1. xtool 検証（WSL2→Swift→xtool→iPad実機に Hello World）← マイルストーン1
2. `/setup-project` コマンドで正式なドキュメントを作成
3. `/add-feature [機能名]` で機能実装
