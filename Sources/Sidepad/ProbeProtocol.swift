// ProbeProtocol.swift
//
// PC(Windows) ⇄ iPad 間のリンク計測／将来の映像転送で共通に使うワイヤフォーマット。
//
// フレーム形式（すべてリトルエンディアン）:
//   [u32 length][u8 type][payload ...]
//   length は type + payload のバイト数（length 自身は含まない）。
//
// TCP はバイトストリームなので「1回の receive = 1メッセージ」にはならない。
// 長さプレフィックスを付けて受信側で境界を復元する（この復元処理が
// そのまま映像フレームの切り出しにも使えるので、計測用と本実装で共用する）。

import Foundation

/// メッセージ種別。PC 側 probe スクリプトと値を一致させること。
enum ProbeMessage: UInt8 {
    /// PC→iPad: 往復遅延の測定。payload = [u64 seq][u64 t_pc_ns]
    case ping = 1
    /// iPad→PC: ping への応答。payload = [u64 seq][u64 t_pc_ns][u64 t_dev_ns]
    case pong = 2
    /// PC→iPad: 負荷データ。payload = [u64 seq][u64 t_pc_ns][bytes...]
    case data = 3
    /// iPad→PC: data の受信通知（ackInterval 個ごと）。payload = [u64 seq][u64 t_dev_ns]
    case ack = 4
    /// PC→iPad: 計測リセット＋設定。payload = [u32 ackInterval][utf8 label]
    case reset = 5
    /// PC→iPad: 集計結果の要求。payload なし
    case reportRequest = 6
    /// iPad→PC: 集計結果。payload = UTF-8 JSON
    case report = 7
}

enum Framing {
    static let headerSize = 4

    static func encode(_ type: ProbeMessage, _ payload: Data = Data()) -> Data {
        var out = Data(capacity: headerSize + 1 + payload.count)
        out.append(littleEndian: UInt32(payload.count + 1))
        out.append(type.rawValue)
        out.append(payload)
        return out
    }

    /// バッファ先頭から取り出せるだけフレームを取り出す。
    /// 取り出せた分はバッファから削除し、途中までのフレームは残す。
    static func drain(_ buffer: inout Data, _ body: (ProbeMessage, Data) -> Void) {
        var offset = 0
        while buffer.count - offset >= headerSize {
            let length = Int(buffer.readUInt32(at: offset))
            guard length >= 1 else {
                // 壊れたストリーム。以降を捨てて同期を諦める（実運用では再接続が正解）。
                buffer.removeAll(keepingCapacity: true)
                return
            }
            guard buffer.count - offset - headerSize >= length else { break }
            let typeRaw = buffer[buffer.startIndex + offset + headerSize]
            let payloadStart = buffer.startIndex + offset + headerSize + 1
            let payload = buffer[payloadStart ..< payloadStart + (length - 1)]
            if let type = ProbeMessage(rawValue: typeRaw) {
                body(type, Data(payload))
            }
            offset += headerSize + length
        }
        if offset > 0 {
            buffer.removeSubrange(buffer.startIndex ..< buffer.startIndex + offset)
        }
    }
}

// MARK: - バイト列ヘルパ

// NOTE: Data の extension 内では `withUnsafeBytes` が Data の
// インスタンスメソッドに解決されてしまいコンパイルが通らない（2026-08-03 実測）。
// バイト単位の素直なシフト演算にして、曖昧さもエンディアン依存も断つ。
extension Data {
    mutating func append(littleEndian value: UInt32) {
        for i in 0 ..< 4 {
            append(UInt8(truncatingIfNeeded: value >> (8 * UInt32(i))))
        }
    }

    mutating func append(littleEndian value: UInt64) {
        for i in 0 ..< 8 {
            append(UInt8(truncatingIfNeeded: value >> (8 * UInt64(i))))
        }
    }

    /// offset は「このData先頭からの相対位置」。スライスでも安全に読めるよう startIndex を足す。
    func readUInt32(at offset: Int) -> UInt32 {
        let start = startIndex + offset
        var v: UInt32 = 0
        for i in 0 ..< 4 {
            v |= UInt32(self[start + i]) << (8 * UInt32(i))
        }
        return v
    }

    func readUInt64(at offset: Int) -> UInt64 {
        let start = startIndex + offset
        var v: UInt64 = 0
        for i in 0 ..< 8 {
            v |= UInt64(self[start + i]) << (8 * UInt64(i))
        }
        return v
    }
}

/// 単調増加クロック（ns）。日時の補正や再起動の影響を受けない。
@inline(__always)
func monotonicNanoseconds() -> UInt64 {
    clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)
}
