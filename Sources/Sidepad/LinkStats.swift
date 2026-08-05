// LinkStats.swift
//
// リンク計測の集計器。USBTransportClient が受け取ったフレームを食わせると、
// スループット・遅延ゆらぎ・ジッタ・取りこぼし・CPU使用率を算出する。
//
// 【クロック同期を必要としない設計】
// PC と iPad の単調クロックには未知のオフセット C がある。
//   観測値 D_i = t_dev_recv - t_pc_send = 真の片道遅延 L_i + C
// 絶対値 L_i は C が不明なので出せないが、
//   D_i - min(D) = L_i - min(L)
// は C が打ち消えて厳密に求まる。ジッタバッファの深さを決めるのは
// まさにこの「最小遅延からの上振れ」なので、同期無しで設計値が出せる。
// 絶対遅延の当たりは PC 側が別途 PING/PONG の RTT から取る。

import Darwin
import Foundation

struct LinkReport: Codable, Sendable {
    var label: String
    var durationSec: Double
    var packets: Int
    var bytes: Int
    var throughputMBps: Double
    var throughputMbps: Double
    var meanPacketBytes: Double

    /// 片道遅延の「最小値からの上振れ」(ms)。ジッタバッファ深さの直接の根拠。
    var owdExcessP50Ms: Double
    var owdExcessP95Ms: Double
    var owdExcessP99Ms: Double
    var owdExcessMaxMs: Double
    /// RFC3550 形式の到着間隔ジッタ(ms)。送信間隔の揺れを差し引いた値。
    var interarrivalJitterMs: Double
    /// 連続する到着の最大間隔(ms)。詰まり（stall）の検出用。
    var maxArrivalGapMs: Double

    var outOfOrder: Int
    var missingSeq: Int
    /// アプリプロセスのCPU使用率(%)。100% = 1コア相当。
    var cpuPercent: Double
    var samples: Int
}

/// 受信フレームを食わせて統計を作る。USBTransportClient の直列キュー上でのみ触ること。
final class LinkStats {
    private(set) var label = ""
    private var ackInterval = 16

    private var packets = 0
    private var bytes = 0
    private var firstRecvNs: UInt64 = 0
    private var lastRecvNs: UInt64 = 0

    private var prevSendNs: UInt64 = 0
    private var prevRecvNs: UInt64 = 0
    private var jitterNs: Double = 0
    private var maxGapNs: UInt64 = 0

    private var nextExpectedSeq: UInt64 = 0
    private var outOfOrder = 0
    private var missingSeq = 0

    /// D_i = t_dev_recv - t_pc_send（オフセット込み。差分でのみ使う）
    private var delaySamples: [Int64] = []
    /// メモリ暴走防止。超えたら以降は間引いて取る。
    private let maxSamples = 300_000
    private var sampleStride = 1

    private var cpuAtStartNs: UInt64 = 0
    private var wallAtStartNs: UInt64 = 0

    func reset(label: String, ackInterval: Int) {
        self.label = label
        self.ackInterval = max(1, ackInterval)
        packets = 0
        bytes = 0
        firstRecvNs = 0
        lastRecvNs = 0
        prevSendNs = 0
        prevRecvNs = 0
        jitterNs = 0
        maxGapNs = 0
        nextExpectedSeq = 0
        outOfOrder = 0
        missingSeq = 0
        delaySamples.removeAll(keepingCapacity: true)
        delaySamples.reserveCapacity(16384)
        sampleStride = 1
        cpuAtStartNs = Self.processCPUNanoseconds()
        wallAtStartNs = monotonicNanoseconds()
    }

    /// DATA フレームを1つ処理する。ACK を返すべきなら true。
    @discardableResult
    func ingest(seq: UInt64, sendNs: UInt64, payloadBytes: Int, recvNs: UInt64) -> Bool {
        if packets == 0 {
            firstRecvNs = recvNs
            if wallAtStartNs == 0 { wallAtStartNs = recvNs }
        } else {
            let gap = recvNs &- lastRecvNs
            if gap > maxGapNs { maxGapNs = gap }

            // RFC3550: D = (Rj - Ri) - (Sj - Si)。送受で同じ ns 単位なのでオフセットは消える。
            let recvDelta = Double(recvNs &- prevRecvNs)
            let sendDelta = Double(sendNs &- prevSendNs)
            let d = abs(recvDelta - sendDelta)
            jitterNs += (d - jitterNs) / 16.0
        }

        if seq != nextExpectedSeq {
            if seq < nextExpectedSeq {
                outOfOrder += 1
            } else {
                missingSeq += Int(seq - nextExpectedSeq)
            }
        }
        nextExpectedSeq = seq &+ 1

        lastRecvNs = recvNs
        prevRecvNs = recvNs
        prevSendNs = sendNs
        packets += 1
        bytes += payloadBytes

        if packets % sampleStride == 0 {
            delaySamples.append(Int64(bitPattern: recvNs &- sendNs))
            if delaySamples.count >= maxSamples {
                // 半分に間引いて以降のストライドを倍にする（全区間を代表させ続ける）
                delaySamples = stride(from: 0, to: delaySamples.count, by: 2).map { delaySamples[$0] }
                sampleStride *= 2
            }
        }

        return packets % ackInterval == 0
    }

    func makeReport() -> LinkReport {
        let nowNs = monotonicNanoseconds()
        let durationNs = packets > 1 ? Double(lastRecvNs &- firstRecvNs) : 0
        let durationSec = durationNs / 1e9
        let wallSec = Double(nowNs &- wallAtStartNs) / 1e9
        // task_thread_times_info は「生存スレッドのみ」の合計なので、計測中にスレッドが
        // 死ぬと現在値 < 開始値になり得る。符号無し減算だと桁違いの値に化けるため 0 で下限を切る
        // （2026-08-06 実測: CPU% が 2.6e11% と報告された）。
        let cpuNow = Self.processCPUNanoseconds()
        let cpuNs = cpuNow > cpuAtStartNs ? cpuNow - cpuAtStartNs : 0

        let sorted = delaySamples.sorted()
        let minDelay = sorted.first ?? 0
        func excessMs(_ p: Double) -> Double {
            guard !sorted.isEmpty else { return 0 }
            let idx = min(sorted.count - 1, max(0, Int((p * Double(sorted.count - 1)).rounded())))
            return Double(sorted[idx] - minDelay) / 1e6
        }

        return LinkReport(
            label: label,
            durationSec: durationSec,
            packets: packets,
            bytes: bytes,
            throughputMBps: durationSec > 0 ? Double(bytes) / durationSec / 1_048_576 : 0,
            throughputMbps: durationSec > 0 ? Double(bytes) * 8 / durationSec / 1_000_000 : 0,
            meanPacketBytes: packets > 0 ? Double(bytes) / Double(packets) : 0,
            owdExcessP50Ms: excessMs(0.50),
            owdExcessP95Ms: excessMs(0.95),
            owdExcessP99Ms: excessMs(0.99),
            owdExcessMaxMs: excessMs(1.0),
            interarrivalJitterMs: jitterNs / 1e6,
            maxArrivalGapMs: Double(maxGapNs) / 1e6,
            outOfOrder: outOfOrder,
            missingSeq: missingSeq,
            cpuPercent: wallSec > 0 ? Double(cpuNs) / 1e9 / wallSec * 100 : 0,
            samples: sorted.count
        )
    }

    /// ライブ表示用の軽いスナップショット（毎パケット計算しないための簡易版）。
    func snapshot() -> LinkSnapshot {
        let durationSec = packets > 1 ? Double(lastRecvNs &- firstRecvNs) / 1e9 : 0
        return LinkSnapshot(
            packets: packets,
            bytes: bytes,
            throughputMBps: durationSec > 0 ? Double(bytes) / durationSec / 1_048_576 : 0,
            interarrivalJitterMs: jitterNs / 1e6,
            maxArrivalGapMs: Double(maxGapNs) / 1e6
        )
    }

    /// プロセス全体のCPU時間（生存スレッドのユーザ＋システム時間）。
    private static func processCPUNanoseconds() -> UInt64 {
        var info = task_thread_times_info()
        var count = mach_msg_type_number_t(
            MemoryLayout<task_thread_times_info>.size / MemoryLayout<natural_t>.size
        )
        let result = withUnsafeMutablePointer(to: &info) { ptr -> kern_return_t in
            ptr.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_THREAD_TIMES_INFO), $0, &count)
            }
        }
        guard result == KERN_SUCCESS else { return 0 }
        let user = UInt64(info.user_time.seconds) * 1_000_000_000 + UInt64(info.user_time.microseconds) * 1000
        let system = UInt64(info.system_time.seconds) * 1_000_000_000 + UInt64(info.system_time.microseconds) * 1000
        return user + system
    }
}

struct LinkSnapshot: Sendable {
    var packets = 0
    var bytes = 0
    var throughputMBps: Double = 0
    var interarrivalJitterMs: Double = 0
    var maxArrivalGapMs: Double = 0
}
