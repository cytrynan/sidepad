// FrameRenderer.swift
//
// VideoDecoder が出力した CVPixelBuffer を画面に描画する責務の抽象化。
//
// 実装予定（未実装・このファイルはスタブのみ）:
// - MetalFrameRenderer: Metal (CVMetalTextureCache 経由でのゼロコピー描画) による
//   低遅延レンダリング。ProMotion 120Hz への追従を想定。

import CoreVideo
import Foundation

protocol FrameRenderer: AnyObject {
    /// デコード済みフレームを描画する。
    func render(_ pixelBuffer: CVPixelBuffer)

    /// 描画対象がリサイズされた際に呼ばれる（レイアウト変更・画面回転など）。
    func resize(to size: CGSize)
}

/// 何もしないスタブ実装。
/// Metal デバイス・CVMetalTextureCache のセットアップは
/// 実際の描画ビュー（MTKView 等）と合わせて後続実装で追加する。
final class StubFrameRenderer: FrameRenderer {
    func render(_ pixelBuffer: CVPixelBuffer) {
        // TODO: 実装 — Metal でのテクスチャ変換・描画
    }

    func resize(to size: CGSize) {
        // TODO: 実装 — 描画先サイズの更新
    }
}
