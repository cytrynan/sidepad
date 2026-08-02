# xtool検証 マイルストーン1 報告（2026-08-01）

実施: Opus 5（司令塔・指揮権限付き）＋ Sonnet 調査係2体。受入検証: Fable 5（全体司令塔）。

## 到達点

Apple ID が不要な部分は**全て完了**。`xtool dev build` が「Darwin SDK が無い」という想定どおりの一点で止まる状態まで実測確認済み。

- WSL は導入済みだった（v2.7.3.0 / カーネル 6.6.114.1、usbipd 要件を満たす）。C: 空き 520GB
- **新規ディストロ `Ubuntu-24.04` を管理者権限なしで作成**（`wsl --install -d Ubuntu-24.04 --no-launch`）。既存 `Ubuntu`(26.04) と既定ディストロは無変更
- 導入済み（すべて動作確認済み）: Swift **6.3.3**（/opt/swift）・xtool **1.17.0**（AppImage）・usbmuxd 1.1.1・libimobiledevice-utils・usbutils。ディスク使用 5.3GB
- `swift build` で Hello world のコンパイル→実行を確認。`xtool new --skip-setup` は Apple ID 無しで雛形生成可
- **USB/IP のカーネル対応（vhci_hcd + usbip_core）がロード可能なことを実測確認**（懸念だった「カーネル自前ビルド」は不要と確定）

## 重要な技術判断

**Ubuntu 26.04 への Swift 導入は断念し 24.04 を新設**。swift.org に 26.04/25.04 向けビルドが存在せず（404実測）、24.04 版の流用は共有ライブラリ不足が芋づる式（libxml2→libicuuc→libpython3.12…）で、SDKビルドでの再発リスクが高いと判断した。24.04 では不足ゼロ。

## 残作業（K・手順書 k-runbook.md 参照）

1. Xcode 26 の .xip を Apple からDL（Apple IDログイン必要）
2. `xtool auth login`（無料Apple ID・password方式・2FA）
3. `xtool sdk install`（20〜40分待ち）
4. iPad の usbipd 接続（**Plan A**）→ 15〜20分で駄目なら **Plan B**（.ipa 書き出し→Sideloadly）

実作業 25〜35分、待ち込み 1.5〜2.5時間。現在地判定: `wsl -d Ubuntu-24.04 bash /root/xtool-doctor.sh`

## 未検証リスク（優先度順）

1. **最大の懸念: usbipd 経由で iPad が xtool から見えるか**。iOS実機に限って失敗報告が集中（usbipd-win Issue #270/#959/#968/#988）。xtool での成功事例は未発見。唯一のWSL+xtool第三者記事も実機インストールでAltStoreへ迂回 → Plan B を正式ルート候補とする
2. Xcode 要求バージョンにドキュメント内矛盾（本文「Xcode 26」/例示「Xcode_16.3.xip」）。SDKビルド失敗時の手がかり
3. password方式は Apple 非公開API（xtool公式が明記。捨てアカウント検討の言及あり）
4. 無料枠の「7日/10 App ID/3アプリ」は xtool 公式に数値記載なし（一般論からの推測）
5. AltStore 併用時の署名者競合（同一Apple IDだと無料枠を食い合い既存アプリ失効の可能性）
6. usbmuxd は自動起動しない／xtool は TTY 無しだと SwiftNIO 層で落ちる（スクリプト化時の地雷。`script -qec` で回避）

## 検証記録

- Opus 最終検証で子の報告した `xtool dev build --sign` が誤りと判明（1.17.0 に `--sign` は無く `--ipa` のみ）→ 手順書修正済み
- Fable 受入検証: `xtool-doctor.sh` を再実行し、[OK]/[TODO] が報告どおりであることを独立に確認（3層ゲート）

## 実機インストール経過（2026-08-01 深夜・中断中）

- 完了: xtool auth login（トークン2027-08まで）／usbipd経由のiPad列挙（レシピ: detach→usbmuxd先行再起動→attach）／ペアリング（iPad＿cy）／ビルド・署名・プロビジョニング／証明書失効のK承認
- 未完: アプリ転送（`xtool dev` が Connecting 100% 後 opTimeout → 再試行で muxError → WSL E_UNEXPECTED → Windows側スタブ Device in error state）
- 2026-08-01 深夜（続き）: K管理者の unbind→bind --force 実施済み（busid 3-1 に変化）。リセット後の `xtool dev` は**2回とも Connecting 100% → WSL E_UNEXPECTED で失敗**。未署名 `Sidepad.ipa`（158KB）を書き出し `xtool\Sidepad.ipa` へ配置済み（`build --ipa` は署名しない実測）
- 2026-08-02: K管理者で USBPcap 撤去＋再起動 → **症状変わらず**（真因ではなかった）
- **2026-08-03 未明: ✅ 実機インストール成功**。真因＝usbipバルクOUTの大チャンク死（ideviceinstallerの64KB書き込みが0バイトからMUX_ERROR、**4KBチャンクなら通る**）。手順＝xtoolハング中に署名済みipaを横取り（`/root/.cache/xtool/tmp-staging-*/staging/app.ipa`）→ `xtool/chunked_install.py`（pymobiledevice3）で4KBチャンク転送＋installation_proxy Install → InstallComplete
- 2026-08-03: K がデベロッパモードON（iOS16+要件）→ **Sidepad 実機起動確認**。インストール一連を `xtool/install-to-ipad.ps1` に1コマンド化し**実走で再検証済み**（busid自動検出→リンク確立→build-grab-ipa.sh→リンクリセット→chunked_install.py→ideviceinstaller -l 受入検証、約2分）
- 技術詳細の正本: ~/.claude/dev/ios-dev-without-mac.md 追記4・5
