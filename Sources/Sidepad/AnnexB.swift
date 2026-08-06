// AnnexB.swift
//
// H.264 Annex-B バイトストリームの解析。
// PC 側エンコーダ（AMF）は開始コード区切りの Annex-B でアクセスユニットを送ってくるが、
// VideoToolbox は AVCC（4バイトビッグエンディアン長プレフィックス）しか受け付けない。
// このファイルはその詰め替え（NALU 分割 → 分類 → AVCC 化）だけを担う。
//
// 【重要】エミュレーション防止バイト (0x000003) は展開しない。
// 展開は VT デコーダの仕事で、ここで触ると二重展開になる。
// 逆に言えば、防止バイトのおかげで NALU payload 内に開始コードは現れないので、
// 素朴な開始コード検索で正しく分割できる。
//
// Foundation のみに依存（VideoToolbox/UIKit 非依存）。
// WSL の swift インタプリタで xtool/run-annexb-tests.sh により単体検証する。

import Foundation

/// NAL unit type（nal_unit_header の下位5ビット）。本プロジェクトで意味を持つものだけ列挙。
enum H264NALType: UInt8 {
    case nonIDRSlice = 1
    case idrSlice = 5
    case sei = 6
    case sps = 7
    case pps = 8
    case accessUnitDelimiter = 9

    static func of(_ nalu: [UInt8]) -> UInt8 {
        guard let first = nalu.first else { return 0 }
        return first & 0x1F
    }
}

/// 1アクセスユニットを分類した結果。
struct ParsedAccessUnit {
    var sps: [[UInt8]] = []
    var pps: [[UInt8]] = []
    /// スライス NAL（IDR/non-IDR）。デコードに使うのはこれだけ。
    var vcl: [[UInt8]] = []
    var hasIDR = false
}

enum AnnexB {
    /// 開始コード（00 00 01 / 00 00 00 01 の両方）で NALU に分割する。
    /// 最初の開始コードより前のバイトは捨てる。空 NALU（開始コードが連続）は返さない。
    static func splitNALUs(_ data: [UInt8]) -> [[UInt8]] {
        var nalus: [[UInt8]] = []
        var i = 0
        var currentStart = -1  // 現在の NALU payload の開始位置（-1 = まだ開始コード未発見）
        let n = data.count

        while i + 2 < n {
            if data[i] == 0, data[i + 1] == 0, data[i + 2] == 1 {
                // 3バイト開始コード発見。直前が 0 なら4バイト開始コードの一部。
                var codeStart = i
                if i > 0, data[i - 1] == 0 { codeStart = i - 1 }
                if currentStart >= 0, codeStart > currentStart {
                    nalus.append(Array(data[currentStart ..< codeStart]))
                }
                currentStart = i + 3
                i += 3
            } else if data[i + 2] != 0 {
                // 次に 00 00 01 が始まり得るのは i+3 以降（データの大半をスキップする高速化）
                i += 3
            } else {
                i += 1
            }
        }
        if currentStart >= 0, n > currentStart {
            nalus.append(Array(data[currentStart ..< n]))
        }
        return nalus
    }

    /// アクセスユニット（1フレーム分の Annex-B バイト列）を分類する。
    static func parseAccessUnit(_ data: [UInt8]) -> ParsedAccessUnit {
        var result = ParsedAccessUnit()
        for nalu in splitNALUs(data) {
            switch H264NALType.of(nalu) {
            case H264NALType.sps.rawValue:
                result.sps.append(nalu)
            case H264NALType.pps.rawValue:
                result.pps.append(nalu)
            case H264NALType.idrSlice.rawValue:
                result.hasIDR = true
                result.vcl.append(nalu)
            case H264NALType.nonIDRSlice.rawValue:
                result.vcl.append(nalu)
            default:
                break  // SEI / AUD 等はデコードに不要（formatDescription にも入れない）
            }
        }
        return result
    }

    /// VCL NALU 群を AVCC（4バイトBE長プレフィックス連結）へ詰め替える。
    static func avccData(_ nalus: [[UInt8]]) -> [UInt8] {
        var out: [UInt8] = []
        out.reserveCapacity(nalus.reduce(0) { $0 + $1.count + 4 })
        for nalu in nalus {
            let len = UInt32(nalu.count)
            out.append(UInt8(truncatingIfNeeded: len >> 24))
            out.append(UInt8(truncatingIfNeeded: len >> 16))
            out.append(UInt8(truncatingIfNeeded: len >> 8))
            out.append(UInt8(truncatingIfNeeded: len))
            out.append(contentsOf: nalu)
        }
        return out
    }
}
