// ConnectionState.swift
//
// PC (Windows) との接続状態を表す最小モデル。
// TransportClient が今後この状態を更新していく想定（現時点では UI 側のプレースホルダ用の値のみ）。

import Foundation

/// PC との接続ライフサイクル。
///
/// - notConnected: 未接続（初期状態・切断後の状態）
/// - connecting: 接続中（ハンドシェイク/ネゴシエーション中）
/// - receiving: 受信中（映像フレームを受信・デコードしている状態）
enum ConnectionState: Equatable {
    case notConnected
    case connecting
    case receiving

    /// UI 表示用のラベル（日本語）。
    var displayLabel: String {
        switch self {
        case .notConnected:
            return "未接続"
        case .connecting:
            return "接続中"
        case .receiving:
            return "受信中"
        }
    }
}
