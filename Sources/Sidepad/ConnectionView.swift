// ConnectionView.swift
//
// 映像表示（全画面）＋接続状態・リンク/デコード統計のオーバーレイ。
// USBTransportClient を起動して PC からの接続を待ち受け、受信フレームを
// VideoPipeline（デコード）→ LatestFrameStore → MetalFrameRenderer（描画）に流す。
// 実機ではコンソールが見えないので、動作ログもこの画面に出す。
// 統計オーバーレイは3本指タップで表示/非表示を切り替える（受信中の既定はOFF）。
// 切断後は直前セッションの最終統計をトグルに関係なく固定表示する（受入検証で数値を読むため。
// 20260807-stats-overlay-freeze のK指摘対応を既定OFF化後も維持する）。
// 未接続時は待ち受け画面（接続案内＋直近の切断理由）を出し、
// listener 常駐により PC の再接続で自動的に表示へ戻る。
// 切断後も直前セッションの最終統計を固定表示する（受入検証で数値を読むため）。
// 切断時に VideoPipeline.flushStats() で最終値まで発行してから凍結し、
// 次接続の RESET でゼロが発行されて固定表示は消える。

import Combine
import SwiftUI
import UIKit

@MainActor
final class LinkViewModel: ObservableObject {
    @Published private(set) var state: ConnectionState = .notConnected
    @Published private(set) var snapshot = LinkSnapshot()
    @Published private(set) var pipelineStats = PipelineStats()
    @Published private(set) var logs: [String] = []
    /// 直近の切断理由（時刻付き）。一度も切断していなければ nil。
    @Published private(set) var lastDisconnectReason: String?

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
                // 残りのデコードを吐き切ってから最終統計を発行（固定表示用）
                pipeline.flushStats()
            }
            Task { @MainActor in self?.state = state }
        }
        transport.onSnapshot = { [weak self] snapshot in
            Task { @MainActor in self?.snapshot = snapshot }
        }
        transport.onLog = { [weak self] message in
            Task { @MainActor in self?.append(message) }
        }
        transport.onDisconnect = { [weak self] reason in
            let at = Date()  // 時刻の確定だけ transport キュー側で行い、整形は MainActor 内で
            Task { @MainActor in
                self?.lastDisconnectReason = "\(Self.timeFormatter.string(from: at)) \(reason)"
            }
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

    /// TouchCatcher からのコールバックで呼ぶ。待ち受け画面での誤送信防止のため
    /// 受信中(.receiving)のときだけ transport へ委譲する（design.md §2-3）。
    func sendTouch(_ event: TouchEvent) {
        guard state == .receiving else { return }
        transport.sendTouch(event)
    }

    private func append(_ message: String) {
        logs.append(message)
        if logs.count > 12 { logs.removeFirst(logs.count - 12) }
    }

    private static let timeFormatter: DateFormatter = {
        let formatter = DateFormatter()
        // locale 固定必須: 未指定だと端末の12/24時間設定で HH が再解釈され（QA1480）、
        // PC側 daemon ログとの時刻突き合わせ（受入の10秒計測）が壊れる
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "HH:mm:ss"
        return formatter
    }()
}

struct ConnectionView: View {
    @StateObject private var model = LinkViewModel()
    /// 統計オーバーレイの表示状態。既定OFF（3本指タップでトグル）。
    @State private var showOverlay = false

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            if let renderer = model.renderer {
                VideoRenderView(renderer: renderer)
                    .ignoresSafeArea()
            }

            if model.state != .receiving {
                waitingView
                // 最終統計はトグルに関係なく自動表示（受入で数値を読むため）。
                // 統計が無くても、トグルONならゼロ値オーバーレイを出して
                // 「3本指タップが効いている」ことを目視確認できるようにする
                if hasSessionStats || showOverlay {
                    statsOverlay
                }
            } else if showOverlay {
                statsOverlay
            }

            // タッチ捕捉層（最前面・透明・全面）。ZStackの兄弟ビューはhitTestで前面が
            // 全タッチを取ってしまい背面にはジェスチャが届かないため（2026-08-11 レビュー必須1）、
            // 1本指タップ/ドラッグ・2本指ドラッグ・3本指タップ(旧ThreeFingerTapCatcher)を
            // すべて同一ビューの recognizer に統合する。design.md §2-3
            TouchCatcher(onEvent: { model.sendTouch($0) }, onToggleOverlay: { showOverlay.toggle() })
                .ignoresSafeArea()
        }
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

            // .secondary はライトモード端末では黒系に解決され黒背景で読めなくなるため明示指定
            Text("USBケーブルでPCに接続してください")
                .font(.subheadline)
                .foregroundStyle(.white.opacity(0.75))

            if let reason = model.lastDisconnectReason {
                Text("直近の切断: \(reason)")
                    .font(.system(size: 12, design: .monospaced))
                    .foregroundStyle(.orange)
            }

            logsView
        }
        .padding()
    }

    // MARK: - 受信中の統計オーバーレイ（タップで表示/非表示）

    /// 直前セッションの統計が残っているか（切断後の固定表示に使う）。
    private var hasSessionStats: Bool {
        model.pipelineStats.decode.decodedFrames > 0 || model.snapshot.packets > 0
    }

    private var statsOverlay: some View {
        VStack(alignment: .leading, spacing: 12) {
            if model.state != .receiving, hasSessionStats {
                Text("最終統計（切断済み）")
                    .font(.system(size: 12, weight: .semibold, design: .monospaced))
                    .foregroundStyle(.orange)
            }
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

            // 切断中は waitingView 側がログを出すので二重表示を避ける
            if model.state == .receiving {
                logsView
            }

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
