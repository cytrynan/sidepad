// VideoDecoder.swift
//
// TransportClient から受け取ったエンコード済み映像データを、
// 画面描画可能な CVPixelBuffer にデコードする責務の抽象化。
//
// 実装予定（未実装・このファイルはスタブのみ）:
// - VTVideoDecoder: VideoToolbox (VTDecompressionSession) によるハードウェアデコード
//   （Windows側 AMF H.264/H.265 エンコードに対応する低遅延デコード設定を想定）

import CoreVideo
import Foundation

protocol VideoDecoder: AnyObject {
    /// エンコード済みフレームデータをデコードし、結果を completion で返す。
    /// デコードに失敗した場合は nil を返す。
    func decode(_ data: Data, completion: @escaping (CVPixelBuffer?) -> Void)

    /// デコーダの内部状態（VTDecompressionSession 等）をリセットする。
    func reset()
}

/// 何もしないスタブ実装。
/// VideoToolbox のセッション初期化・フォーマット記述（CMFormatDescription）は
/// 実データ（SPS/PPS 等）が必要なため、後続実装で追加する。
final class StubVideoDecoder: VideoDecoder {
    func decode(_ data: Data, completion: @escaping (CVPixelBuffer?) -> Void) {
        // TODO: 実装 — VTDecompressionSession でのデコード
        completion(nil)
    }

    func reset() {
        // TODO: 実装 — セッションの破棄・再生成
    }
}
