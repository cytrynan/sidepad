// SidepadApp.swift
//
// アプリのエントリポイント。
// 現時点では ConnectionView（接続状態のプレースホルダ画面）を表示するだけの骨格。
// 今後、起動時の TransportClient 初期化や設定読み込みなどをここに追加していく想定。

import SwiftUI

@main
struct SidepadApp: App {
    var body: some Scene {
        WindowGroup {
            ConnectionView()
        }
    }
}
