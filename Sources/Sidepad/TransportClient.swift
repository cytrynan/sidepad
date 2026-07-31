// TransportClient.swift
//
// PC (Windows) からの映像データ受信経路を抽象化するプロトコル。
// USB C-C データ通信（本命）／有線LAN(TCP)フォールバック／将来的な Wi-Fi モード(P2)
// のいずれも同じインターフェースで扱えるようにするための受け口。
//
// 実装予定（未実装・このファイルはスタブのみ）:
// - USBTransportClient: USB経由のデータ通信（Network.framework の NWConnection や
//   ExternalAccessory framework 等、実装方式は要検討）
// - TCPTransportClient: USB-C→Ethernetアダプタ経由の有線LAN、または Wi-Fi モード
//
// 受信したエンコード済みフレームデータは onFrameData で通知し、
// 呼び出し側（後続実装で VideoDecoder に渡す想定）がデコードを担当する。

import Foundation

protocol TransportClient: AnyObject {
    /// 現在の接続状態。
    var state: ConnectionState { get }

    /// 接続状態が変化した際に呼ばれる。
    var onStateChange: ((ConnectionState) -> Void)? { get set }

    /// PC から受信した生データ（エンコード済み映像フレーム等）を通知する。
    var onFrameData: ((Data) -> Void)? { get set }

    /// 接続を開始する（ハンドシェイク〜受信ループの開始）。
    func start()

    /// 接続を終了する。
    func stop()
}

/// 何もしないスタブ実装。
/// UI やアプリ全体の配線を先に組めるようにするための仮実装で、
/// 実際の通信ロジックは持たない。
final class StubTransportClient: TransportClient {
    private(set) var state: ConnectionState = .notConnected {
        didSet { onStateChange?(state) }
    }

    var onStateChange: ((ConnectionState) -> Void)?
    var onFrameData: ((Data) -> Void)?

    func start() {
        // TODO: 実装 — USB/TCP 接続の確立、state を .connecting → .receiving へ遷移させる
    }

    func stop() {
        // TODO: 実装 — 接続のクローズ、state を .notConnected へ遷移させる
        state = .notConnected
    }
}
