// TouchInput.swift
//
// iPad→PC のタッチ入力返送。ワイヤフォーマットの正本は
// .steering/20260811-touch-input/design.md §1。
//
// TouchEvent/TouchWire は純関数のみ（UIKit非依存）にして、Windows swiftc でも
// 単体テストできるようにする（xtool/tests/touchwire_main.swift）。
// TouchCatcher（UIViewRepresentable）だけ #if canImport(UIKit) で囲み、
// 非Appleホストでの swiftc コンパイル時にスキップされるようにする
// （AnnexB.swift 方式に倣い、ConnectionView.swift 側の UIKit 実装は
// クロスプラットフォームテスト対象に含めない）。

import Foundation

/// タッチメッセージの種別。ProbeMessage.touch の payload 先頭バイト。
enum TouchKind: UInt8 {
    /// 1本指ドラッグ中のカーソル移動（左ボタンドラッグ中の移動もこれ）。x,y は絶対位置。
    case move = 1
    /// タップ確定。x,y はタップ位置。
    case tap = 2
    /// 2本指スクロール。x,y は2指の重心、dx,dy は前回からの移動量。
    case scroll = 3
    /// 長押し確定。x,y で左ボタン押下。2026-08-11 K承認の追加（design.md §1「長押しドラッグ」節）。
    case dragBegin = 4
    /// 長押しドラッグ終了。x,y へ移動して左ボタン解放。
    case dragEnd = 5
}

/// タッチの発生源。PC側は当面同一扱い（ログ区別のためだけに送る）。
enum TouchSource: UInt8 {
    case finger = 0
    case pencil = 1
}

/// iPad→PC で送るタッチイベント1件。
struct TouchEvent: Equatable {
    var kind: TouchKind
    var source: TouchSource
    /// 0..65535 に正規化された座標。ビューの bounds 基準、左上=(0,0)・右下=(65535,65535)。
    var x: UInt16
    var y: UInt16
    /// スクロール移動量（iPadのポイント単位、符号付き）。kind=scroll 以外は 0。
    var dx: Int16
    var dy: Int16
}

/// TouchEvent ⇄ ワイヤバイト列（固定10バイト）の純関数コーデック。
enum TouchWire {
    static let payloadSize = 10

    static func encode(_ event: TouchEvent) -> Data {
        var out = Data(capacity: payloadSize)
        out.append(event.kind.rawValue)
        out.append(event.source.rawValue)
        out.append(littleEndian: event.x)
        out.append(littleEndian: event.y)
        out.append(littleEndian: UInt16(bitPattern: event.dx))
        out.append(littleEndian: UInt16(bitPattern: event.dy))
        return out
    }

    static func decode(_ payload: Data) -> TouchEvent? {
        guard payload.count == payloadSize else { return nil }
        guard let kind = TouchKind(rawValue: payload[payload.startIndex]),
              let source = TouchSource(rawValue: payload[payload.startIndex + 1]) else {
            return nil
        }
        let x = payload.readUInt16(at: 2)
        let y = payload.readUInt16(at: 4)
        let dx = Int16(bitPattern: payload.readUInt16(at: 6))
        let dy = Int16(bitPattern: payload.readUInt16(at: 8))
        return TouchEvent(kind: kind, source: source, x: x, y: y, dx: dx, dy: dy)
    }
}

/// point の1成分（x または y）を [0, extent) の範囲で 0..65535 へ正規化する純関数。
/// extent<=0（bounds未確定）の場合は呼び出し側で送信自体を止めること。
enum TouchNormalize {
    static func coordinate(_ value: CGFloat, extent: CGFloat) -> UInt16 {
        guard extent > 0 else { return 0 }
        let ratio = value / extent
        let scaled = (ratio * 65535).rounded()
        return UInt16(clamping: Int(scaled))
    }
}

#if canImport(UIKit)
import SwiftUI
import UIKit

/// 1本指タップ・1本指ドラッグ(move)・2本指ドラッグ(scroll)・3本指タップ(統計オーバーレイのトグル)を
/// 同一UIView上の4つの recognizer で捕捉し、それぞれのコールバックへ送出する。
///
/// design.md §2-3（2026-08-11 レビュー必須1で改訂）: ZStack の兄弟ビューは hitTest で前面が全タッチを
/// 取ってしまい、背面のレイヤにはジェスチャが一切届かない（初版の「背面に置けば3本指が先取り」は
/// UIKitの挙動と不一致だった）。そのため捕捉層は1枚に統合し、旧 ThreeFingerTapCatcher の役割も
/// このビューの tap(3本指) recognizer に統合する。
struct TouchCatcher: UIViewRepresentable {
    let onEvent: (TouchEvent) -> Void
    /// 3本指タップ時に呼ばれる（旧 ThreeFingerTapCatcher の役割）。
    let onToggleOverlay: () -> Void

    /// move/scroll のレート制限間隔。design.md §2-1: 最小間隔8ms（最終イベントは必ず送る）。
    static let minInterval: TimeInterval = 0.008

    private static let allowedTypes: [NSNumber] = [
        NSNumber(value: UITouch.TouchType.direct.rawValue),
        NSNumber(value: UITouch.TouchType.pencil.rawValue),
    ]

    func makeUIView(context: Context) -> UIView {
        let view = UIView()
        view.backgroundColor = .clear
        context.coordinator.view = view

        // 3本指タップ = 統計オーバーレイのトグル（旧 ThreeFingerTapCatcher）。
        // 他の3つより先に生成し、tap(1本指)側の require(toFail:) から参照できるようにする。
        let tap3 = UITapGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.toggled(_:)))
        tap3.numberOfTouchesRequired = 3
        tap3.delegate = context.coordinator
        view.addGestureRecognizer(tap3)

        let tap = UITapGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.tapped(_:)))
        tap.numberOfTouchesRequired = 1
        tap.allowedTouchTypes = Self.allowedTypes
        tap.delegate = context.coordinator
        // 3本指トグル時の誤クリック防止（design.md §2-3）。タップ確定は指を離した時なので
        // 実用上の遅延は生じない。
        tap.require(toFail: tap3)
        view.addGestureRecognizer(tap)

        let movePan = UIPanGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.moved(_:)))
        movePan.minimumNumberOfTouches = 1
        movePan.maximumNumberOfTouches = 1
        movePan.allowedTouchTypes = Self.allowedTypes
        movePan.delegate = context.coordinator
        view.addGestureRecognizer(movePan)

        let scrollPan = UIPanGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.scrolled(_:)))
        scrollPan.minimumNumberOfTouches = 2
        scrollPan.maximumNumberOfTouches = 2
        scrollPan.allowedTouchTypes = Self.allowedTypes
        scrollPan.delegate = context.coordinator
        view.addGestureRecognizer(scrollPan)

        // 長押しドラッグ（2026-08-11 K承認・design.md §1「長押しドラッグ」節）。
        // 認識器間の排他はUIKit既定に任せる: 指が静止したまま0.35秒経てばlongPressがbeginし、
        // pan(1)/tap(1)はfailする。素早く動かせば逆にpanが先に走ってlongPressがfailする
        // （＝従来のカーソル移動のまま）。よって require(toFail:) 等の明示的な排他指定は不要。
        let longPress = UILongPressGestureRecognizer(
            target: context.coordinator, action: #selector(Coordinator.longPressed(_:)))
        longPress.minimumPressDuration = 0.35
        longPress.numberOfTouchesRequired = 1
        longPress.allowedTouchTypes = Self.allowedTypes
        longPress.delegate = context.coordinator
        view.addGestureRecognizer(longPress)

        return view
    }

    func updateUIView(_ uiView: UIView, context: Context) {
        context.coordinator.onEvent = onEvent
        context.coordinator.onToggleOverlay = onToggleOverlay
    }

    func makeCoordinator() -> Coordinator { Coordinator(onEvent: onEvent, onToggleOverlay: onToggleOverlay) }

    final class Coordinator: NSObject, UIGestureRecognizerDelegate {
        var onEvent: (TouchEvent) -> Void
        var onToggleOverlay: () -> Void
        weak var view: UIView?

        private var lastTouchType: UITouch.TouchType = .direct
        private var lastMoveSentAt: TimeInterval = 0
        private var lastScrollSentAt: TimeInterval = 0
        private var lastScrollTranslation: CGPoint = .zero

        init(onEvent: @escaping (TouchEvent) -> Void, onToggleOverlay: @escaping () -> Void) {
            self.onEvent = onEvent
            self.onToggleOverlay = onToggleOverlay
        }

        // UIGestureRecognizerDelegate: 各ジェスチャが着地する直前に触れた指の種類を控える
        // （UIGestureRecognizer 自体は touch を公開しないため、この経路で拾う）。
        func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer, shouldReceive touch: UITouch) -> Bool {
            lastTouchType = touch.type
            return true
        }

        @objc func toggled(_ gesture: UITapGestureRecognizer) {
            onToggleOverlay()
        }

        @objc func tapped(_ gesture: UITapGestureRecognizer) {
            guard let view else { return }
            emit(kind: .tap, point: gesture.location(in: view))
        }

        @objc func moved(_ gesture: UIPanGestureRecognizer) {
            guard let view else { return }
            switch gesture.state {
            case .began:
                lastMoveSentAt = 0  // 最初の1発は必ず送る
            case .changed:
                let now = CACurrentMediaTime()
                guard now - lastMoveSentAt >= TouchCatcher.minInterval else { return }
                lastMoveSentAt = now
            case .ended, .cancelled:
                break  // 最終イベントは必ず送る（取りこぼし対策。design.md §2-1）
            default:
                return
            }
            emit(kind: .move, point: gesture.location(in: view))
        }

        @objc func longPressed(_ gesture: UILongPressGestureRecognizer) {
            guard let view else { return }
            switch gesture.state {
            case .began:
                // pan(1)の move と同じレート制限状態を共有する（design.md §1「長押しドラッグ」節:
                // changed=kind=1 move（8msレート制限共通）。直後のchangedを必ず送れるようリセット）。
                lastMoveSentAt = 0
                emit(kind: .dragBegin, point: gesture.location(in: view))
            case .changed:
                let now = CACurrentMediaTime()
                guard now - lastMoveSentAt >= TouchCatcher.minInterval else { return }
                lastMoveSentAt = now
                emit(kind: .move, point: gesture.location(in: view))
            case .ended, .cancelled:
                // 最終イベントは必ず送る（フェイルセーフ。PC側 ReleaseDragIfActive の前提=dragEnd到達）
                emit(kind: .dragEnd, point: gesture.location(in: view))
            default:
                return
            }
        }

        @objc func scrolled(_ gesture: UIPanGestureRecognizer) {
            guard let view else { return }
            switch gesture.state {
            case .began:
                lastScrollSentAt = 0
                lastScrollTranslation = .zero
                return  // began 時点では移動量ゼロなので送るものが無い
            case .changed:
                let now = CACurrentMediaTime()
                guard now - lastScrollSentAt >= TouchCatcher.minInterval else { return }
                lastScrollSentAt = now
            case .ended, .cancelled:
                break  // 最終イベントは必ず送る
            default:
                return
            }

            // setTranslation(.zero, in:) はしない（velocity計算を壊すため）。
            // 自前で前回値との差分を取り、前回値を更新するだけにする。design.md §2-1
            let translation = gesture.translation(in: view)
            let dxRaw = translation.x - lastScrollTranslation.x
            let dyRaw = translation.y - lastScrollTranslation.y
            lastScrollTranslation = translation
            guard dxRaw != 0 || dyRaw != 0 else { return }

            emit(
                kind: .scroll,
                point: gesture.location(in: view),
                dx: Int16(clamping: Int(dxRaw.rounded())),
                dy: Int16(clamping: Int(dyRaw.rounded()))
            )
        }

        private func emit(kind: TouchKind, point: CGPoint, dx: Int16 = 0, dy: Int16 = 0) {
            guard let view, view.bounds.width > 0, view.bounds.height > 0 else { return }
            let x = TouchNormalize.coordinate(point.x, extent: view.bounds.width)
            let y = TouchNormalize.coordinate(point.y, extent: view.bounds.height)
            let source: TouchSource = lastTouchType == .pencil ? .pencil : .finger
            onEvent(TouchEvent(kind: kind, source: source, x: x, y: y, dx: dx, dy: dy))
        }
    }
}
#endif
