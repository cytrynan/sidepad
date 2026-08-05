// ConnectionView.swift
//
// 接続状態とリンク実測値を表示する画面。
// USBTransportClient を起動して PC からの接続を待ち受け、受信中の統計をライブ表示する。
// 実機ではコンソールが見えないので、動作ログもこの画面に出す。

import Combine
import SwiftUI
import UIKit

@MainActor
final class LinkViewModel: ObservableObject {
    @Published private(set) var state: ConnectionState = .notConnected
    @Published private(set) var snapshot = LinkSnapshot()
    @Published private(set) var logs: [String] = []

    private let transport = USBTransportClient()
    private var started = false

    func start() {
        guard !started else { return }
        started = true

        transport.onStateChange = { [weak self] state in
            Task { @MainActor in self?.state = state }
        }
        transport.onSnapshot = { [weak self] snapshot in
            Task { @MainActor in self?.snapshot = snapshot }
        }
        transport.onLog = { [weak self] message in
            Task { @MainActor in self?.append(message) }
        }
        transport.start()
        UIApplication.shared.isIdleTimerDisabled = true  // 計測中に画面が消えると受信も止まる
    }

    private func append(_ message: String) {
        logs.append(message)
        if logs.count > 12 { logs.removeFirst(logs.count - 12) }
    }
}

struct ConnectionView: View {
    @StateObject private var model = LinkViewModel()

    var body: some View {
        VStack(spacing: 20) {
            Image(systemName: symbolName)
                .imageScale(.large)
                .font(.system(size: 44))
                .foregroundStyle(.tint)

            Text("Sidepad")
                .font(.title2)
                .bold()

            Text(model.state.displayLabel)
                .font(.headline)
                .foregroundStyle(.secondary)

            Grid(alignment: .leading, horizontalSpacing: 24, verticalSpacing: 6) {
                metricRow("スループット", String(format: "%.1f MB/s", model.snapshot.throughputMBps))
                metricRow("受信パケット", "\(model.snapshot.packets)")
                metricRow("受信バイト", String(format: "%.1f MB", Double(model.snapshot.bytes) / 1_048_576))
                metricRow("ジッタ", String(format: "%.2f ms", model.snapshot.interarrivalJitterMs))
                metricRow("最大到着間隔", String(format: "%.2f ms", model.snapshot.maxArrivalGapMs))
            }
            .font(.system(.body, design: .monospaced))
            .padding()
            .background(.quaternary, in: RoundedRectangle(cornerRadius: 12))

            VStack(alignment: .leading, spacing: 2) {
                ForEach(Array(model.logs.enumerated()), id: \.offset) { _, line in
                    Text(line)
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            Spacer()
        }
        .padding()
        .onAppear { model.start() }
    }

    private func metricRow(_ label: String, _ value: String) -> some View {
        GridRow {
            Text(label).foregroundStyle(.secondary)
            Text(value).bold()
        }
    }

    private var symbolName: String {
        switch model.state {
        case .notConnected:
            return "cable.connector.slash"
        case .connecting:
            return "cable.connector"
        case .receiving:
            return "display"
        }
    }
}

// NOTE: #Preview マクロは使用しない。
// xtool 環境では Apple 独自マクロ (PreviewsMacros) のプラグインが無く
// ビルドエラーになる（2026-08-01 実測）。プレビューは実機確認で代替する。
