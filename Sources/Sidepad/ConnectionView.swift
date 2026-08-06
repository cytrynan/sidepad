// ConnectionView.swift
//
// 映像表示（全画面）＋接続状態・リンク/デコード統計のオーバーレイ。
// USBTransportClient を起動して PC からの接続を待ち受け、受信フレームを
// VideoPipeline（デコード）→ LatestFrameStore → MetalFrameRenderer（描画）に流す。
// 実機ではコンソールが見えないので、動作ログもこの画面に出す。
// 統計オーバーレイは画面タップで表示/非表示を切り替える。

import Combine
import SwiftUI
import UIKit

@MainActor
final class LinkViewModel: ObservableObject {
    @Published private(set) var state: ConnectionState = .notConnected
    @Published private(set) var snapshot = LinkSnapshot()
    @Published private(set) var pipelineStats = PipelineStats()
    @Published private(set) var logs: [String] = []

    private let transport = USBTransportClient()
    private let pipeline = VideoPipeline()
    /// Metal 初期化失敗時は nil（画面には失敗ログが出る）。
    let renderer: MetalFrameRenderer?
    private var started = false

    init() {
        var initLogs: [String] = []
        renderer = MetalFrameRenderer(store: pipeline.frameStore) { message in
            initLogs.append(message)
        }
        logs = initLogs
    }

    func start() {
        guard !started else { return }
        started = true

        transport.onStateChange = { [weak self, pipeline] state in
            if state != .receiving {
                // 切断＝ストリームの区切り。次の接続は SPS/PPS から来る前提でセッションを畳む。
                pipeline.reset()
            }
            Task { @MainActor in self?.state = state }
        }
        transport.onSnapshot = { [weak self] snapshot in
            Task { @MainActor in self?.snapshot = snapshot }
        }
        transport.onLog = { [weak self] message in
            Task { @MainActor in self?.append(message) }
        }
        transport.onFrameData = { [pipeline] data in
            pipeline.ingest(data)
        }
        transport.onMeasurementReset = { [pipeline] in
            pipeline.resetStats()  // LinkStats と同時にゼロへ（受入照合の前提）
        }
        pipeline.onStats = { [weak self] stats in
            Task { @MainActor in self?.pipelineStats = stats }
        }
        pipeline.onLog = { [weak self] message in
            Task { @MainActor in self?.append(message) }
        }
        transport.start()
        UIApplication.shared.isIdleTimerDisabled = true  // 受信中に画面が消えると表示も止まる
    }

    private func append(_ message: String) {
        logs.append(message)
        if logs.count > 12 { logs.removeFirst(logs.count - 12) }
    }
}

struct ConnectionView: View {
    @StateObject private var model = LinkViewModel()
    @State private var showOverlay = true

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            if let renderer = model.renderer {
                VideoRenderView(renderer: renderer)
                    .ignoresSafeArea()
            }

            if model.state != .receiving {
                waitingView
            } else if showOverlay {
                statsOverlay
            }
        }
        .contentShape(Rectangle())
        .onTapGesture { showOverlay.toggle() }
        .statusBarHidden()
        .onAppear { model.start() }
    }

    // MARK: - 接続待ち画面（従来の表示）

    private var waitingView: some View {
        VStack(spacing: 20) {
            Image(systemName: symbolName)
                .imageScale(.large)
                .font(.system(size: 44))
                .foregroundStyle(.tint)

            Text("Sidepad")
                .font(.title2)
                .bold()
                .foregroundStyle(.white)

            Text(model.state.displayLabel)
                .font(.headline)
                .foregroundStyle(.secondary)

            logsView
        }
        .padding()
    }

    // MARK: - 受信中の統計オーバーレイ（タップで表示/非表示）

    private var statsOverlay: some View {
        VStack(alignment: .leading, spacing: 12) {
            Grid(alignment: .leading, horizontalSpacing: 24, verticalSpacing: 4) {
                metricRow("デコードfps", String(format: "%.1f", model.pipelineStats.decodedFPS))
                metricRow("デコード済", "\(model.pipelineStats.decode.decodedFrames)")
                metricRow("描画済", "\(model.pipelineStats.renderedFrames)")
                metricRow("デコード失敗", "\(model.pipelineStats.decode.decodeErrors)")
                metricRow("SPS未着破棄", "\(model.pipelineStats.decode.droppedPreSPS)")
                metricRow("decode p50/p99/max", String(
                    format: "%.1f / %.1f / %.1f ms",
                    model.pipelineStats.decode.decodeP50Ms, model.pipelineStats.decode.decodeP99Ms,
                    model.pipelineStats.decode.decodeMaxMs))
                metricRow("recv→done p50/p99/max", String(
                    format: "%.1f / %.1f / %.1f ms",
                    model.pipelineStats.recvToDoneP50Ms, model.pipelineStats.recvToDoneP99Ms,
                    model.pipelineStats.recvToDoneMaxMs))
                metricRow("待ち行列", "\(model.pipelineStats.queueDepth)")
                metricRow("解像度", "\(model.pipelineStats.decode.width)x\(model.pipelineStats.decode.height)")
                Divider().gridCellUnsizedAxes(.horizontal)
                metricRow("スループット", String(format: "%.1f MB/s", model.snapshot.throughputMBps))
                metricRow("受信パケット", "\(model.snapshot.packets)")
                metricRow("ジッタ", String(format: "%.2f ms", model.snapshot.interarrivalJitterMs))
                metricRow("最大到着間隔", String(format: "%.2f ms", model.snapshot.maxArrivalGapMs))
            }
            .font(.system(size: 13, design: .monospaced))
            .padding(12)
            .background(.black.opacity(0.55), in: RoundedRectangle(cornerRadius: 12))

            logsView

            Spacer()
        }
        .foregroundStyle(.white)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .padding()
    }

    private var logsView: some View {
        VStack(alignment: .leading, spacing: 2) {
            ForEach(Array(model.logs.enumerated()), id: \.offset) { _, line in
                Text(line)
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
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
