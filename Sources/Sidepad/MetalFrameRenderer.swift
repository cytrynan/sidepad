// MetalFrameRenderer.swift
//
// デコード済み CVPixelBuffer (NV12) を Metal で画面描画する。
//
// 構成（スレッド境界がこのファイルの核）:
// - LatestFrameStore: decodeキュー側が最新フレームを差し替え、描画側が取り出す受け渡し点。
//   ここが「遅れフレームは捨てて最新表示」の実現箇所（デコードは全フレーム行い、捨てるのは表示だけ）。
// - MetalFrameRenderer (@MainActor): MTKView と Metal オブジェクトを所有し、
//   CVMetalTextureCache で NV12 の2プレーンをゼロコピーでテクスチャ化して
//   BT.709 (video range) → RGB 変換シェーダで全画面 aspect-fit 描画する。
// - VideoRenderView: SwiftUI への出口 (UIViewRepresentable)。
//
// シェーダは実行時コンパイル (makeLibrary(source:))。
// xtool/SwiftPM の .metal リソースコンパイルは未検証の可動部品なので持ち込まない（design.md）。

import CoreVideo
import Foundation
import MetalKit
import SwiftUI

/// decodeキュー → 描画リンク間の最新フレーム受け渡し。FrameRenderer の実装でもある。
final class LatestFrameStore: FrameRenderer, @unchecked Sendable {
    private let lock = NSLock()
    private var latest: CVPixelBuffer?
    private var latestIsNew = false
    private var renderedCount = 0

    // MARK: FrameRenderer（decodeキューから呼ばれる）

    func render(_ pixelBuffer: CVPixelBuffer) {
        lock.lock()
        latest = pixelBuffer      // 前のフレームが未描画ならここで上書き＝表示段のフレーム破棄
        latestIsNew = true
        lock.unlock()
    }

    func resize(to size: CGSize) {}  // アスペクトは描画時に毎回計算するので何もしない

    // MARK: 描画側

    /// 最新フレームを覗く（状態は変えない）。
    /// 描画が最後まで成立するか分からない時点で「描画済み」を消費しないため、
    /// take ではなく peek + commitRendered の2段にしている（レビュー指摘M1:
    /// drawable枯渇・テクスチャ化失敗で early return すると、フレームが一度も
    /// 表示されないまま描画済み扱いになり、統計が受入判定を誤らせる）。
    func peek() -> CVPixelBuffer? {
        lock.lock()
        defer { lock.unlock() }
        return latest
    }

    /// 描画コマンドの発行まで成立した時に呼ぶ。まだ新規だったフレームなら描画済みとして数える。
    func commitRendered(_ buffer: CVPixelBuffer) {
        lock.lock()
        if latestIsNew, latest === buffer {
            latestIsNew = false
            renderedCount += 1
        }
        lock.unlock()
    }

    /// 新規フレームとして画面描画された通算数（＝取りこぼしなく表示できているかの計器）。
    var renderedFrames: Int {
        lock.lock()
        defer { lock.unlock() }
        return renderedCount
    }

    /// 計測リセット（PC側の RESET メッセージ受信時）。表示中のフレームはそのまま。
    func resetRenderedCount() {
        lock.lock()
        renderedCount = 0
        lock.unlock()
    }
}

@MainActor
final class MetalFrameRenderer: NSObject, MTKViewDelegate {
    let view: MTKView
    private let store: LatestFrameStore
    private let commandQueue: MTLCommandQueue
    private let pipelineState: MTLRenderPipelineState
    private var textureCache: CVMetalTextureCache?

    /// 初期化失敗（Metal不可・シェーダ不成立）は nil。実機では起きない想定だが黙らせない。
    init?(store: LatestFrameStore, onLog: ((String) -> Void)? = nil) {
        guard let device = MTLCreateSystemDefaultDevice(),
              let queue = device.makeCommandQueue()
        else {
            onLog?("Metal デバイス生成失敗")
            return nil
        }
        self.store = store
        self.commandQueue = queue

        do {
            let library = try device.makeLibrary(source: Self.shaderSource, options: nil)
            let desc = MTLRenderPipelineDescriptor()
            desc.vertexFunction = library.makeFunction(name: "sidepadVertex")
            desc.fragmentFunction = library.makeFunction(name: "sidepadFragment")
            desc.colorAttachments[0].pixelFormat = .bgra8Unorm
            self.pipelineState = try device.makeRenderPipelineState(descriptor: desc)
        } catch {
            onLog?("シェーダコンパイル失敗: \(error)")
            return nil
        }

        CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &textureCache)

        let mtkView = MTKView(frame: .zero, device: device)
        mtkView.framebufferOnly = true
        mtkView.colorPixelFormat = .bgra8Unorm
        mtkView.preferredFramesPerSecond = 120  // ProMotion 追従（60Hzソースでも害なし）
        mtkView.backgroundColor = .black
        self.view = mtkView

        super.init()
        mtkView.delegate = self
    }

    // MARK: - MTKViewDelegate

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        // 消費（描画済みマーク）はコマンド発行が成立してから。ここでは覗くだけ。
        guard let pixelBuffer = store.peek(),
              let drawable = view.currentDrawable,
              let passDescriptor = view.currentRenderPassDescriptor,
              let textureCache
        else { return }

        let width = CVPixelBufferGetWidth(pixelBuffer)
        let height = CVPixelBufferGetHeight(pixelBuffer)

        // NV12: plane0 = Y (r8), plane1 = CbCr (rg8)。プレーン寸法は実値を使う（奇数解像度対策）
        var yTexRef: CVMetalTexture?
        var cbcrTexRef: CVMetalTexture?
        CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, textureCache, pixelBuffer, nil,
            .r8Unorm,
            CVPixelBufferGetWidthOfPlane(pixelBuffer, 0),
            CVPixelBufferGetHeightOfPlane(pixelBuffer, 0),
            0, &yTexRef
        )
        CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, textureCache, pixelBuffer, nil,
            .rg8Unorm,
            CVPixelBufferGetWidthOfPlane(pixelBuffer, 1),
            CVPixelBufferGetHeightOfPlane(pixelBuffer, 1),
            1, &cbcrTexRef
        )
        guard let yTexRef, let cbcrTexRef,
              let yTexture = CVMetalTextureGetTexture(yTexRef),
              let cbcrTexture = CVMetalTextureGetTexture(cbcrTexRef),
              let commandBuffer = commandQueue.makeCommandBuffer(),
              let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: passDescriptor)
        else { return }

        // aspect-fit: 画像とビューの縦横比から NDC スケールを決める
        let drawableSize = view.drawableSize
        var scale = SIMD2<Float>(1, 1)
        if drawableSize.width > 0, drawableSize.height > 0, width > 0, height > 0 {
            let imageAspect = Float(width) / Float(height)
            let viewAspect = Float(drawableSize.width) / Float(drawableSize.height)
            if imageAspect > viewAspect {
                scale = SIMD2<Float>(1, viewAspect / imageAspect)
            } else {
                scale = SIMD2<Float>(imageAspect / viewAspect, 1)
            }
        }

        encoder.setRenderPipelineState(pipelineState)
        encoder.setVertexBytes(&scale, length: MemoryLayout<SIMD2<Float>>.size, index: 0)
        encoder.setFragmentTexture(yTexture, index: 0)
        encoder.setFragmentTexture(cbcrTexture, index: 1)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        encoder.endEncoding()

        // CVMetalTexture は GPU 実行完了まで生かす必要がある（Sendable な箱に入れて寿命を延ばす）
        let refs = TextureLifetimeBox([yTexRef, cbcrTexRef])
        commandBuffer.addCompletedHandler { _ in refs.release() }
        commandBuffer.present(drawable)
        commandBuffer.commit()
        store.commitRendered(pixelBuffer)
        // 使い終わったテクスチャキャッシュのリサイクルを促す（VT側のバッファプール圧迫を防ぐ定石）
        CVMetalTextureCacheFlush(textureCache, 0)
    }

    // MARK: - シェーダ（実行時コンパイル）

    private static let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;

    struct VSOut {
        float4 position [[position]];
        float2 uv;
    };

    vertex VSOut sidepadVertex(uint vid [[vertex_id]],
                               constant float2 &scale [[buffer(0)]]) {
        // triangle strip の全画面クアッド
        float2 corners[4] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,1) };
        float2 p = corners[vid];
        VSOut out;
        out.position = float4(p * scale, 0.0, 1.0);
        out.uv = float2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5);
        return out;
    }

    fragment float4 sidepadFragment(VSOut in [[stage_in]],
                                    texture2d<float> yTex [[texture(0)]],
                                    texture2d<float> cbcrTex [[texture(1)]]) {
        constexpr sampler s(mag_filter::linear, min_filter::linear);
        // BT.709 video range (16-235 / 16-240) → RGB
        float y = (yTex.sample(s, in.uv).r - 16.0/255.0) * (255.0/219.0);
        float2 cbcr = (cbcrTex.sample(s, in.uv).rg - 128.0/255.0) * (255.0/224.0);
        float3 rgb = float3(
            y + 1.5748 * cbcr.y,
            y - 0.1873 * cbcr.x - 0.4681 * cbcr.y,
            y + 1.8556 * cbcr.x
        );
        return float4(clamp(rgb, 0.0, 1.0), 1.0);
    }
    """
}

/// GPU 実行完了まで CVMetalTexture を保持するための箱。
/// addCompletedHandler の @Sendable 要件を満たすために用意する（中身には触らない）。
private final class TextureLifetimeBox: @unchecked Sendable {
    private var refs: [CVMetalTexture]?

    init(_ refs: [CVMetalTexture]) {
        self.refs = refs
    }

    func release() {
        refs = nil
    }
}

/// SwiftUI から Metal 描画ビューを使うためのラッパ。
struct VideoRenderView: UIViewRepresentable {
    let renderer: MetalFrameRenderer

    func makeUIView(context: Context) -> MTKView {
        renderer.view
    }

    func updateUIView(_ uiView: MTKView, context: Context) {}
}
