# 設計（v1: 常駐運用）

## 方針

- **spikeのコードを捨てずに再構成する**。`pc/src/encode_main.cpp`（キャプチャ→エンコード→送信のメインループ）から、start/stop できる `Pipeline` を抽出し、既存 `pc_encode_spike` CLI は薄いラッパとして**挙動維持**（受入スクリプトの再走行で差分検証）。
- 新規常駐 `sidepadd` は「状態機械＋イベント」の小さなプログラムにする。映像処理は全部 Pipeline に委譲。

## PC側の構成

```
pc/src/
  pipeline.{h,cpp}      … encode_main.cpp から抽出（DXGI+AMF+sender の開始/停止/統計取得）
  encode_main.cpp       … 既存CLI（Pipelineを呼ぶだけに縮小。引数・出力形式は不変）
  daemon_main.cpp       … sidepadd 本体（状態機械）
  usbmux_events.{h,cpp} … usbmux(127.0.0.1:27015) に Listen を張り Attached/Detached を受ける
  vdd_control.{h,cpp}   … 仮想DPの有効/無効（同梱devcon enable/disable、インスタンスID指定）
```

### 状態機械（daemon_main）

```
IDLE --(Attached, ConnectionType=USB)--> VDD_ON --> WAIT_DISPLAY --> STREAMING
STREAMING --(Detached / 送信エラー / エンコーダ失敗)--> TEARDOWN --> IDLE
どの状態でも失敗はバックオフ（1s→2s→…最大10s）付きで IDLE に戻す。遷移は全てログ。
```

- **WAIT_DISPLAY**: VDD有効化後、`--list-displays` 相当の列挙で 2048x1536@120 の仮想DPが出るまでポーリング（100ms間隔・タイムアウト5s）。表示番号ずれ問題はここで吸収
- **接続先**: iPad tcp/7789 へ usbmux Connect（既存 stream_sender の経路。Wi-Fi二重掲載は ConnectionType=USB で選別済みのロジックを流用）
- **スリープ/復帰**: メッセージ専用ウィンドウ＋ `RegisterSuspendResumeNotification`。Suspend で TEARDOWN、Resume で IDLE から再検知
- **スタッツ**: STREAMING 中、60秒周期で fps・累積フレーム・エンコードp99・working set を1行CSVでログ（4時間ソークの証拠になる形）

### 自動起動

- タスクスケジューラ「ログオン時・最上位特権」で `sidepadd.exe` を登録する `pc/daemon/install-task.ps1`（登録/解除/状態確認）。devcon による VDD 有効/無効に管理者が要るため昇格必須

## iPad側の構成（変更は1回にまとめて再インストール）

- `ConnectionState`（待ち受け/受信中）を Renderer の手前に持ち、未接続時は待ち受け画面（アプリ名＋「USBを接続してください」＋直近切断理由）
- NWListener は接続断で listen に戻る（既存の再生成ロジックを流用）。受信再開で自動的に表示へ
- 3本指タップジェスチャで統計オーバーレイをトグル（既定OFF）

## 検証手段（先に渡す）

1. **挙動保存**: Pipeline 抽出後に既存受入スクリプト（20260806/20260807 の受入コマンド）を再走行し、全項目PASSを確認してから daemon に進む
2. **抜き差し試験**: 手動抜き差し20回をログで判定する `pc/daemon/verify-replug.ps1`（各サイクルの Attached→STREAMING 到達時間と成否を集計。物理抜き差しはK作業、判定はスクリプト）
3. **10秒計測**: daemon ログの Attached→初回フレーム送信のタイムスタンプ差で機械判定
4. **4時間ソーク**: スタッツCSVを読み、fps 平均の前半/後半差・working set の傾きで横ばい判定する `pc/daemon/verify-soak.ps1`

## 非スコープ（設計上の割り切り）

- サービス化（SCM）はしない。ログオンセッションで十分（自分用・GPU/デスクトップアクセスも素直）
- 設定は当面ハードコード定数（configファイル化はv1.5）
- devcon が使えない環境の考慮はしない（同梱済み前提）
