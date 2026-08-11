// touchwire_main.swift
//
// TouchWire（TouchInput.swift）の単体検証。ProbeProtocol.swift + TouchInput.swift と
// 連結して swiftc でコンパイル・実行する（実行方法は xtool/run-touchwire-tests.sh。
// annexb_main.swift と同じ形。TouchInput.swift の UIKit依存部分は
// #if canImport(UIKit) で囲まれているため、非Appleホストではこのファイルから
// 見える TouchEvent/TouchWire/TouchNormalize だけがコンパイルされる）。
//
// すべての assert を通過して "PASS" を出力し exit 0 したら合格。

import Foundation

func check(_ condition: Bool, _ label: String) {
    if !condition {
        print("FAIL: \(label)")
        exit(1)
    }
}

func hex(_ data: Data) -> String {
    data.map { String(format: "%02X", $0) }.joined(separator: " ")
}

// ---- ラウンドトリップ: 全kind ----

for kind: TouchKind in [.move, .tap, .scroll, .dragBegin, .dragEnd] {
    for source: TouchSource in [.finger, .pencil] {
        let event = TouchEvent(kind: kind, source: source, x: 100, y: 200, dx: -5, dy: 5)
        let encoded = TouchWire.encode(event)
        check(encoded.count == TouchWire.payloadSize, "\(kind)/\(source): payload size == 10")
        guard let decoded = TouchWire.decode(encoded) else {
            check(false, "\(kind)/\(source): decode failed")
            continue
        }
        check(decoded == event, "\(kind)/\(source): roundtrip一致")
    }
}

// ---- 境界値: x/y = 0 と 65535 ----

do {
    let event = TouchEvent(kind: .tap, source: .finger, x: 0, y: 0, dx: 0, dy: 0)
    let decoded = TouchWire.decode(TouchWire.encode(event))
    check(decoded == event, "境界値 x=0,y=0")
}
do {
    let event = TouchEvent(kind: .tap, source: .finger, x: 65535, y: 65535, dx: 0, dy: 0)
    let decoded = TouchWire.decode(TouchWire.encode(event))
    check(decoded == event, "境界値 x=65535,y=65535")
}

// ---- 境界値: dx/dy の負値・Int16の両端 ----

do {
    let event = TouchEvent(kind: .scroll, source: .pencil, x: 1, y: 1, dx: Int16.min, dy: Int16.max)
    let decoded = TouchWire.decode(TouchWire.encode(event))
    check(decoded == event, "境界値 dx=Int16.min,dy=Int16.max")
}
do {
    let event = TouchEvent(kind: .scroll, source: .finger, x: 1, y: 1, dx: -1, dy: -300)
    let decoded = TouchWire.decode(TouchWire.encode(event))
    check(decoded == event, "境界値 負のdx,dy")
}

// ---- 不正入力: サイズ不一致・不明kind/sourceはnilを返す ----

check(TouchWire.decode(Data()) == nil, "空payloadはnil")
check(TouchWire.decode(Data(repeating: 0, count: 9)) == nil, "9バイト(不足)はnil")
check(TouchWire.decode(Data(repeating: 0, count: 11)) == nil, "11バイト(過剰)はnil")

do {
    var bad = TouchWire.encode(TouchEvent(kind: .tap, source: .finger, x: 0, y: 0, dx: 0, dy: 0))
    bad[bad.startIndex] = 0  // kind=0 は未定義
    check(TouchWire.decode(bad) == nil, "不明kindはnil")
}
do {
    var bad = TouchWire.encode(TouchEvent(kind: .tap, source: .finger, x: 0, y: 0, dx: 0, dy: 0))
    bad[bad.startIndex + 1] = 2  // source=2 は未定義
    check(TouchWire.decode(bad) == nil, "不明sourceはnil")
}

// ---- ゴールデンバイト列照合（design.md §1。C++実装(PC側)と独立に固定するための基準値） ----

// G1 tap: kind=2 source=0 x=0x1234 y=0xABCD dx=0 dy=0
// wire = 0B 00 00 00 08 02 00 34 12 CD AB 00 00 00 00
do {
    let event = TouchEvent(kind: .tap, source: .finger, x: 0x1234, y: 0xABCD, dx: 0, dy: 0)
    let wire = Framing.encode(.touch, TouchWire.encode(event))
    let expected: [UInt8] = [
        0x0B, 0x00, 0x00, 0x00, 0x08, 0x02, 0x00, 0x34, 0x12, 0xCD, 0xAB, 0x00, 0x00, 0x00, 0x00,
    ]
    check(Array(wire) == expected, "G1 tap ゴールデンバイト列 (got: \(hex(wire)))")
}

// G2 scroll: kind=3 source=1 x=0x8000 y=0x8000 dx=-10 dy=300
// wire = 0B 00 00 00 08 03 01 00 80 00 80 F6 FF 2C 01
do {
    let event = TouchEvent(kind: .scroll, source: .pencil, x: 0x8000, y: 0x8000, dx: -10, dy: 300)
    let wire = Framing.encode(.touch, TouchWire.encode(event))
    let expected: [UInt8] = [
        0x0B, 0x00, 0x00, 0x00, 0x08, 0x03, 0x01, 0x00, 0x80, 0x00, 0x80, 0xF6, 0xFF, 0x2C, 0x01,
    ]
    check(Array(wire) == expected, "G2 scroll ゴールデンバイト列 (got: \(hex(wire)))")
}

// G3 dragBegin: kind=4 source=0 x=0x0001 y=0xFFFE dx=0 dy=0（2026-08-11 長押しドラッグ追加分）
// wire = 0B 00 00 00 08 04 00 01 00 FE FF 00 00 00 00
do {
    let event = TouchEvent(kind: .dragBegin, source: .finger, x: 0x0001, y: 0xFFFE, dx: 0, dy: 0)
    let wire = Framing.encode(.touch, TouchWire.encode(event))
    let expected: [UInt8] = [
        0x0B, 0x00, 0x00, 0x00, 0x08, 0x04, 0x00, 0x01, 0x00, 0xFE, 0xFF, 0x00, 0x00, 0x00, 0x00,
    ]
    check(Array(wire) == expected, "G3 dragBegin ゴールデンバイト列 (got: \(hex(wire)))")
}

// ---- Framing.drain で G1/G2 を復元できることも確認（送受信の往復を模擬） ----

do {
    var buffer = Data()
    let e1 = TouchEvent(kind: .tap, source: .finger, x: 0x1234, y: 0xABCD, dx: 0, dy: 0)
    let e2 = TouchEvent(kind: .scroll, source: .pencil, x: 0x8000, y: 0x8000, dx: -10, dy: 300)
    buffer.append(Framing.encode(.touch, TouchWire.encode(e1)))
    buffer.append(Framing.encode(.touch, TouchWire.encode(e2)))

    var received: [TouchEvent] = []
    Framing.drain(&buffer) { type, payload in
        check(type == .touch, "drain: type==touch")
        if let decoded = TouchWire.decode(payload) {
            received.append(decoded)
        }
    }
    check(received.count == 2, "drain: 2件復元 (got \(received.count))")
    check(received.first == e1, "drain: 1件目一致")
    check(received.last == e2, "drain: 2件目一致")
    check(buffer.isEmpty, "drain: バッファ消費済み")
}

print("PASS")
