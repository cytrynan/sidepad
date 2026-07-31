# K 作業手順書 — xtool で iPad にアプリを入れるまで

最終更新: 2026-08-01 / 作成: くろこ

---

## この手順書の前提

くろこ側で**ここまでは完了済み**です。K が触る必要はありません。

- ✅ WSL に新しいディストリビューション **`Ubuntu-24.04`** を作成（既存の `Ubuntu` は無変更）
- ✅ Swift 6.3.3 導入・動作確認済み
- ✅ xtool 1.17.0 導入・動作確認済み
- ✅ usbmuxd / libimobiledevice / usbutils 導入済み
- ✅ USB/IP のカーネル対応（vhci_hcd）がロードできることを実測確認済み

**K の残作業は STEP 1〜6 です。** 実際に手を動かす時間は合計 **25〜35 分**、待ち時間（ダウンロード・SDK ビルド）を含めた所要時間は **1.5〜2.5 時間**です。待ち時間は放置で構いません。

**いま自分がどこまで進んだか分からなくなったら**、下のコマンドを叩けば現在地が出ます。困ったら常にこれ。

```powershell
wsl -d Ubuntu-24.04 bash /root/xtool-doctor.sh
```

`[TODO]` と表示された最初の項目が、次にやるべきことです。

---

## STEP 0: 使う Apple ID を先に決める（所要 1 分・最初に1回だけ）

STEP 1 に進む前に、**xtool のログイン（STEP 3）に使う Apple ID を決めてください**。後から変えると署名やアプリの入れ直しが発生するので、ここで1回だけ決めます。

- **推奨: 普段の Apple ID で進める**（いちばん摩擦が少ない）
- ただし次のどちらかに当てはまるなら、**xtool 専用の無料 Apple ID を新規作成**する方が安全:
  1. xtool の Password ログインが Apple の**非公開 API** を使うことに抵抗がある（xtool 公式自身が「心配なら捨てアカウントも検討してよい」と書いている）
  2. **AltStore を本格常用する予定**がある（同じ ID だと無料枠〈同時3アプリ・App ID 週10個〉を食い合い、既存アプリが失効することがある）
- どちらを選んでも、**STEP 1 の Xcode ダウンロードは普段の Apple ID でよい**（DL とxtoolログインの ID は別々で動きます）

以降の STEP 3 の「Apple ID」は、ここで決めた ID を指します。

---

## STEP 1: Xcode 26 の .xip をダウンロードする（所要 5 分 + 待ち 20〜60 分）

xtool は iOS の SDK を Xcode の中から取り出して使います。そのため Xcode の `.xip` が1つ必要です。Apple のサイトはログインが要るので、ここは K にしかできません。

1. ブラウザで次の URL を開く
   https://developer.apple.com/download/all/?q=Xcode
2. Apple ID でサインインを求められるので、**普段使っている Apple ID でサインイン**する
   （※ xtool のログイン（STEP 3）は STEP 0 で決めた ID を使います。ここと同じでなくても OK）
3. 2ファクタ認証のコードを iPhone / iPad で承認する
4. 「Sign in」後、Xcode のリリース一覧が出る。検索ボックスに既に `Xcode` が入っている状態
5. 一覧の中から **`Xcode 26`** 系で**いちばん上（＝最新）のもの**を探す
   - `Xcode 26.x` のような表記。`Release Candidate` や `beta` と付いていないものを選ぶ
6. その行の左にある **▶（三角）をクリックして展開**する
7. 中に出てくる **`Xcode_26.x.xip`** という**リンクをクリック**するとダウンロードが始まる
   - ファイルサイズは **10〜15 GB 程度**あります。回線次第で 20〜60 分かかります
   - 保存先はブラウザ既定のままで OK（＝ `C:\Users\meer0\Downloads\`）

> **注意**: `.dmg` ではなく **`.xip`** を取ってください。`Command Line Tools` ではなく **Xcode 本体**です。

ダウンロードが終わったらファイル名を確認しておいてください（STEP 2 で使います）。

```powershell
Get-ChildItem "$env:USERPROFILE\Downloads\Xcode*.xip" | Select-Object Name, @{n='GB';e={[math]::Round($_.Length/1GB,2)}}
```

---

## STEP 2: .xip を WSL の中にコピーする（所要 1 分 + 待ち 5〜10 分）

`/mnt/c` 越しに展開すると非常に遅いので、先に Linux 側のディスクへ移します。

PowerShell を開いて、**下をそのまま貼り付け**てください（ファイル名は自動で拾います）。

```powershell
$xip = Get-ChildItem "$env:USERPROFILE\Downloads\Xcode*.xip" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Write-Output "コピー元: $($xip.FullName)"
wsl -d Ubuntu-24.04 bash -lc "cp '/mnt/c/Users/meer0/Downloads/$($xip.Name)' /root/Xcode.xip && ls -lh /root/Xcode.xip"
```

`/root/Xcode.xip` のサイズが表示されれば成功です。

---

## STEP 3: xtool に Apple ID でログインする（所要 3 分）

ここが2つ目の「K にしかできない」作業です。

1. PowerShell で WSL に入る

```powershell
wsl -d Ubuntu-24.04
```

2. プロンプトが `root@...:~#` に変わったら、次を実行

```bash
xtool auth login
```

3. **1つめの質問** — こう聞かれます:

```
Select login mode
0: API Key (requires paid Apple Developer Program membership)
1: Password (works with any Apple ID but uses private APIs)
Choice (0-1):
```

→ **`1` と入力して Enter**（無料 Apple ID なので必ず 1 です。0 は年間 $99 の有料プログラム向け）

4. **2つめ** — Apple ID のメールアドレスを聞かれるので入力して Enter

5. **3つめ** — パスワードを聞かれるので入力して Enter（画面には表示されません）

6. **4つめ** — 2ファクタ認証コードを聞かれます。K の iPhone / iPad に6桁のコードが出るので、それを入力して Enter

7. 成功したか確認:

```bash
xtool auth status
```

→ `Logged out` 以外（ログイン済みの表示）になれば OK

> **補足**: ここで入れるのは **STEP 0 で決めた Apple ID** です（Password 方式が非公開 API を使う件・AltStore との枠食い合いの判断は STEP 0 に集約済み）。

---

## STEP 4: Darwin SDK をビルドする（所要 1 分 + 待ち 20〜40 分）

STEP 3 と同じ WSL の中で、続けて実行します。

```bash
xtool sdk install /root/Xcode.xip
```

`.xip` の展開と SDK の構築が走ります。**20〜40 分ほど無反応に見えることがありますが、止まっていません。**放置してください。

終わったら確認:

```bash
xtool sdk status
swift sdk list
```

→ `Not installed` ではなくなり、`swift sdk list` に `darwin` が出れば成功です。

ここまで来たら **.xip はもう不要**なので消して構いません（10〜15 GB 空きます）:

```bash
rm -f /root/Xcode.xip
```

### ビルドできるか試す

```bash
cd /root
xtool new MyFirstApp --skip-setup
cd MyFirstApp
xtool dev build
```

エラーなく終われば、**「iOS アプリをビルドできる」というゴールの半分は達成**です。

> もし `error: No valid Swift SDK bundles found` が出たら、STEP 4 の SDK インストールが失敗しています。`xtool sdk status` を見直してください。

---

## STEP 5: iPad を WSL に繋ぐ（所要 15 分・**ここが一番失敗しやすい**）

> **先に読んでください。** 事前調査の結果、**この STEP 5 は失敗する可能性が相応にあります**（iOS 実機を WSL に通す手法には未解決の不具合報告が複数あり、xtool での成功事例が確認できていません）。
> **15〜20 分試して駄目なら、STEP 5 は諦めて STEP 6（Plan B）に進んでください。** Plan B の方が確実です。時間を溶かさないでください。

### 5-1. usbipd-win を入れる（管理者権限が必要）

1. スタートメニューで `PowerShell` と検索
2. 「Windows PowerShell」を**右クリック** → **「管理者として実行」**
3. 「このアプリがデバイスに変更を加えることを許可しますか?」→ **「はい」**
4. 青い管理者 PowerShell が開いたら、次を貼り付けて Enter

```powershell
winget install --interactive --exact dorssel.usbipd-win
```

5. インストーラの画面が出たら **Next → Install → Finish**
6. **この管理者 PowerShell は閉じずに置いておく**（5-2 で使います）

### 5-2. iPad を繋いで bind する

1. **iPad を USB-C ケーブルで PC に繋ぐ**
   - **データ転送対応のケーブル**を使ってください。充電専用ケーブルだと認識しません
2. iPad の画面に「**このコンピュータを信頼しますか?**」が出たら → **「信頼」** → **パスコードを入力**
   - 出ない場合は一度ケーブルを抜き差し
3. 管理者 PowerShell で接続一覧を出す

```powershell
usbipd list
```

4. 出力の中から **`Apple Mobile Device` / `iPad` / `Apple Inc.`** と書かれた行を探し、いちばん左の **BUSID**（`2-3` のような数字）をメモする
5. その BUSID を使って bind（**`2-3` の部分は自分の値に置き換える**）

```powershell
usbipd bind --busid 2-3 --force
```

6. WSL に繋ぐ（**この attach は管理者でなくても動きますが、そのまま管理者で続けて構いません**）

```powershell
usbipd attach --wsl --busid 2-3
```

> `Device busy` / `Device in error state` と出たら: ケーブルを抜き差ししてから 5 と 6 をもう一度。3 回試して駄目なら STEP 6 へ。

### 5-3. WSL 側で見えているか確認

**別の（管理者でない）PowerShell** を開いて:

```powershell
wsl -d Ubuntu-24.04 bash /root/xtool-doctor.sh
```

- `iOS device seen: <UDID>` と出れば **成功**。5-4 へ
- `no iOS device` のままなら → **STEP 6（Plan B）へ**

> `lsusb` にルートハブしか出ない、という状態がまさに調査で報告されていた典型的な失敗です。粘っても直らないことが多いので、素直に Plan B に降りてください。

### 5-4. アプリを実機にインストールする

```powershell
wsl -d Ubuntu-24.04
```

WSL に入ったら:

```bash
usbmuxd          # 起動していなければ。既に動いていればエラーが出るが無視してよい
cd /root/MyFirstApp
xtool dev
```

ビルド → 署名 → インストールまで自動で走ります。

### 5-5. iPad 側で開発者を信頼する（初回のみ必須）

インストール直後、アプリを叩いても「信頼されていないデベロッパ」と出て起動しません。iPad 側で1回だけ許可します。

1. iPad で **「設定」** アプリを開く
2. **「一般」** をタップ
3. 下の方の **「VPN とデバイス管理」** をタップ
4. 「デベロッパ App」の下に **K の Apple ID のメールアドレス**が出ているのでタップ
5. **「"<メールアドレス>" を信頼」** をタップ
6. 確認ダイアログで **「信頼」** をタップ

これでホーム画面のアプリが起動できます。**完了です。**

---

## STEP 6: Plan B — .ipa を作って Sideloadly で入れる（STEP 5 が駄目だったとき）

STEP 5 が通らなくても、**ビルドは WSL、インストールは Windows** と分担すれば目的は達成できます。事前調査では、WSL + xtool を試した人も最終的にこの方法に落ち着いていました。

### 6-1. .ipa を書き出す

```powershell
wsl -d Ubuntu-24.04
```

```bash
cd /root/MyFirstApp
xtool dev build --ipa
find /root/MyFirstApp -name '*.ipa'
```

出てきたパス（`/root/MyFirstApp/.build/....ipa` のような形）をメモします。

### 6-2. .ipa を Windows 側に取り出す

```bash
cp $(find /root/MyFirstApp -name '*.ipa' | head -1) /mnt/c/Users/meer0/Downloads/
exit
```

`C:\Users\meer0\Downloads\` に `.ipa` が出ます。

### 6-3. Sideloadly で iPad に入れる

> **注意**: Sideloadly は Apple のデバイスドライバを必要とします。この PC には現在 iTunes / Apple Devices が入っていないため、**追加インストールが必要**です。

1. https://sideloadly.io/ を開き、**Windows 版**をダウンロードしてインストール
   - 導入時に Apple のドライバ（iTunes もしくは Apple Devices）を求められたら、案内に従って入れる
2. iPad を USB-C で接続し、「このコンピュータを信頼」→「信頼」→ パスコード
3. Sideloadly を起動する
4. 上部の **「iDevice」** のドロップダウンに iPad が出ていることを確認
5. **「IPA」欄の右のフォルダアイコン**をクリック → `C:\Users\meer0\Downloads\` の `.ipa` を選ぶ
6. **「Apple ID」欄**に K の Apple ID（メールアドレス）を入力
7. 右下の **「Start」** をクリック
8. パスワードを聞かれるので入力 → 2ファクタ認証コードを入力
9. 進捗バーが最後まで進み **「Done」** になれば完了
10. **STEP 5-5 と同じ手順で iPad 側の「信頼」を行う**（設定 → 一般 → VPN とデバイス管理 → 信頼）

> **AltStore を既に使っている場合の注意**: xtool と AltStore で**同じ Apple ID を使うと、無料枠（App ID 数・証明書）を食い合って既存アプリが失効することがあります**。AltStore を常用しているなら、xtool 側は別の Apple ID にするのが安全です。

---

## 覚えておくべき制約（無料 Apple ID）

- **署名は 7 日で切れます。** 7 日ごとに入れ直し（`xtool dev` を再実行、または Sideloadly で再インストール）が必要です
- **同時にインストールできるのは 3 アプリまで**、**App ID は 7 日で 10 個まで**
- bundle ID には xtool が自動で接頭辞を付けます（`com.example.MyFirstApp` → `XTL-1234.com.example.MyFirstApp`）。無料アカウント同士の衝突を避けるためで、正常な挙動です
- Network Extension など一部の機能（entitlement）は無料アカウントでは使えません

> ※ 上の「7日」「10個」「3アプリ」は Apple の無料枠一般の制約で、**xtool のドキュメントに明記されている数値ではありません**（xtool 公式は「制約がある」とだけ書いています）。実際の挙動が違ったらそちらが正です。

---

## よく使うコマンド早見

```powershell
# 現在地の確認（迷ったらまずこれ）
wsl -d Ubuntu-24.04 bash /root/xtool-doctor.sh

# WSL に入る
wsl -d Ubuntu-24.04

# iPad を WSL に繋ぎ直す（管理者 PowerShell）
usbipd list
usbipd attach --wsl --busid <BUSID>
```

```bash
# WSL の中で
xtool auth status          # ログイン状態
xtool sdk status           # SDK の状態
xtool devices --no-wait    # 見えているデバイス
usbmuxd                    # デバイス通信デーモンを起動（自動起動しないので都度必要）
cd /root/MyFirstApp && xtool dev   # ビルド→署名→実機インストール
```

---

## トラブル時の対応表

| 症状 | 対処 |
|------|------|
| `error: No valid Swift SDK bundles found` | STEP 4 の SDK インストールが未完了。`xtool sdk status` を確認 |
| `xtool devices` が何も出さない | `usbmuxd` を起動していない。WSL 内で `usbmuxd` を実行 |
| `usbipd bind` で `Device busy` | ケーブル抜き差し → `usbipd bind --busid X-Y --force` をやり直し。3回で駄目なら Plan B |
| WSL の `lsusb` にルートハブしか出ない | 調査で報告されている典型的な失敗。粘らず Plan B へ |
| iPad で「信頼されていないデベロッパ」 | STEP 5-5 の信頼操作を実施 |
| 数日後にアプリが起動しなくなった | 無料署名の 7 日失効。`xtool dev` で入れ直し |
| xtool がスクリプト実行時に `epoll_ctl ... Operation not permitted` で落ちる | 端末（TTY）が無いのが原因。手で WSL に入って実行するか `script -qec "xtool ..." /dev/null` を使う |
