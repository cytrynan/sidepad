# brief: 切断後も最終統計を固定表示（T1）

- 背景: 2026-08-07 のデコード表示受入で、PC側送信終了＝切断と同時に統計オーバーレイが
  waitingView に置き換わり、最終値（3600/3600等）を読み取れず不便だった（K指摘の積み残し①）。
- 変更: ConnectionView.swift のみ。切断中も直前セッションの統計が残っていれば
  statsOverlay を表示（「最終統計（切断済み）」ヘッダ付き・タップ表示切替は従来通り）。
  ログは waitingView 側と二重になるため受信中のみ overlay に出す。
- 根拠: 統計値は ViewModel に残存し、次接続時の RESET メッセージで初めてゼロに戻る
  （VideoPipeline.resetStats / LinkStats）。切断時の pipeline.reset() はデコーダのみ畳む。
- 検証: WSL xtool ビルドが通ること（実機受入は次回インストール時、120Hz E2E と同時に実施）。
- 実装者の仕様級判断: 「残存統計あり」判定は decodedFrames>0 または packets>0。
  接続前（統計ゼロ）は従来通り waitingView のみ。
