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
//   kind: 1=move 2=tap 3=scroll   source: 0=finger 1=pencil (unused on PC)
//   x,y : 0..65535 normalized position in the iPad's fullscreen view
//   dx,dy: scroll delta in iPad points (kind=3 only, otherwise 0)
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
};

// Decodes the 10-byte touch payload (the DATA-less part after [length][type
// 8]) into *out. Pure function: exact-size check only, no side effects.
// Returns false (leaving *out untouched) when size != 10 or either pointer
// is null.
bool ParseTouchMsg(const uint8_t* payload, size_t size, TouchMsg* out);

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

    // Dispatches on m.kind (move/tap/scroll); unknown kinds are ignored
    // (forward-compatible with future kinds the PC binary predates).
    // Called from the receiver thread (see stream_sender.h's threading
    // contract) -- SendInput is safe to call from any thread.
    void OnTouch(const TouchMsg& m);

    long long moves() const { return moves_.load(std::memory_order_relaxed); }
    long long taps() const { return taps_.load(std::memory_order_relaxed); }
    long long scrolls() const {
        return scrolls_.load(std::memory_order_relaxed);
    }
    // SendInput() returned fewer events injected than requested. Counted,
    // not logged per-occurrence -- SendInput can fail transiently (e.g. a
    // secure desktop / UAC prompt in focus) and touch is a high-rate stream,
    // so per-event logging would flood the log (design.md section 3-1).
    long long inject_errors() const {
        return inject_errors_.load(std::memory_order_relaxed);
    }

private:
    void RecordInjectResult(UINT sent, UINT expected);

    std::mutex target_mutex_;
    RECT target_{0, 0, 1, 1};

    std::atomic<long long> moves_{0};
    std::atomic<long long> taps_{0};
    std::atomic<long long> scrolls_{0};
    std::atomic<long long> inject_errors_{0};
};
