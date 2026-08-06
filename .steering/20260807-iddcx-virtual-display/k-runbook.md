# K実行手順（2026-08-07 準備完了分・タスク1〜3まとめ）

所要: 約10分＋iPad接続。**すべて同じ「管理者PowerShell」ウィンドウでOK。**
開き方: `Win+X` → 「ターミナル（管理者）」 → UACで「はい」

## 手順1: 仮想ディスプレイ(VDD)導入（タスク1）

貼り付け:

```powershell
pwsh -ExecutionPolicy Bypass -File C:\Users\meer0\dev\sidepad-all\sidepad\pc\vdd\install-vdd.ps1
```

- 最後に `=== RESULT ===` が出る。**NG があってもそのまま手順2へ**（回収はくろこ）。
- もし「仮想ディスプレイデバイス未検出」と出たら: スタートメニューから
  **Virtual Driver Control** を起動 → **Install Driver** をクリック → 上のコマンドをもう一度。

## 手順2: iTunes撤去（タスク3。AMDS＝iPad通信サービスは残す）

貼り付け:

```powershell
Start-Process msiexec -ArgumentList '/x','{2A1EC6BB-2EC1-428B-9FE0-A202C3A714EA}','/passive','/norestart' -Wait; Restart-Service 'Apple Mobile Device Service'; Get-Service 'Apple Mobile Device Service'
```

- 進捗バーが1〜2分出る。質問は出ない。最後に `Status: Running` が出ればOK。

## 手順3: iPadをUSB-Cで接続して Sidepad アプリを開く

- ケーブルを挿すだけ（usbipd の操作は不要。Windows側のままでよい）。

## 手順4: くろこに「手順3まで完了」と伝える

以降はくろこが実施する:
1. verify（仮想ディスプレイ出現・2048x1536@120モード・どのアダプタ配下かの実測）
2. usbmux直結の5秒送信テスト＝iTunes撤去後もUSB通信が生きている確認（タスク3の受入）
3. 新ビルド（切断後も最終統計を固定表示する修正入り）のインストール段取り
   → ここで `usbipd bind --force` の1コマンドとケーブル抜き差しだけ追加でお願いする
4. 120Hz E2E 実測（仮想ディスプレイ→エンコード→USB→iPad表示）

## 参考（今日までに検証済みのこと）

- キャプチャ120fpsは成立済み（内蔵パネルを一時的に120Hz化して実測、fps=120.08・欠落0。終了後60Hzへ復帰済み）
- PC側・iPad側ともビルド検証済み。ドライバ導入以外に追加インストールは無い
