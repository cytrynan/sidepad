// VideoPipeline.swift
//
// 受信AU → デコード → LatestFrameStore の配線と統計集計。
//
// スレッド設計（design.md）:
// - transport キューではデコードしない（受信ループを塞ぐと LinkStats の到着統計が汚れる）。
//   ingest() はタイムスタンプ取得とキュー投入だけして即返る。
// - decode キューは無界の直列キュー。HWデコードは数ms/枚で60fpsに余裕があるが、
//   健全性監視として待ち行列深さ (queueDepth) を統計に出す（伸び続けたらデコード律速）。
// - エンコード済みフレームは1枚も捨てない（全フレームがP参照。捨てるのは表示段=LatestFrameStore）。

import CoreVideo
import Foundation

/// UI 表示用のパイプライン統計。
struct PipelineStats: Sendable {
    var decode = DecodeStats()
    /// 受信(ingest)→デコード完了の壁時計 (ms)。キュー待ちを含む（受入基準 p99 ≤ 10ms）。
    var recvToDoneP50Ms = 0.0
    var recvToDoneP99Ms = 0.0
    var recvToDoneMaxMs = 0.0
    /// いまデコード待ちのフレーム数。
    var queueDepth = 0
    /// 新規フレームとして画面描画された通算数。
    var renderedFrames = 0
    /// 直近区間のデコードfps。
    var decodedFPS = 0.0
}

final class VideoPipeline: @unchecked Sendable {
    private let queue = DispatchQueue(label: "com.sidepad.decode", qos: .userInteractive)
    private let decoder = VTVideoDecoder()
    let frameStore = LatestFrameStore()

    private let lock = NSLock()
    private var enqueued = 0

    // 以下は decode キュー上でのみ触る
    private var done = 0
    // 受入走行(20秒×60fps)が丸ごと収まる窓（レビュー指摘M2）
    private var recvToDoneRing = [Double](repeating: 0, count: 2048)
    private var recvToDoneCount = 0
    private var recvToDoneMaxMs = 0.0
    private var lastEmitNs: UInt64 = 0
    private var lastEmitDecoded = 0

    /// decode キュー上で約4Hzで呼ばれる。
    var onStats: ((PipelineStats) -> Void)?
    var onLog: ((String) -> Void)? {
        didSet { decoder.onLog = onLog }
    }

    /// transport キューから呼ばれる。即返る。
    func ingest(_ frameData: Data) {
        let recvNs = monotonicNanoseconds()
        lock.lock()
        enqueued += 1
        lock.unlock()

        queue.async { [self] in
            decoder.decode(frameData) { [frameStore] pixelBuffer in
                if let pixelBuffer {
                    frameStore.render(pixelBuffer)
                }
            }
            done += 1
            let ms = Double(monotonicNanoseconds() &- recvNs) / 1_000_000
            recvToDoneRing[recvToDoneCount % recvToDoneRing.count] = ms
            recvToDoneCount += 1
            if ms > recvToDoneMaxMs { recvToDoneMaxMs = ms }
            emitStatsIfNeeded()
        }
    }

    /// 切断時に呼ぶ（次の接続は新しいストリーム＝SPS/PPSから来る前提でセッションを畳む）。
    func reset() {
        queue.async { [self] in
            decoder.reset()
        }
    }

    /// 切断時に呼ぶ: デコード待ちを吐き切った後（直列キューなので enqueue 済みの
    /// decode がすべて先に走る）、スロットルを無視して現在値を発行する。
    /// これが無いと切断後の固定表示が最後の〜250ms分（120Hzで最大約30フレーム）
    /// 欠けた値で凍り、「decodedFrames == 受信フレーム数」の受入照合が偽欠落になる。
    func flushStats() {
        queue.async { [self] in
            emitStatsIfNeeded(force: true)
        }
    }

    /// PC側の計測リセット（RESETメッセージ）に同期して統計をゼロに戻す。
    /// LinkStats（受信パケット数）が 0 に戻るのに合わせないと
    /// 「decodedFrames == 受信フレーム数」の受入照合が2回目の走行から崩れる（レビュー指摘M3）。
    func resetStats() {
        lock.lock()
        enqueued = 0
        lock.unlock()
        frameStore.resetRenderedCount()
        queue.async { [self] in
            decoder.resetCounters()
            done = 0
            recvToDoneCount = 0
            recvToDoneMaxMs = 0
            lastEmitNs = 0
            lastEmitDecoded = 0
            // ゼロを即時発行する。これが無いと @Published 側に前セッションの値が残り、
            // 新走行が1フレームもデコードせずに終わったとき前回の最終統計を
            // 今回の結果として読んでしまう（レビュー指摘M2）。
            emitStatsIfNeeded(force: true)
        }
    }

    // MARK: - 統計（decode キュー上）

    private func emitStatsIfNeeded(force: Bool = false) {
        guard let onStats else { return }
        let now = monotonicNanoseconds()
        guard force || lastEmitNs == 0 || now &- lastEmitNs > 250_000_000 else { return }

        var stats = PipelineStats()
        stats.decode = decoder.snapshot()
        let n = min(recvToDoneCount, recvToDoneRing.count)
        if n > 0 {
            let sorted = recvToDoneRing[0 ..< n].sorted()
            stats.recvToDoneP50Ms = sorted[n / 2]
            stats.recvToDoneP99Ms = sorted[min(n - 1, Int(Double(n) * 0.99))]
            stats.recvToDoneMaxMs = recvToDoneMaxMs
        }
        lock.lock()
        stats.queueDepth = enqueued - done
        lock.unlock()
        stats.renderedFrames = frameStore.renderedFrames

        if lastEmitNs != 0 {
            let dt = Double(now &- lastEmitNs) / 1_000_000_000
            if dt > 0 {
                stats.decodedFPS = Double(stats.decode.decodedFrames - lastEmitDecoded) / dt
            }
        }
        lastEmitNs = now
        lastEmitDecoded = stats.decode.decodedFrames
        onStats(stats)
    }
}
