// ConnectionView.swift
//
// 接続状態（未接続/接続中/受信中）を表示するプレースホルダ画面。
// 現時点では TransportClient と未結線（state はローカルの @State のみ）。
// 後続実装で TransportClient の状態変化を購読し、この state を更新する想定。

import SwiftUI

struct ConnectionView: View {
    @State private var state: ConnectionState = .notConnected

    var body: some View {
        VStack(spacing: 16) {
            Image(systemName: symbolName)
                .imageScale(.large)
                .font(.system(size: 48))
                .foregroundStyle(.tint)

            Text("Sidepad")
                .font(.title2)
                .bold()

            Text(state.displayLabel)
                .font(.headline)
                .foregroundStyle(.secondary)
        }
        .padding()
    }

    private var symbolName: String {
        switch state {
        case .notConnected:
            return "cable.connector.slash"
        case .connecting:
            return "cable.connector"
        case .receiving:
            return "display"
        }
    }
}

#Preview {
    ConnectionView()
}
