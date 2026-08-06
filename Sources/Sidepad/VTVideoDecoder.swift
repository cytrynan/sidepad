// VTVideoDecoder.swift
//
// VideoToolbox (VTDecompressionSession) による H.264 ハードウェアデコード。
//
// 前提（PC側 amf_encoder.cpp の設計値）:
// - H.264 High profile・Bフレーム無し → 表示順 = デコード順。同期デコード（flags=[]）で
//   completion がデコード完了までに呼ばれ、並べ替えバッファが不要になる（低遅延の要）。
// - IDR は先頭1回のみ＋イントラリフレッシュ。SPS/PPS は先頭 IDR に付属して届く。
//   SPS/PPS 未着のフレームはデコード不能なので破棄してカウントだけする。
//
// スレッド規約: decode()/reset() は VideoPipeline の直列 decode キューからのみ呼ぶ。
// 内部状態はロックしない（呼び出し側の直列性に依存）。

import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

/// UI 表示用のデコード統計スナップショット。
struct DecodeStats: Sendable {
    var decodedFrames = 0
    var decodeErrors = 0
    /// SPS/PPS 到着前に捨てたフレーム数（PC送信開始前に接続していれば 0 のはず）。
    var droppedPreSPS = 0
    var sessionRebuilds = 0
    /// デコード呼び出しの壁時計 (ms)
    var decodeP50Ms = 0.0
    var decodeP99Ms = 0.0
    var decodeMaxMs = 0.0
    var width = 0
    var height = 0
}

final class VTVideoDecoder: VideoDecoder {
    private var session: VTDecompressionSession?
    private var formatDescription: CMVideoFormatDescription?
    private var currentSPS: [[UInt8]] = []
    private var currentPPS: [[UInt8]] = []

    private var stats = DecodeStats()
    // 受入走行(20秒×60fps=1200)が丸ごと収まる窓にする（レビュー指摘M2:
    // 256=約4.3秒では走行序盤のスパイクが p99 から消え、合格判定を誤る）
    private var decodeMsRing = [Double](repeating: 0, count: 2048)
    private var decodeMsCount = 0
    /// セッション再生成の再試行バックオフ（レビュー指摘M4対応）
    private var lastRebuildAttemptNs: UInt64 = 0

    var onLog: ((String) -> Void)?

    // MARK: - VideoDecoder

    func decode(_ data: Data, completion: @escaping (CVPixelBuffer?) -> Void) {
        let au = AnnexB.parseAccessUnit([UInt8](data))

        // SPS/PPS が届いたら（初回 or 変化時）セッションを作り直す
        if !au.sps.isEmpty, !au.pps.isEmpty, au.sps != currentSPS || au.pps != currentPPS {
            rebuildSession(sps: au.sps, pps: au.pps)
        }

        // セッション生成に失敗したまま放置しない（レビュー指摘M4: SPS/PPS は先頭IDRにしか
        // 付かないので、ここで復帰を試みないと残り全フレームが凍結する）。500ms バックオフ。
        if session == nil, !currentSPS.isEmpty, !currentPPS.isEmpty {
            let now = monotonicNanoseconds()
            if now &- lastRebuildAttemptNs > 500_000_000 {
                rebuildSession(sps: currentSPS, pps: currentPPS)
            }
        }

        guard session != nil, formatDescription != nil else {
            stats.droppedPreSPS += 1
            completion(nil)
            return
        }
        guard !au.vcl.isEmpty else {
            completion(nil)  // パラメータセットのみの AU（現構成では来ない想定）
            return
        }

        let t0 = monotonicNanoseconds()
        var output: CVPixelBuffer?
        let status = decodeAVCC(AnnexB.avccData(au.vcl), into: &output)

        if status == kVTInvalidSessionErr {
            // バックグラウンド復帰等でセッションが無効化された場合。
            // パラメータセットは手元にあるので再生成して同じフレームをもう一度だけ試す。
            log("VTセッション無効化を検出。再生成して再試行")
            rebuildSession(sps: currentSPS, pps: currentPPS)
            _ = decodeAVCC(AnnexB.avccData(au.vcl), into: &output)
        }

        if output != nil {
            stats.decodedFrames += 1
            recordDecodeMs(Double(monotonicNanoseconds() &- t0) / 1_000_000)
        } else {
            stats.decodeErrors += 1
            if stats.decodeErrors <= 5 {
                log("デコード失敗 status=\(status) (通算\(stats.decodeErrors)回)")
            }
        }
        completion(output)
    }

    func reset() {
        if let session {
            VTDecompressionSessionInvalidate(session)
        }
        session = nil
        formatDescription = nil
        currentSPS = []
        currentPPS = []
        // 統計は接続をまたいで累積させる（UI で通算を見たいので消さない）
    }

    func snapshot() -> DecodeStats {
        var s = stats
        let n = min(decodeMsCount, decodeMsRing.count)
        if n > 0 {
            let sorted = decodeMsRing[0 ..< n].sorted()
            s.decodeP50Ms = sorted[n / 2]
            s.decodeP99Ms = sorted[min(n - 1, Int(Double(n) * 0.99))]
        }
        return s
    }

    /// 計測カウンタをゼロに戻す（PC側 RESET 受信時。レビュー指摘M3:
    /// 受信パケット数は RESET で 0 に戻るのに、デコード統計だけ累積だと
    /// 「decodedFrames == 受信フレーム数」の受入照合が2回目の走行から成立しない）。
    /// セッション・SPS/PPS はストリーム状態なので触らない。
    func resetCounters() {
        let width = stats.width
        let height = stats.height
        stats = DecodeStats()
        stats.width = width
        stats.height = height
        decodeMsCount = 0
    }

    // MARK: - セッション管理

    private func rebuildSession(sps: [[UInt8]], pps: [[UInt8]]) {
        reset()  // 旧セッション破棄（currentSPS/PPS もクリアされるので後で入れ直す）
        currentSPS = sps
        currentPPS = pps

        // CMVideoFormatDescriptionCreateFromH264ParameterSets に全パラメータセットを渡す
        let paramSets: [[UInt8]] = sps + pps
        var fmt: CMVideoFormatDescription?
        let status: OSStatus = paramSets.withContiguousPointers { pointers, sizes in
            CMVideoFormatDescriptionCreateFromH264ParameterSets(
                allocator: kCFAllocatorDefault,
                parameterSetCount: pointers.count,
                parameterSetPointers: pointers,
                parameterSetSizes: sizes,
                nalUnitHeaderLength: 4,
                formatDescriptionOut: &fmt
            )
        }
        lastRebuildAttemptNs = monotonicNanoseconds()
        guard status == noErr, let fmt else {
            log("FormatDescription 生成失敗 status=\(status)")
            return
        }

        let dims = CMVideoFormatDescriptionGetDimensions(fmt)
        stats.width = Int(dims.width)
        stats.height = Int(dims.height)

        // Metal 描画にゼロコピーで渡すための必須属性。
        // ピクセル形式は NV12 video range を明示（シェーダの変換係数と前提を一致させる。
        // 既定に任せると full range 出力時に黒潰れする）。
        let pixelBufferAttrs: [CFString: Any] = [
            kCVPixelBufferMetalCompatibilityKey: true,
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        ]
        var newSession: VTDecompressionSession?
        let createStatus = VTDecompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            formatDescription: fmt,
            decoderSpecification: nil,
            imageBufferAttributes: pixelBufferAttrs as CFDictionary,
            outputCallback: nil,
            decompressionSessionOut: &newSession
        )
        guard createStatus == noErr, let newSession else {
            log("VTセッション生成失敗 status=\(createStatus)")
            return
        }
        // リアルタイム優先（内部バッファリングを抑える）。失敗しても致命ではない。
        VTSessionSetProperty(newSession, key: kVTDecompressionPropertyKey_RealTime, value: kCFBooleanTrue)

        session = newSession
        formatDescription = fmt
        stats.sessionRebuilds += 1
        log("VTセッション生成 \(dims.width)x\(dims.height)")
    }

    /// AVCC データを CMSampleBuffer に包んで同期デコードする。
    /// 成功時は output にピクセルバッファが入る。戻り値はデコード呼び出しの status。
    private func decodeAVCC(_ avcc: [UInt8], into output: inout CVPixelBuffer?) -> OSStatus {
        guard let session, let formatDescription else { return kVTInvalidSessionErr }

        var blockBuffer: CMBlockBuffer?
        var status = CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault,
            memoryBlock: nil,
            blockLength: avcc.count,
            blockAllocator: kCFAllocatorDefault,
            customBlockSource: nil,
            offsetToData: 0,
            dataLength: avcc.count,
            flags: 0,
            blockBufferOut: &blockBuffer
        )
        guard status == noErr, let blockBuffer else { return status }
        status = avcc.withUnsafeBytes { raw in
            CMBlockBufferReplaceDataBytes(
                with: raw.baseAddress!, blockBuffer: blockBuffer,
                offsetIntoDestination: 0, dataLength: avcc.count
            )
        }
        guard status == noErr else { return status }

        var sampleBuffer: CMSampleBuffer?
        var sampleSize = avcc.count
        status = CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault,
            dataBuffer: blockBuffer,
            formatDescription: formatDescription,
            sampleCount: 1,
            sampleTimingEntryCount: 0,
            sampleTimingArray: nil,
            sampleSizeEntryCount: 1,
            sampleSizeArray: &sampleSize,
            sampleBufferOut: &sampleBuffer
        )
        guard status == noErr, let sampleBuffer else { return status }

        // flags=[]（同期）: B無しストリームでは completion がこの呼び出し中に走り、
        // 出力の並べ替え・遅延ゼロでフレームが得られる。
        // （クロージャに Sendable が要求されるため、var でなくロック付きの箱で受ける）
        let box = PixelBufferBox()
        let decodeStatus = VTDecompressionSessionDecodeFrame(
            session, sampleBuffer: sampleBuffer, flags: [], infoFlagsOut: nil
        ) { st, _, imageBuffer, _, _ in
            if st == noErr {
                box.set(imageBuffer)
            }
        }
        // flags=[] なら handler は return 前に走るはずだが、前提が崩れた時に
        // 全フレーム黒落ちする総崩れを防ぐ保険（同期時は実質ノーコスト）
        VTDecompressionSessionWaitForAsynchronousFrames(session)
        output = box.take()
        return decodeStatus
    }

    // MARK: - 統計

    private func recordDecodeMs(_ ms: Double) {
        decodeMsRing[decodeMsCount % decodeMsRing.count] = ms
        decodeMsCount += 1
        if ms > stats.decodeMaxMs { stats.decodeMaxMs = ms }
    }

    private func log(_ message: String) {
        onLog?(message)
    }
}

/// 同期デコードの出力受け取り用。ハンドラは同一呼び出し内で走るが、
/// Sendable 要件を満たすためロックで包む。
private final class PixelBufferBox: @unchecked Sendable {
    private let lock = NSLock()
    private var buffer: CVPixelBuffer?

    func set(_ value: CVImageBuffer?) {
        lock.lock()
        buffer = value
        lock.unlock()
    }

    func take() -> CVPixelBuffer? {
        lock.lock()
        defer { lock.unlock() }
        return buffer
    }
}

// MARK: - パラメータセット配列 → C ポインタ配列 変換ヘルパ

private extension Array where Element == [UInt8] {
    /// [[UInt8]] の各要素の先頭ポインタと長さを、全要素の生存を保証したままクロージャに渡す。
    /// （可変個の withUnsafeBufferPointer ネストは書けないので、一時コピーを明示確保する。
    ///   セッション再生成時にしか呼ばれないのでコピーコストは無視できる）
    func withContiguousPointers<R>(
        _ body: (_ pointers: [UnsafePointer<UInt8>], _ sizes: [Int]) -> R
    ) -> R {
        let buffers: [UnsafeMutableBufferPointer<UInt8>] = map { bytes in
            let buf = UnsafeMutableBufferPointer<UInt8>.allocate(capacity: bytes.count)
            _ = buf.initialize(fromContentsOf: bytes)
            return buf
        }
        defer { buffers.forEach { $0.deallocate() } }
        return body(buffers.map { UnsafePointer($0.baseAddress!) }, map(\.count))
    }
}
