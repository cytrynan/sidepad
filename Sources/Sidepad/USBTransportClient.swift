// USBTransportClient.swift
//
// USB C-C データ通信（usbmux 経由の TCP）で PC から映像データを受け取る TransportClient 実装。
//
// 【なぜ iPad 側が listen するのか】
// usbmuxd（Windows/WSL 側）は「PC のローカルポート → デバイス上のTCPポート」という向きにしか
// 接続を張れない。したがって iPad がサーバ、PC がクライアントになる。
// 有線LAN(TCP)フォールバックでも同じ形にできるので、この実装はそのまま流用できる。
//
// 現段階では TransportClient としての受け口（onFrameData）に加えて、
// リンク実測用の PING/ACK/REPORT を処理する。計測が終わっても捨てずに、
// そのまま映像転送の土台として使う想定（計測用と本実装を分けない）。

import Foundation
import Network

final class USBTransportClient: TransportClient, @unchecked Sendable {
    static let defaultPort: UInt16 = 7789

    private let port: NWEndpoint.Port
    /// すべての可変状態はこの直列キュー上でのみ触る（コールバックもこのキューで届く）。
    private let queue = DispatchQueue(label: "com.sidepad.transport", qos: .userInteractive)
    private var listener: NWListener?
    private var connection: NWConnection?
    private var buffer = Data()
    private let stats = LinkStats()
    private var lastSnapshotNs: UInt64 = 0

    private(set) var state: ConnectionState = .notConnected {
        didSet {
            guard state != oldValue else { return }
            onStateChange?(state)
        }
    }

    var onStateChange: ((ConnectionState) -> Void)?
    var onFrameData: ((Data) -> Void)?
    /// PC側の計測リセット（RESETメッセージ）通知。デコード側の統計を LinkStats と同期して
    /// ゼロに戻すために使う（受入照合「decodedFrames == 受信フレーム数」の前提）。
    var onMeasurementReset: (() -> Void)?
    /// ライブ表示用（約10Hz）。
    var onSnapshot: ((LinkSnapshot) -> Void)?
    /// 画面に出す動作ログ（実機はコンソールが見えないので UI に出す）。
    var onLog: ((String) -> Void)?
    /// PC との接続が切れた時の理由通知（待ち受け画面の「直近の切断理由」表示用）。
    var onDisconnect: ((String) -> Void)?

    init(port: UInt16 = USBTransportClient.defaultPort) {
        self.port = NWEndpoint.Port(rawValue: port) ?? 7789
    }

    // MARK: - TransportClient

    func start() {
        queue.async { [self] in
            guard listener == nil else { return }
            let tcp = NWProtocolTCP.Options()
            tcp.noDelay = true               // Nagle は遅延の敵。小さい制御パケットを溜めさせない
            tcp.enableKeepalive = false
            let params = NWParameters(tls: nil, tcp: tcp)
            params.allowLocalEndpointReuse = true
            params.includePeerToPeer = false

            do {
                let newListener = try NWListener(using: params, on: port)
                newListener.newConnectionHandler = { [weak self] conn in
                    self?.accept(conn)
                }
                newListener.stateUpdateHandler = { [weak self] state in
                    switch state {
                    case .ready:
                        self?.log("listening on tcp/\(self?.port.rawValue ?? 0)")
                    case .failed(let error):
                        self?.log("listener failed: \(error)")
                        self?.state = .notConnected
                    default:
                        break
                    }
                }
                listener = newListener
                newListener.start(queue: queue)
                state = .connecting
            } catch {
                log("listener 生成失敗: \(error)")
                state = .notConnected
            }
        }
    }

    /// タッチイベントをPCへ送る。TouchCatcher からのコールバックで呼ばれる想定。
    /// 未接続なら黙って捨てる（touchは損失許容。design.md §2-2）。
    func sendTouch(_ event: TouchEvent) {
        queue.async { [self] in
            guard let connection else { return }
            send(.touch, TouchWire.encode(event), on: connection)
        }
    }

    func stop() {
        queue.async { [self] in
            connection?.cancel()
            connection = nil
            listener?.cancel()
            listener = nil
            buffer.removeAll(keepingCapacity: false)
            state = .notConnected
        }
    }

    // MARK: - 接続

    private func accept(_ conn: NWConnection) {
        // 同時に相手にするのは1本だけ。古い接続は落とす。
        connection?.cancel()
        connection = conn
        buffer.removeAll(keepingCapacity: true)

        conn.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .ready:
                log("PC 接続確立")
                self.state = .connecting
            case .failed(let error):
                // 入れ替え済み接続の .failed でも必ずログを残す（dropConnection 内の
                // === ガードで弾かれると無音になり、画面ログが唯一の診断経路の実機で困る）
                log("接続エラー: \(error)")
                dropConnection(conn, reason: "接続エラー: \(error)")
            case .cancelled:
                dropConnection(conn, reason: "接続キャンセル")
            default:
                break
            }
        }
        conn.start(queue: queue)
        receiveLoop(conn)
    }

    private func dropConnection(_ conn: NWConnection, reason: String) {
        guard connection === conn else { return }
        conn.cancel()  // 参照を外すだけだと未解放の NWConnection が抜き差しの回数だけ積み上がる
        connection = nil
        buffer.removeAll(keepingCapacity: true)
        onDisconnect?(reason)
        state = .connecting  // listener は生きているので次の接続を待てる
        log("切断（\(reason)）。次の接続を待機")
    }

    private func receiveLoop(_ conn: NWConnection) {
        // completion は conn.start(queue:) に渡した直列キュー上で呼ばれる。
        // 到着時刻はここで即座に取る（後段の処理時間を遅延に混ぜないため）。
        conn.receive(minimumIncompleteLength: 1, maximumLength: 262_144) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            let recvNs = monotonicNanoseconds()
            if let data, !data.isEmpty {
                handle(data, recvNs: recvNs, on: conn)
            }
            if isComplete || error != nil {
                if let error {
                    // NWError は localizedDescription だと "(NWError error 0.)" 等に化けて
                    // 原因(posix(ECONNRESET)等)が消える。文字列補間で debugDescription を使う
                    dropConnection(conn, reason: "受信エラー: \(error)")
                } else {
                    dropConnection(conn, reason: "PC側からストリーム終了")
                }
                return
            }
            receiveLoop(conn)
        }
    }

    // MARK: - フレーム処理

    private func handle(_ incoming: Data, recvNs: UInt64, on conn: NWConnection) {
        buffer.append(incoming)
        Framing.drain(&buffer) { [self] type, payload in
            switch type {
            case .data:
                guard payload.count >= 16 else { break }
                let seq = payload.readUInt64(at: 0)
                let sendNs = payload.readUInt64(at: 8)
                if state != .receiving { state = .receiving }
                let needAck = stats.ingest(
                    seq: seq,
                    sendNs: sendNs,
                    payloadBytes: payload.count + 5,  // 長さプレフィックス+型を含むワイヤバイト数
                    recvNs: recvNs
                )
                if needAck {
                    var ack = Data(capacity: 16)
                    ack.append(littleEndian: seq)
                    ack.append(littleEndian: recvNs)
                    send(.ack, ack, on: conn)
                }
                // 本実装での映像フレーム相当（16バイトの計測ヘッダを除いた中身）
                if let handler = onFrameData, payload.count > 16 {
                    handler(payload.subdata(in: 16 ..< payload.count))
                }
                emitSnapshotIfNeeded(recvNs)

            case .ping:
                guard payload.count >= 16 else { break }
                var pong = Data(capacity: 24)
                pong.append(payload)                      // seq + t_pc_ns をそのまま返す
                pong.append(littleEndian: monotonicNanoseconds())
                send(.pong, pong, on: conn)

            case .reset:
                var ackInterval = 16
                var label = ""
                if payload.count >= 4 {
                    ackInterval = Int(payload.readUInt32(at: 0))
                    label = String(data: payload.subdata(in: 4 ..< payload.count), encoding: .utf8) ?? ""
                }
                stats.reset(label: label, ackInterval: ackInterval)
                lastSnapshotNs = 0
                onMeasurementReset?()
                // ゼロ化したスナップショットを即時発行（前セッションの固定表示を消すため。
                // 発行しないと次のデータパケット到着まで @Published 側に旧値が残る）
                onSnapshot?(stats.snapshot())
                log("計測開始: \(label.isEmpty ? "(no label)" : label) ackごと=\(ackInterval)")

            case .reportRequest:
                let report = stats.makeReport()
                let json = (try? JSONEncoder().encode(report)) ?? Data()
                send(.report, json, on: conn)
                log(String(format: "report: %.1f MB/s p99+%.2fms", report.throughputMBps, report.owdExcessP99Ms))
                onSnapshot?(stats.snapshot())

            case .pong, .ack, .report, .touch:
                break  // これらは iPad→PC 方向。受け取ることはない。
            }
        }
    }

    private func send(_ type: ProbeMessage, _ payload: Data, on conn: NWConnection) {
        conn.send(content: Framing.encode(type, payload), completion: .idempotent)
    }

    private func emitSnapshotIfNeeded(_ nowNs: UInt64) {
        guard let onSnapshot else { return }
        if lastSnapshotNs == 0 || nowNs &- lastSnapshotNs > 100_000_000 {
            lastSnapshotNs = nowNs
            onSnapshot(stats.snapshot())
        }
    }

    private func log(_ message: String) {
        onLog?(message)
    }
}
