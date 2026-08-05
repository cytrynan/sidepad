---
type: steering-result
generated:
  by: kuroko/claude-opus-5
  at: 2026-08-03
updated:
  by: kuroko/claude-fable-5
  at: 2026-08-06
status: done
---

# 最終結果（2026-08-06）: USBデータリンクは成立。アーキテクチャ変更不要

## 結論（ネイティブ usbmux 直結 = 本実装と同じ経路の実測）
| 指標 | 実測値 | 判定 |
|---|---|---|
| 実効スループット上限（全力） | **25〜30 MB/s (209〜249 Mbps)** | H.264/HEVC 40〜80Mbps に対し余裕3倍以上 |
| チャンク耐性 | 4K/16K/64K/256K 全て欠落0 | **usbipの大チャンク死はネイティブでは再現せず** |
| RTT（無負荷） | p50 約1.1ms | |
| 40Mbps時: 片道上振れ p99 | **2.5ms (16Kスライス)** | 転送予算5〜10msに収まる |
| 80Mbps時: 同 | 5.0ms | 〃 |
| 120Mbps時: 同 | 9.7ms・欠落0 | 120Hz向け帯域でも成立 |
| iPad側CPU | 40Mbpsで3〜5% | 無視できる |
| 最大到着ギャップ | 負荷によらず**約15〜30msの単発ヒッカップが常在** | ジッタバッファ/遅延フレーム破棄の設計対象 |

- 設計指針: **フレームは約16KBスライスで送る**（40Mbps時 p99 2.5ms と最良）。深いバッファではなく「遅れフレームは捨てて最新を出す」方式が向く（ギャップは稀・単発のため）
- JSON: `probe-native-direct.json`（直結・確定値）/ `probe-native.json`（forward中継経由・参考）
- 有線LANフォールバックは**不要になった**

## 経路ごとの顛末
1. **WSL+usbip**: 連続ストリーム127KBで死亡（チャンク非依存）→ 開発インストール専用と確定
2. **ネイティブ + pymobiledevice3 forward中継**: 約5MB/s(40Mbps)で頭打ち＝**Python中継が律速**。計測には不適
3. **ネイティブ + usbmux直結（27015へ自前でConnect）**: 上記confirmed値。本実装もこの形にする

## 計測器のバグ3件（すべて修正済み・再発防止コメント入り）
- probe: フレーム全長を 5+length で計算（正: 4+length）→ 2フレーム目以降サイレント破損
- probe: Windows Python 3.10 の `time.monotonic_ns` は15.6ms刻み → `perf_counter_ns` に変更
- LinkStats(iPad): スレッド死でCPU時間が巻き戻ると符号無し減算が桁化け → 0クランプ（**iPad側は次回インストールで反映**）

# 経過記録（2026-08-03 時点・ネイティブ経路の計測前）

## できたもの（すべてビルド通過・実機インストール済み・実走確認済み）
- iPad側: `ProbeProtocol.swift` / `LinkStats.swift` / `USBTransportClient.swift`（TransportClient実装第一版）/ `ConnectionView.swift`（ライブ表示）
- PC側: `pc/probe/usb_link_probe.py`（送信＋計測＋スイープ＋JSON出力）、`run-probe-wsl.sh`、`run-probe-windows.ps1`
- `xtool/install-to-ipad.ps1` に **step 0 = Windows側リポジトリ → /root/sidepad の rsync 同期**を追加
  （従来は同期段が無く、8/1のcloneのままビルドされていた＝Windows側の編集が反映されない事故）

## 実測（WSL+usbip 経路）: 使用不可と確定
- 6条件（全力 4K/16K/64K/256K、40Mbpsペーシング 16K/64K）すべて同一の壊れ方
- **iPad到達 127,131 B / 31パケットで停止**。以降 PC 側 send がブロックし15秒でタイムアウト
- **チャンクサイズ非依存**。4KBでも死ぬ（追記4の「4KBなら通る」はAFCの同期転送に限る話だった）
- PC側 `sendall` は 1.8〜3.0MB 成功していたが、差分はバッファ滞留。**受信側集計が無ければ誤読していた**
- リンク復旧レシピ後、アプリは生存のまま死亡直前の集計を返した ＝ アプリ／iOS 側の問題ではない
- 参考値（到達した31パケット＝4.7ms分のバースト）: 25.9 MB/s、ジッタ0.11ms、片道上振れ p99 4.12ms、iPad CPU 0.05%
  → 区間が短すぎて設計値には使えない。**計測器が端から端まで一周した確認**としてのみ有効

## 経過（2026-08-05〜06）: AMDS導入の迷走と確定ルート
- winget経由のiTunesインストールは **`/passive` だと iTunes64.msi しか入れず AMDS が付かない**と判明
  （クリーン再インストールでも再現。詳細 `~/.claude/dev/ios-dev-without-mac.md` AMDS入手の罠）
- 確定ルート: `winget download` でバンドル取得 → Windows標準 bsdtar で `AppleMobileDeviceSupport64.msi` を抽出（済み）
  → MSI単体を管理者でインストール
- **K貼り付け用スクリプト作成済み（未実行のまま2026-08-06未明時点で待機中）**:
  `<scratchpad>\install-amds-and-probe.ps1`＝AMDS MSI インストール→サービス確認→`run-probe-windows.ps1 -Sweep -Rebind` まで一気通貫。
  ログ `<scratchpad>\fix-amds2.log`、結果 `probe-native.json`（このフォルダ）
  ※ scratchpad はセッション固有のため消えている場合は再作成: ①`winget download --id Apple.iTunes`
  ②`C:\Windows\System32\tar.exe -xf <exe> AppleMobileDeviceSupport64.msi` ③管理者で `msiexec /i <msi> /passive`
- 注意: 昇格起動（Start-Process -Verb RunAs）はくろこ側から実行不可（分類器ブロック）＝管理者作業はK貼り付けが必須

## ★次のタスク（このステアリングは完了）
1. **AMFエンコード実装**（PC側）。設計値が揃った: 16KBスライス・40〜80Mbps・転送予算は実測でp99 5ms以下
2. その次に IddCx 仮想ディスプレイ（60fps超の再測定もここで）
3. iPad側の次回インストール時: LinkStats のCPUクランプ修正を反映（`install-to-ipad.ps1`。要 `usbipd bind` に戻す＝管理者）

## 環境の現状メモ（2026-08-06）
- iPad は **Windows/AMDS側**に居る（usbipd は Not shared）。開発インストールに戻すには管理者で `usbipd bind`
- AMDS(AppleMobileDeviceSupport64.msi 単体) 導入済み・自動起動。iTunes本体も入っている
  （2026-08-06 K承認: 動作が安定したら撤去検証＝iTunes MSIだけアンインストール→AMDS稼働と直結経路の再確認。AMDSは残す）
- Bonjour は今回のバンドルに同梱されず（3ファイル構成）
- Store版「Apple Devices」でAMDSが入るかは未確認のまま（もう不要になったので追わない）
