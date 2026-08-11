// pc/src/input_injector.cpp -- see input_injector.h for the contract.

#include "input_injector.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// Wheel units injected per iPad point of 2-finger drag. WHEEL_DELTA (120) is
// "one notch"; 4 units/point means ~30 points of drag for one notch, tuned
// for "feels responsive but not twitchy" -- design.md section 3-1 flags this
// as a future config knob, hence the named constant instead of a literal.
constexpr LONG kWheelPerPoint = 4;

// true = content moves the same direction as the dragging fingers (macOS
// "natural" scrolling). See input_injector.h's VerticalWheelDelta /
// HorizontalWheelDelta doc comment for why the two axes have opposite sign
// conventions in Win32 (review 2026-08-11 finding 必須2 -- the original
// single-sign-for-both-axes implementation had vertical scrolling inverted).
constexpr bool kNaturalScroll = true;

INPUT MakeMoveInput(POINT p) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = p.x;
    in.mi.dy = p.y;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    return in;
}

}  // namespace

LONG VerticalWheelDelta(int16_t dy) {
    // MOUSEEVENTF_WHEEL positive = forward rotation = content moves down.
    // Natural scroll: finger down (dy>0) -> content down -> positive.
    const LONG raw = static_cast<LONG>(dy) * kWheelPerPoint;
    return kNaturalScroll ? raw : -raw;
}

LONG HorizontalWheelDelta(int16_t dx) {
    // MOUSEEVENTF_HWHEEL positive = rightward rotation = content moves left.
    // Natural scroll: finger right (dx>0) -> content right -> negative.
    const LONG raw = static_cast<LONG>(dx) * kWheelPerPoint;
    return kNaturalScroll ? -raw : raw;
}

bool ParseTouchMsg(const uint8_t* payload, size_t size, TouchMsg* out) {
    if (!payload || !out || size != 10) return false;
    out->kind = payload[0];
    out->source = payload[1];
    out->x = static_cast<uint16_t>(payload[2] | (payload[3] << 8));
    out->y = static_cast<uint16_t>(payload[4] | (payload[5] << 8));
    out->dx = static_cast<int16_t>(static_cast<uint16_t>(payload[6] | (payload[7] << 8)));
    out->dy = static_cast<int16_t>(static_cast<uint16_t>(payload[8] | (payload[9] << 8)));
    return true;
}

POINT MapToVirtualDesktop(uint16_t x, uint16_t y, RECT target, RECT vscreen) {
    const double tw = static_cast<double>(target.right - target.left);
    const double th = static_cast<double>(target.bottom - target.top);
    const double px = target.left + (x / 65535.0) * std::max(0.0, tw - 1.0);
    const double py = target.top + (y / 65535.0) * std::max(0.0, th - 1.0);

    const double vw = static_cast<double>(vscreen.right - vscreen.left);
    const double vh = static_cast<double>(vscreen.bottom - vscreen.top);
    double nx = vw > 1.0 ? (px - vscreen.left) * 65535.0 / (vw - 1.0) : 0.0;
    double ny = vh > 1.0 ? (py - vscreen.top) * 65535.0 / (vh - 1.0) : 0.0;
    nx = std::clamp(nx, 0.0, 65535.0);
    ny = std::clamp(ny, 0.0, 65535.0);

    POINT out;
    out.x = static_cast<LONG>(std::lround(nx));
    out.y = static_cast<LONG>(std::lround(ny));
    return out;
}

void InputInjector::SetTargetRect(RECT desktop_rect) {
    std::lock_guard<std::mutex> lock(target_mutex_);
    target_ = desktop_rect;
}

void InputInjector::RecordInjectResult(UINT sent, UINT expected) {
    if (sent < expected) {
        inject_errors_.fetch_add(1, std::memory_order_relaxed);
    }
}

void InputInjector::OnTouch(const TouchMsg& m) {
    RECT target;
    {
        std::lock_guard<std::mutex> lock(target_mutex_);
        target = target_;
    }
    RECT vscreen;
    vscreen.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vscreen.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vscreen.right = vscreen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vscreen.bottom = vscreen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    switch (m.kind) {
        case kTouchMove: {
            const POINT p = MapToVirtualDesktop(m.x, m.y, target, vscreen);
            INPUT in = MakeMoveInput(p);
            const UINT sent = SendInput(1, &in, sizeof(INPUT));
            RecordInjectResult(sent, 1);
            moves_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case kTouchTap: {
            const POINT p = MapToVirtualDesktop(m.x, m.y, target, vscreen);
            INPUT ins[3]{};
            ins[0] = MakeMoveInput(p);
            ins[1].type = INPUT_MOUSE;
            ins[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            ins[2].type = INPUT_MOUSE;
            ins[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            const UINT sent = SendInput(3, ins, sizeof(INPUT));
            RecordInjectResult(sent, 3);
            taps_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case kTouchScroll: {
            const POINT p = MapToVirtualDesktop(m.x, m.y, target, vscreen);
            std::vector<INPUT> ins;
            ins.push_back(MakeMoveInput(p));
            if (m.dy != 0) {
                INPUT wi{};
                wi.type = INPUT_MOUSE;
                wi.mi.mouseData = static_cast<DWORD>(VerticalWheelDelta(m.dy));
                wi.mi.dwFlags = MOUSEEVENTF_WHEEL;
                ins.push_back(wi);
            }
            if (m.dx != 0) {
                INPUT hi{};
                hi.type = INPUT_MOUSE;
                hi.mi.mouseData = static_cast<DWORD>(HorizontalWheelDelta(m.dx));
                hi.mi.dwFlags = MOUSEEVENTF_HWHEEL;
                ins.push_back(hi);
            }
            const UINT expected = static_cast<UINT>(ins.size());
            const UINT sent = SendInput(expected, ins.data(), sizeof(INPUT));
            RecordInjectResult(sent, expected);
            scrolls_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        default:
            break;  // unknown kind: forward-compatible no-op
    }
}
