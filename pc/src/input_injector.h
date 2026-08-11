// pc/src/input_injector.h
//
// Injects PC mouse input from touch messages received over the reverse
// (iPad -> PC) leg of the existing probe wire (type=8; see
// .steering/20260811-touch-input/design.md sections 1 and 3).
//
// Wire payload (10 bytes, little-endian, matches Sources/Sidepad/
// TouchInput.swift's TouchWire.encode exactly -- see the golden byte
// listing in design.md section 1, mirrored in ParseTouchMsgTest usage and
// pc/probe/verify-touch-inject.ps1):
//   [u8 kind][u8 source][u16 x][u16 y][i16 dx][i16 dy]
//   kind: 1=move 2=tap 3=scroll 4=dragBegin 5=dragEnd
//         source: 0=finger 1=pencil (unused on PC)
//   x,y : 0..65535 normalized position in the iPad's fullscreen view
//   dx,dy: scroll delta in iPad points (kind=3 only, otherwise 0)
//
// Long-press drag (2026-08-11 K-approved addition, design.md section 1
// "長押しドラッグ"): kind=4 (dragBegin) presses the left button down at
// x,y; the drag's own movement then rides on ordinary kind=1 (move)
// messages -- PC-side OnTouch needs no special handling for that, because
// MOUSEEVENTF_MOVE alone (no button flags) simply moves the cursor while
// SendInput's already-down left button state carries the drag. kind=5
// (dragEnd) releases at x,y.
//
// ParseTouchMsg and MapToVirtualDesktop are pure functions on purpose (no
// Windows API calls, no state) so they can be desk-checked against the
// golden bytes and independently-computed expected coordinates without
// ever calling SendInput -- see the design doc's judge-independence note
// (section 3-4) and pc/probe/verify-touch-inject.ps1.
//
// Threading contract: OnTouch is called from StreamSender's receiver
// thread (see stream_sender.h). SendInput is thread-free, so no locking is
// needed around the injection calls themselves; SetTargetRect (called once
// from the controlling thread after InitCapture) and OnTouch's read of the
// target rect are still mutex-guarded because they run on different
// threads with no other synchronization between them.

#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

// One decoded touch message (see the wire layout above).
struct TouchMsg {
    uint8_t kind = 0;
    uint8_t source = 0;
    uint16_t x = 0;
    uint16_t y = 0;
    int16_t dx = 0;
    int16_t dy = 0;
};

enum TouchKind : uint8_t {
    kTouchMove = 1,
    kTouchTap = 2,
    kTouchScroll = 3,
    kTouchDragBegin = 4,
    kTouchDragEnd = 5,
};

// Decodes the 10-byte touch payload (the DATA-less part after [length][type
// 8]) into *out. Pure function: exact-size check only, no side effects.
// Returns false (leaving *out untouched) when size != 10 or either pointer
// is null.
bool ParseTouchMsg(const uint8_t* payload, size_t size, TouchMsg* out);

// Per-axis wheel delta from a 2-finger drag distance in iPad points.
// Pure functions (no Windows calls) so the polarity can be self-tested
// without ever calling SendInput -- see pc_encode_spike's --touch-selftest
// and pc/probe/verify-touch-inject.ps1.
//
// design.md section 3-1 (revised in review 2026-08-11 finding 必須2, the
// original text was self-contradictory): natural scrolling (content moves
// the same direction as the dragging fingers) has OPPOSITE sign convention
// on the two axes, because Win32's own wheel-message polarity is opposite
// on the two axes:
//   vertical:   MOUSEEVENTF_WHEEL positive  = forward rotation = content
//               moves DOWN. Finger down (dy>0) -> content down -> POSITIVE.
//               VerticalWheelDelta(dy)   == +dy * kWheelPerPoint
//   horizontal: MOUSEEVENTF_HWHEEL positive = rightward rotation = content
//               moves LEFT. Finger right (dx>0) -> content right -> NEGATIVE.
//               HorizontalWheelDelta(dx) == -dx * kWheelPerPoint
// kNaturalScroll flips both signs together when false (traditional
// scrolling); kWheelPerPoint and kNaturalScroll are defined at the top of
// input_injector.cpp.
LONG VerticalWheelDelta(int16_t dy);
LONG HorizontalWheelDelta(int16_t dx);

// Two-stage coordinate transform (design.md section 3-1):
//   1) x,y (0..65535 normalized within the iPad view) -> pixel position
//      inside `target` (the VDD output's DesktopCoordinates).
//   2) that pixel position -> 0..65535 normalized within `vscreen` (the
//      Windows virtual screen: SM_XVIRTUALSCREEN/SM_YVIRTUALSCREEN as
//      vscreen.left/top, SM_CXVIRTUALSCREEN/SM_CYVIRTUALSCREEN added to get
//      vscreen.right/bottom), which is what MOUSEEVENTF_ABSOLUTE |
//      MOUSEEVENTF_VIRTUALDESK expects.
// Pure function: takes both rects as parameters instead of calling
// GetSystemMetrics itself, so it can be desk-checked with fixed inputs.
// Both target and vscreen must have width/height >= 2 (typical monitor
// sizes always do); degenerate rects clamp to the rect's origin.
POINT MapToVirtualDesktop(uint16_t x, uint16_t y, RECT target, RECT vscreen);

// Injects one touch message as PC mouse input via SendInput. Not a pure
// function -- this is the one piece of section 3-4's "local machine
// verification" that actually moves the cursor, so it is exercised only by
// pc/probe/verify-touch-inject.ps1 (never run automatically; see that
// script's header) and never by the desk-check tests for ParseTouchMsg /
// MapToVirtualDesktop above.
class InputInjector {
public:
    InputInjector() = default;
    InputInjector(const InputInjector&) = delete;
    InputInjector& operator=(const InputInjector&) = delete;

    // The VDD output's DesktopCoordinates (screen-pixel RECT). Call once
    // after InitCapture (Pipeline); before the first call OnTouch maps
    // everything into a degenerate 1x1 rect at (0,0), which is harmless
    // (SendInput still fires, just to the wrong place) rather than crashing
    // on an unset target.
    void SetTargetRect(RECT desktop_rect);

    // Dispatches on m.kind (move/tap/scroll/dragBegin/dragEnd); unknown
    // kinds are ignored (forward-compatible with future kinds the PC binary
    // predates). Called from the receiver thread (see stream_sender.h's
    // threading contract) -- SendInput is safe to call from any thread.
    void OnTouch(const TouchMsg& m);

    // --- Long-press drag fail-safe (design.md section 1; review 2026-08-11
    // findings 必須1/必須3) ----------------------------------------------
    //
    // A dropped/never-sent dragEnd must not leave the left button stuck
    // down -- the worst failure mode, since it makes the PC itself
    // unusable. Two hazards had to be closed; see TransitionDragLocked()'s
    // doc comment (private section below) for the full explanation of
    // both. This TestTransitionDrag() entry point exists purely so
    // --touch-selftest (pc/src/encode_main.cpp) can drive that SAME locked
    // state machine -- transitions AND the failure/rollback path -- with
    // `simulated_success` standing in for a real SendInput's result,
    // proving the fail-safe's logic without a single SendInput call. Never
    // used by OnTouch or ReleaseDragIfActive themselves -- those always
    // inject for real.
    bool TestTransitionDrag(bool begin, bool simulated_success) {
        return TransitionDragLocked(begin, [simulated_success] { return simulated_success; });
    }

    bool DragActive() const {
        std::lock_guard<std::mutex> lock(drag_mutex_);
        return drag_active_;
    }
    // Fail-safe entry point: call from Pipeline::Stop() (and once more from
    // ~Pipeline() after the receiver thread is fully joined -- see
    // pipeline.cpp) so a session that ends (detach / send failure / quit /
    // session-limit) with a drag still held never leaves the PC's left
    // button stuck down. Idempotent and safe to call even when no drag is
    // active (TransitionDragLocked sees nothing to end, no SendInput call
    // happens at all). Safe to call repeatedly even after a failed release
    // (必須3): the state stays active until a release actually succeeds.
    void ReleaseDragIfActive();

    long long moves() const { return moves_.load(std::memory_order_relaxed); }
    long long taps() const { return taps_.load(std::memory_order_relaxed); }
    long long scrolls() const {
        return scrolls_.load(std::memory_order_relaxed);
    }
    // Completed drags (a real, successful TransitionDragLocked(begin=false,
    // ...) transition, whether from a genuine dragEnd message or the
    // ReleaseDragIfActive() fail-safe). A failed release (必須3) is NOT
    // counted here -- it rolls back and stays retryable instead.
    long long drags() const { return drags_.load(std::memory_order_relaxed); }
    // SendInput() returned fewer events injected than requested. Counted,
    // not logged per-occurrence -- SendInput can fail transiently (e.g. a
    // secure desktop / UAC prompt in focus) and touch is a high-rate stream,
    // so per-event logging would flood the log (design.md section 3-1).
    long long inject_errors() const {
        return inject_errors_.load(std::memory_order_relaxed);
    }

private:
    // A dropped/never-sent dragEnd must not leave the left button stuck
    // down -- the worst failure mode, since it makes the PC itself
    // unusable. Two hazards had to be closed:
    //
    //   必須1 (non-atomic state+injection): an earlier version did the
    //   drag_active_ compare-exchange and the SendInput call as two
    //   separate steps. That let OnTouch's dragBegin (CAS succeeds ->
    //   about to call SendInput(LEFTDOWN)) race against a concurrent
    //   Pipeline::Stop() -> ReleaseDragIfActive() on another thread: the
    //   release could run in the gap and see drag_active_ already back to
    //   false, so it skips LEFTUP -- while the real LEFTDOWN still lands a
    //   moment later. Result: physical button down, drag_active_ == false,
    //   nobody left to release it. Fixed here by holding drag_mutex_
    //   across BOTH the state check/set AND the injection callback, for
    //   all three entry points (OnTouch's dragBegin, OnTouch's dragEnd,
    //   ReleaseDragIfActive) -- they cannot interleave with each other.
    //
    //   必須3 (injection failure disarms the fail-safe): if SendInput
    //   under-delivers (sent < expected) on the release side, the old code
    //   still flipped drag_active_ to false first, so a real LEFTUP
    //   failure left the button down but the fail-safe "off" --
    //   unrecoverable, no retry. This now rolls the tentative transition
    //   back (drag_active_ reverts to its pre-call value) whenever the
    //   injection callback reports failure, so the SAME dragEnd retry or a
    //   later ReleaseDragIfActive() call can still see drag_active_ == true
    //   and try again.
    //
    // Parameterized on the injection step (a callable, not a hardcoded
    // SendInput call) so --touch-selftest can drive this exact state
    // machine -- including the failure/rollback path -- through the public
    // TestTransitionDrag() wrapper, with zero SendInput calls.
    //
    // `begin`: true = dragBegin direction (inactive->active), false =
    // dragEnd/ReleaseDragIfActive direction (active->inactive).
    // `inject`: called ONLY when a real transition is happening (skips
    // duplicate-begin / nothing-to-end automatically, so SendInput is never
    // called for a no-op) with drag_mutex_ held; must return true iff the
    // injection fully succeeded (sent == expected). Returns true iff a
    // real, successful transition happened (state committed); false for
    // either "nothing to do" or "injection failed and was rolled back".
    bool TransitionDragLocked(bool begin, const std::function<bool()>& inject);

    void RecordInjectResult(UINT sent, UINT expected);

    std::mutex target_mutex_;
    RECT target_{0, 0, 1, 1};

    // Guards drag_active_ AND every injection callback run through
    // TransitionDragLocked -- see that method's doc comment (必須1/必須3).
    // mutable: DragActive() is const but still needs the lock to read a
    // plain (non-atomic) bool safely.
    mutable std::mutex drag_mutex_;
    bool drag_active_ = false;  // guarded by drag_mutex_, NOT atomic anymore

    std::atomic<long long> moves_{0};
    std::atomic<long long> taps_{0};
    std::atomic<long long> scrolls_{0};
    std::atomic<long long> drags_{0};
    std::atomic<long long> inject_errors_{0};
};
