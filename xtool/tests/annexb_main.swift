// annexb_main.swift
//
// AnnexB.swift の単体検証。AnnexB.swift と連結して swift インタプリタで実行する
// （実行方法は xtool/run-annexb-tests.sh。SwiftPM のテストターゲットにはしない:
//   swift test はパッケージ全ターゲットをビルドするため、SwiftUI 依存の
//   アプリターゲットが Linux/Windows ホストで壊れる）。
// すべての assert を通過して "PASS" を出力し exit 0 したら合格。

import Foundation

func check(_ condition: Bool, _ label: String) {
    if !condition {
        print("FAIL: \(label)")
        exit(1)
    }
}

// ---- splitNALUs: 3バイト/4バイト開始コード混在・先頭ゴミ・末尾NALU ----

// [ゴミ 0xAA] [0001] SPS(0x67 1 2 3) [001] PPS(0x68 4) [0001] IDR(0x65 9 9)
let mixed: [UInt8] = [
    0xAA,
    0x00, 0x00, 0x00, 0x01, 0x67, 1, 2, 3,
    0x00, 0x00, 0x01, 0x68, 4,
    0x00, 0x00, 0x00, 0x01, 0x65, 9, 9,
]
let nalus = AnnexB.splitNALUs(mixed)
check(nalus.count == 3, "mixed: NALU数 (\(nalus.count))")
check(nalus[0] == [0x67, 1, 2, 3], "mixed: SPS payload")
check(nalus[1] == [0x68, 4], "mixed: PPS payload")
check(nalus[2] == [0x65, 9, 9], "mixed: IDR payload（末尾NALUがデータ終端まで）")

// ---- 空NALU（開始コード連続）はスキップ ----
let empties: [UInt8] = [0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x41, 7]
let n2 = AnnexB.splitNALUs(empties)
check(n2.count == 1 && n2[0] == [0x41, 7], "空NALUスキップ")

// ---- 開始コード無し → 空 ----
check(AnnexB.splitNALUs([1, 2, 3, 4]).isEmpty, "開始コード無し")
check(AnnexB.splitNALUs([]).isEmpty, "空入力")

// ---- エミュレーション防止バイトは保存される（payload をバイト単位で変えない） ----
let epb: [UInt8] = [0x00, 0x00, 0x01, 0x65, 0x00, 0x00, 0x03, 0x01, 0xFF]
let n3 = AnnexB.splitNALUs(epb)
check(n3.count == 1 && n3[0] == [0x65, 0x00, 0x00, 0x03, 0x01, 0xFF], "EPB保存")

// ---- parseAccessUnit: 分類・IDR判定・SEI/AUD除外 ----
// AUD(0x09) SEI(0x06) SPS SPS PPS IDR nonIDR という人工AU
var au: [UInt8] = []
func appendNALU(_ payload: [UInt8]) {
    au.append(contentsOf: [0x00, 0x00, 0x00, 0x01])
    au.append(contentsOf: payload)
}
appendNALU([0x09, 0xF0])              // AUD
appendNALU([0x06, 0x05, 0x00])        // SEI
appendNALU([0x67, 0x64, 0x00, 0x28])  // SPS
appendNALU([0x68, 0xEE])              // PPS
appendNALU([0x65, 0x88, 0x84])        // IDR slice
appendNALU([0x41, 0x9A])              // non-IDR slice
let parsed = AnnexB.parseAccessUnit(au)
check(parsed.sps.count == 1 && parsed.sps[0][0] == 0x67, "SPS抽出")
check(parsed.pps.count == 1 && parsed.pps[0][0] == 0x68, "PPS抽出")
check(parsed.vcl.count == 2, "VCLはスライス2本のみ（SEI/AUD除外）")
check(parsed.hasIDR, "IDR判定")

// P フレームだけの AU
let pOnly = AnnexB.parseAccessUnit([0x00, 0x00, 0x01, 0x41, 0x9A, 0x00, 0x00, 0x01, 0x41, 0x9B])
check(pOnly.sps.isEmpty && pOnly.pps.isEmpty && pOnly.vcl.count == 2 && !pOnly.hasIDR, "PのみAU")

// ---- avccData: 4バイトBE長プレフィックス ----
let avcc = AnnexB.avccData([[0x65, 1, 2], [0x41]])
check(avcc == [0, 0, 0, 3, 0x65, 1, 2, 0, 0, 0, 1, 0x41], "AVCC長プレフィックス")
check(AnnexB.avccData([]).isEmpty, "AVCC空入力")

// 長さ 256 超（BE の桁またぎ）
let big = [UInt8](repeating: 0x41, count: 300)
let avccBig = AnnexB.avccData([big])
check(Array(avccBig[0 ..< 4]) == [0, 0, 0x01, 0x2C], "AVCC長 300 = 0x012C")
check(avccBig.count == 304, "AVCC全長")

print("PASS")
