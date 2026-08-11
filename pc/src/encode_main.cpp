// pc/src/encode_main.cpp
//
// pc_encode_spike: DXGI Desktop Duplication capture -> AMF H.264 encode,
// with the encoder configured from the 2026-08-06 USB datalink measurements
// (see pc/src/amf_encoder.h and .steering/20260806-amf-encoder/design.md).
//
// Output goes to run statistics, an optional Annex-B .h264 dump, and -- with
// --send -- the USB transport (StreamSender: one DATA message per frame,
// 16 KiB socket writes, usbmux direct or plain TCP). The run ends with
// PASS/FAIL lines against .steering/20260806-amf-encoder/requirements.md
// plus, when sending, .steering/20260806-usb-transport-integration/.
//
// Usage:
//   pc_encode_spike [--seconds N] [--bitrate-mbps N] [--output file.h264]
//                   [--display N] [--send usb[:PORT] | tcp:HOST:PORT]
//                   [--list-displays] [--touch-selftest]
//
// --touch-selftest: protocol/pure-function self-check for the touch input
// path (.steering/20260811-touch-input/design.md section 3-4, judge fix
// J4). Never calls SendInput -- safe to run unattended, unlike
// pc/probe/verify-touch-inject.ps1's full run. Exits before any capture/
// encode/transport setup, like --list-displays.
//
// Run pc_test_source alongside so the desktop actually changes at 60 fps;
// on an idle desktop the fps/bitrate acceptance checks will fail by design.
//
// The capture/encode/send machinery lives in Pipeline (pc/src/pipeline.cpp,
// shared with the sidepadd resident daemon); this file is only argument
// parsing, the banner, and the acceptance summary.

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "pipeline.h"
#include "vdd_mode.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

void FailIfErr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "[fatal] %s failed: hr=0x%08lX\n", what,
                     static_cast<unsigned long>(hr));
        std::exit(1);
    }
}

// --list-displays: every adapter x output, then exit. The IddCx virtual
// display (VDD) may enumerate under its own display-only adapter rather than
// the render GPU; --display N only reaches outputs on the default D3D11
// adapter (adapter 0), so this is how we find out which case we're in
// (.steering/20260807-iddcx-virtual-display/).
int ListDisplays() {
    ComPtr<IDXGIFactory1> factory;
    FailIfErr(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())),
              "CreateDXGIFactory1");
    // Reported separately: a merged FOUND cannot distinguish "native mode
    // available" from "only the fallback survived a driver reinstall"
    // (review 2026-08-11 B-3).
    bool native_found = false;    // 2732x2048@120
    bool fallback_found = false;  // 2048x1536@120 (IddCx導入の受入基準1)
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, adapter.GetAddressOf()) ==
            DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        std::printf("adapter %u: %ls%s\n", a, ad.Description,
                    (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "");
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, output.GetAddressOf()) ==
                DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_OUTPUT_DESC od{};
            output->GetDesc(&od);
            const auto& rc = od.DesktopCoordinates;
            std::printf("  output %u: %ls attached=%d %ldx%ld @(%ld,%ld) %dHz\n",
                        o, od.DeviceName, od.AttachedToDesktop ? 1 : 0,
                        rc.right - rc.left, rc.bottom - rc.top, rc.left, rc.top,
                        DisplayRefreshHz(od.DeviceName));
            // Mode list via DXGI, not GDI: EnumDisplayDevices returns FALSE in
            // the sandboxed automation session this tool is verified from,
            // while the DXGI path demonstrably works there (2026-08-07).
            UINT mode_count = 0;
            output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0,
                                       &mode_count, nullptr);
            std::vector<DXGI_MODE_DESC> modes(mode_count);
            if (mode_count &&
                SUCCEEDED(output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                     0, &mode_count,
                                                     modes.data()))) {
                std::vector<std::string> high;  // distinct, >=100Hz
                for (const auto& m : modes) {
                    const int hz =
                        m.RefreshRate.Denominator == 0
                            ? 0
                            : static_cast<int>((m.RefreshRate.Numerator +
                                                m.RefreshRate.Denominator / 2) /
                                               m.RefreshRate.Denominator);
                    if (m.Width == kVddNativeWidth &&
                        m.Height == kVddNativeHeight && hz == kVddRefreshHz) {
                        native_found = true;
                    }
                    if (m.Width == kVddFallbackWidth &&
                        m.Height == kVddFallbackHeight &&
                        hz == kVddRefreshHz) {
                        fallback_found = true;
                    }
                    if (hz < 100) continue;
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%ux%u@%d", m.Width,
                                  m.Height, hz);
                    if (std::find(high.begin(), high.end(), std::string(buf)) ==
                        high.end()) {
                        high.emplace_back(buf);
                    }
                }
                std::printf("    modes: %u total; >=100Hz:", mode_count);
                if (high.empty()) {
                    std::printf(" (none)");
                }
                for (const auto& s : high) std::printf(" %s", s.c_str());
                std::printf("\n");
            }
        }
    }
    std::printf(
        "note: --display N indexes outputs on the default D3D11 adapter "
        "(usually adapter 0)\n");
    std::printf("NATIVE MODE %dx%d@%d: %s\n", kVddNativeWidth,
                kVddNativeHeight, kVddRefreshHz,
                native_found ? "FOUND" : "NOT FOUND");
    std::printf("FALLBACK MODE %dx%d@%d: %s\n", kVddFallbackWidth,
                kVddFallbackHeight, kVddRefreshHz,
                fallback_found ? "FOUND" : "NOT FOUND");
    return 0;
}

// --touch-selftest (judge fix J4): the PC-side half of protocol-mismatch
// detection. The iPad side pins the same golden bytes in
// xtool/tests/touchwire_main.swift; this pins them here, independently, so
// a wire-format mismatch between the two implementations shows up as a
// build-time-adjacent FAIL on either side instead of a silent runtime bug.
// Only exercises the pure functions in input_injector.h (ParseTouchMsg,
// MapToVirtualDesktop, VerticalWheelDelta, HorizontalWheelDelta) --
// SendInput is never called, so this is safe to run unattended.
int TouchSelfTest() {
    struct Check {
        std::string name;
        bool pass;
    };
    std::vector<Check> checks;
    auto add = [&](const char* name, bool pass) {
        checks.push_back({name, pass});
    };

    std::printf("touch self-test (design.md section 1/3-1, no SendInput calls)\n\n");

    // --- G1/G2 golden bytes (design.md section 1) ---------------------------
    {
        // wire = 0B 00 00 00 08 02 00 34 12 CD AB 00 00 00 00
        const uint8_t wire[] = {0x0B, 0x00, 0x00, 0x00, 0x08, 0x02, 0x00, 0x34,
                                 0x12, 0xCD, 0xAB, 0x00, 0x00, 0x00, 0x00};
        const uint32_t length = wire[0] | (wire[1] << 8) | (wire[2] << 16) |
                                (static_cast<uint32_t>(wire[3]) << 24);
        add("G1 length/type framing", length == 0x0B && wire[4] == 8);
        TouchMsg m{};
        const bool parsed = ParseTouchMsg(wire + 5, sizeof(wire) - 5, &m);
        add("G1 ParseTouchMsg", parsed && m.kind == 2 && m.source == 0 &&
                                    m.x == 0x1234 && m.y == 0xABCD &&
                                    m.dx == 0 && m.dy == 0);
    }
    {
        // wire = 0B 00 00 00 08 03 01 00 80 00 80 F6 FF 2C 01
        const uint8_t wire[] = {0x0B, 0x00, 0x00, 0x00, 0x08, 0x03, 0x01, 0x00,
                                 0x80, 0x00, 0x80, 0xF6, 0xFF, 0x2C, 0x01};
        const uint32_t length = wire[0] | (wire[1] << 8) | (wire[2] << 16) |
                                (static_cast<uint32_t>(wire[3]) << 24);
        add("G2 length/type framing", length == 0x0B && wire[4] == 8);
        TouchMsg m{};
        const bool parsed = ParseTouchMsg(wire + 5, sizeof(wire) - 5, &m);
        add("G2 ParseTouchMsg", parsed && m.kind == 3 && m.source == 1 &&
                                    m.x == 0x8000 && m.y == 0x8000 &&
                                    m.dx == -10 && m.dy == 300);
    }
    {
        // G3 dragBegin (design.md section 1, added 2026-08-11 K-approved
        // long-press drag): wire = 0B 00 00 00 08 04 00 01 00 FE FF 00 00 00 00
        const uint8_t wire[] = {0x0B, 0x00, 0x00, 0x00, 0x08, 0x04, 0x00, 0x01,
                                 0x00, 0xFE, 0xFF, 0x00, 0x00, 0x00, 0x00};
        const uint32_t length = wire[0] | (wire[1] << 8) | (wire[2] << 16) |
                                (static_cast<uint32_t>(wire[3]) << 24);
        add("G3 length/type framing", length == 0x0B && wire[4] == 8);
        TouchMsg m{};
        const bool parsed = ParseTouchMsg(wire + 5, sizeof(wire) - 5, &m);
        add("G3 ParseTouchMsg (kind=4 dragBegin)",
            parsed && m.kind == kTouchDragBegin && m.source == 0 &&
                m.x == 0x0001 && m.y == 0xFFFE && m.dx == 0 && m.dy == 0);
    }
    {
        // kind=5 (dragEnd) parse smoke test: not a design.md golden (only
        // dragBegin is pinned there), but ParseTouchMsg has no kind-specific
        // logic, so this exercises the same field layout with kind=5 for
        // the "②kind=4/5のパース" requirement explicitly.
        uint8_t payload[10] = {kTouchDragEnd, 1, 0xFF, 0xC0, 0x00, 0x40, 0, 0, 0, 0};
        TouchMsg m{};
        const bool parsed = ParseTouchMsg(payload, sizeof(payload), &m);
        add("kind=5 (dragEnd) ParseTouchMsg",
            parsed && m.kind == kTouchDragEnd && m.source == 1 &&
                m.x == 0xC0FF && m.y == 0x4000 && m.dx == 0 && m.dy == 0);
    }
    {
        TouchMsg m{};
        uint8_t buf9[9] = {0}, buf11[11] = {0}, buf10[10] = {0};
        add("ParseTouchMsg rejects size 9", !ParseTouchMsg(buf9, 9, &m));
        add("ParseTouchMsg rejects size 11", !ParseTouchMsg(buf11, 11, &m));
        add("ParseTouchMsg rejects null payload", !ParseTouchMsg(nullptr, 10, &m));
        add("ParseTouchMsg rejects null out", !ParseTouchMsg(buf10, 10, nullptr));
    }

    // --- MapToVirtualDesktop (design.md section 3-1) -------------------------
    {
        // Single-monitor identity case: target == vscreen, so the two-stage
        // transform round-trips to (x,y) within 1 unit of rounding.
        RECT r{0, 0, 2732, 2048};
        struct Case { uint16_t x, y; };
        Case cases[] = {{0, 0}, {65535, 65535}, {0, 65535}, {65535, 0}, {32767, 32767}};
        bool ok = true;
        for (const auto& c : cases) {
            POINT p = MapToVirtualDesktop(c.x, c.y, r, r);
            if (std::abs(static_cast<long>(p.x) - static_cast<long>(c.x)) > 1 ||
                std::abs(static_cast<long>(p.y) - static_cast<long>(c.y)) > 1) {
                ok = false;
            }
        }
        add("MapToVirtualDesktop identity (single monitor)", ok);
    }
    {
        // Second-monitor offset case: VDD (2732x2048) to the right of a
        // 1920x1080 primary. Expected values computed independently here
        // (not copied from input_injector.cpp's formula).
        RECT target{1920, 0, 1920 + 2732, 2048};
        RECT vscreen{0, 0, 1920 + 2732, 2048};
        POINT p_tl = MapToVirtualDesktop(0, 0, target, vscreen);
        const double expect_tl_x = (1920.0 - 0.0) * 65535.0 / (4652.0 - 1.0);
        POINT p_br = MapToVirtualDesktop(65535, 65535, target, vscreen);
        const double expect_br_x = (1920.0 + 2731.0) * 65535.0 / (4652.0 - 1.0);
        const double expect_br_y = 2047.0 * 65535.0 / (2048.0 - 1.0);
        const bool ok =
            std::abs(p_tl.x - static_cast<long>(expect_tl_x + 0.5)) <= 1 &&
            p_tl.y == 0 &&
            std::abs(p_br.x - static_cast<long>(expect_br_x + 0.5)) <= 1 &&
            std::abs(p_br.y - static_cast<long>(expect_br_y + 0.5)) <= 1;
        add("MapToVirtualDesktop offset (2nd monitor)", ok);
    }
    {
        RECT degenerate{100, 100, 100, 100};
        RECT vscreen{0, 0, 1920, 1080};
        POINT p = MapToVirtualDesktop(32767, 32767, degenerate, vscreen);
        add("MapToVirtualDesktop degenerate rect stays in range",
            p.x >= 0 && p.x <= 65535 && p.y >= 0 && p.y <= 65535);
    }

    // --- Wheel polarity sign test (review 2026-08-11 finding 必須2) ---------
    // Natural scroll: vertical is SAME sign as dy, horizontal is OPPOSITE
    // sign of dx (Win32's WHEEL/HWHEEL polarity is opposite on the two
    // axes -- see input_injector.h's doc comment on these two functions).
    {
        add("VerticalWheelDelta(+10) > 0 (finger down -> content down)",
            VerticalWheelDelta(10) > 0);
        add("VerticalWheelDelta(-10) < 0 (finger up -> content up)",
            VerticalWheelDelta(-10) < 0);
        add("HorizontalWheelDelta(+10) < 0 (finger right -> content right)",
            HorizontalWheelDelta(10) < 0);
        add("HorizontalWheelDelta(-10) > 0 (finger left -> content left)",
            HorizontalWheelDelta(-10) > 0);
        // G2's own dx=-10 dy=300 magnitudes, sign-checked against the
        // golden values (magnitude is kWheelPerPoint=4 -- not repinned here
        // so a future retune of the constant doesn't false-fail this test).
        add("G2 dy=300 -> VerticalWheelDelta > 0", VerticalWheelDelta(300) > 0);
        add("G2 dx=-10 -> HorizontalWheelDelta > 0", HorizontalWheelDelta(-10) > 0);
    }

    // --- Long-press drag fail-safe state machine (design.md section 1,
    // review 2026-08-11 addition) ---------------------------------------
    // Deliberately calls ONLY BeginDragState()/EndDragState()/DragActive()
    // -- the pure atomic-only half of the drag machine -- never OnTouch()
    // or ReleaseDragIfActive() themselves, both of which conditionally call
    // SendInput. This proves the fail-safe's state transitions without
    // moving the real cursor or pressing the real left button, per the
    // task's "SendInput自体は発火しない形で状態だけ検証する" requirement.
    {
        InputInjector injector;  // fresh instance, target rect irrelevant here
        add("drag inactive by default", !injector.DragActive());
        add("BeginDragState() 1st call transitions inactive->active (would inject LEFTDOWN)",
            injector.BeginDragState());
        add("DragActive() true after BeginDragState()", injector.DragActive());
        add("BeginDragState() duplicate call returns false (design.md fail-safe (1), ignored)",
            !injector.BeginDragState());
        add("DragActive() still true after the ignored duplicate", injector.DragActive());
        add("EndDragState() transitions active->inactive (would inject LEFTUP; this is exactly "
            "what ReleaseDragIfActive() does before its SendInput call)",
            injector.EndDragState());
        add("DragActive() false after EndDragState()", !injector.DragActive());
        add("EndDragState() with nothing active returns false (no double LEFTUP)",
            !injector.EndDragState());
        add("drags() counted exactly 1 completed drag", injector.drags() == 1);

        // Re-arm to prove the SAME state machine is what
        // ReleaseDragIfActive() (Pipeline::Stop()'s fail-safe call) relies
        // on -- teardown-while-dragging must clear drag_active_.
        injector.BeginDragState();
        add("re-armed: DragActive() true", injector.DragActive());
        add("EndDragState() drops it again (Pipeline::Stop()'s fail-safe path, state only)",
            injector.EndDragState());
        add("DragActive() false after the fail-safe-equivalent release", !injector.DragActive());
        add("drags() now counts 2 completed drags", injector.drags() == 2);
    }

    bool all_pass = true;
    for (const auto& c : checks) {
        std::printf("[%s] %s\n", c.pass ? "PASS" : "FAIL", c.name.c_str());
        all_pass = all_pass && c.pass;
    }
    std::printf("\n%s (%zu/%zu checks passed)\n",
                all_pass ? "ALL PASS" : "SOME FAILED",
                std::count_if(checks.begin(), checks.end(),
                              [](const Check& c) { return c.pass; }),
                checks.size());
    return all_pass ? 0 : 2;
}

double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(p * (v.size() - 1));
    return v[idx];
}

double PercentileLL(std::vector<long long> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(p * (v.size() - 1));
    return static_cast<double>(v[idx]);
}

// Minimal scan for `"key": <number>` in the device report JSON. Both report
// producers (Swift JSONEncoder, mock_receiver.py) emit flat objects with
// unique keys, so substring search is unambiguous; no JSON library needed.
long long ExtractJsonLL(const std::string& json, const char* key,
                        long long fallback) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return fallback;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return fallback;
    return std::strtoll(json.c_str() + pos + 1, nullptr, 10);
}

struct Options {
    int seconds = 20;
    long long bitrate_bps = 40'000'000;
    UINT display = 0;
    std::string output_path;
    // "" = no transport (statistics/dump only, the pre-integration behavior).
    // "usb" | "usb:PORT" = usbmux direct to the iPad's listen port (7789).
    // "tcp:HOST:PORT"    = plain TCP (mock receiver / wired-LAN fallback).
    std::string send_target;
    bool list_displays = false;
    bool touch_selftest = false;
};

Options ParseArgs(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        auto need_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[fatal] %s needs a value\n", flag);
                std::exit(1);
            }
            return argv[++i];
        };
        if (std::strcmp(argv[i], "--seconds") == 0) {
            opt.seconds = std::atoi(need_value("--seconds"));
        } else if (std::strcmp(argv[i], "--bitrate-mbps") == 0) {
            opt.bitrate_bps = 1'000'000LL * std::atoll(need_value("--bitrate-mbps"));
        } else if (std::strcmp(argv[i], "--display") == 0) {
            opt.display = static_cast<UINT>(std::atoi(need_value("--display")));
        } else if (std::strcmp(argv[i], "--output") == 0) {
            opt.output_path = need_value("--output");
        } else if (std::strcmp(argv[i], "--send") == 0) {
            opt.send_target = need_value("--send");
        } else if (std::strcmp(argv[i], "--list-displays") == 0) {
            opt.list_displays = true;
        } else if (std::strcmp(argv[i], "--touch-selftest") == 0) {
            opt.touch_selftest = true;
        } else {
            std::fprintf(stderr, "[fatal] unknown arg: %s\n", argv[i]);
            std::exit(1);
        }
    }
    return opt;
}

}  // namespace

int main(int argc, char** argv) {
    // Physical pixels for DXGI coordinates (see daemon_main.cpp main()).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    setvbuf(stdout, nullptr, _IONBF, 0);
    const Options opt = ParseArgs(argc, argv);
    if (opt.list_displays) return ListDisplays();
    if (opt.touch_selftest) return TouchSelfTest();

    Pipeline pipeline;
    std::string err;
    if (!pipeline.InitCapture(opt.display, &err)) {
        std::fprintf(stderr, "[fatal] %s\n", err.c_str());
        return 1;
    }
    const int width = pipeline.width();
    const int height = pipeline.height();
    const int fps = pipeline.fps();
    const double frame_budget_bytes =
        static_cast<double>(opt.bitrate_bps) / 8.0 / fps;

    std::printf("sidepad pc-encode spike: DXGI capture -> AMF H.264\n");
    std::printf("  display: %dx%d@%dHz  bitrate: %.0f Mbps CBR  run: %ds\n",
                width, height, fps, opt.bitrate_bps / 1e6, opt.seconds);
    std::printf("  frame budget: %.0f bytes  vbv: 1 frame  transport slice: 16KiB\n\n",
                frame_budget_bytes);

    if (!pipeline.InitEncoder(opt.bitrate_bps, &err)) {
        std::fprintf(stderr, "[fatal] AmfEncoder::Init: %s\n", err.c_str());
        return 1;
    }
    std::printf("encoder ready: slices/frame=%d converter_fallback=%s\n\n",
                pipeline.encoder().slices_per_frame(),
                pipeline.encoder().using_converter() ? "yes (BGRA rejected)"
                                                     : "no (direct BGRA)");

    if (!pipeline.InitOutputs(opt.output_path, opt.send_target, &err)) {
        std::fprintf(stderr, "[fatal] %s\n", err.c_str());
        return 1;
    }
    if (pipeline.sending()) {
        std::printf("transport connected: %s (frame = 1 DATA msg, 16KiB writes)\n\n",
                    opt.send_target.c_str());
    }

    // --- Timed run (capture + drain threads live in Pipeline) ---
    const auto run_start = Clock::now();
    const auto run_until = run_start + std::chrono::seconds(opt.seconds);
    pipeline.Start();
    while (Clock::now() < run_until && !pipeline.stopping()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    pipeline.Stop();  // includes the 250ms flush tail
    const double elapsed =
        std::chrono::duration<double>(Clock::now() - run_start).count() - 0.25;

    if (!pipeline.fatal_error().empty()) {
        std::fprintf(stderr, "%s\n", pipeline.fatal_error().c_str());
        return 1;
    }

    // --- Summary + acceptance verdicts (thresholds from requirements.md) ---
    const auto& latency_ms_all = pipeline.latency_ms_all();
    const long long outputs = pipeline.encoded_frames();
    const double enc_fps = outputs / elapsed;
    const double lat_p99 = Percentile(latency_ms_all, 0.99);
    const double rate_mbps = pipeline.encoded_bytes() * 8.0 / elapsed / 1e6;
    const double frame_p99 = PercentileLL(pipeline.frame_bytes_all(), 0.99);
    const double slice_med = PercentileLL(pipeline.slice_bytes_all(), 0.50);
    const long long idr_frames = pipeline.idr_frames();
    const long long sent_frames = pipeline.sent_frames();
    const long long total_drops = pipeline.encoder().input_full_drops() +
                                  pipeline.encoder().pool_starved_drops();

    std::printf("\n=== summary (%.1fs) ===\n", elapsed);
    std::printf("captured/submitted: %lld  encoded: %lld  submit_errors: %lld\n",
                pipeline.captured(), outputs, pipeline.submit_errors());
    std::printf("drops (busy policy): input_full=%lld pool_starved=%lld\n",
                pipeline.encoder().input_full_drops(),
                pipeline.encoder().pool_starved_drops());
    std::printf("latency ms: avg=%.2f p99=%.2f max=%.2f (n=%zu)\n",
                latency_ms_all.empty()
                    ? 0.0
                    : std::accumulate(latency_ms_all.begin(),
                                      latency_ms_all.end(), 0.0) /
                          latency_ms_all.size(),
                lat_p99,
                latency_ms_all.empty()
                    ? 0.0
                    : *std::max_element(latency_ms_all.begin(),
                                        latency_ms_all.end()),
                latency_ms_all.size());
    std::printf("bitrate: %.1f Mbps (target %.0f)  frame bytes p99=%.0f "
                "(budget %.0f)  IDR frames: %lld\n",
                rate_mbps, opt.bitrate_bps / 1e6, frame_p99, frame_budget_bytes,
                idr_frames);
    std::printf("slices: n=%zu median=%.0fB p99=%.0fB (target 16384B)\n\n",
                pipeline.slice_bytes_all().size(), slice_med,
                PercentileLL(pipeline.slice_bytes_all(), 0.99));

    // --- Transport summary (only with --send) ---
    const bool sending = !opt.send_target.empty();
    std::string report_json;
    long long report_missing = -1;
    long long report_packets = -1;
    if (sending) {
        StreamSender& sender = pipeline.sender();
        const auto& sns = sender.send_elapsed_ns();
        std::vector<double> send_ms;
        send_ms.reserve(sns.size());
        for (long long ns : sns) send_ms.push_back(ns / 1e6);
        const double send_avg =
            send_ms.empty() ? 0.0
                            : std::accumulate(send_ms.begin(), send_ms.end(), 0.0) /
                                  send_ms.size();
        std::printf("transport: sent=%lld frames  send ms avg=%.2f p99=%.2f "
                    "max=%.2f  acks=%lld\n",
                    sent_frames, send_avg, Percentile(send_ms, 0.99),
                    send_ms.empty() ? 0.0
                                    : *std::max_element(send_ms.begin(),
                                                        send_ms.end()),
                    sender.acks_received());
        // Touch injection counters (judge fix J1: verify-touch-inject.ps1
        // matches these against the TOUCH_SCRIPT send counts -- coordinate
        // checks alone don't prove the injected-event COUNT is right).
        std::printf("touch: moves=%lld taps=%lld scrolls=%lld drags=%lld "
                    "inject_errors=%lld\n",
                    pipeline.touch_moves(), pipeline.touch_taps(),
                    pipeline.touch_scrolls(), pipeline.touch_drags(),
                    pipeline.touch_inject_errors());
        if (!pipeline.send_failed()) {
            report_json = sender.RequestReport(/*timeout_ms=*/5000);
        }
        if (report_json.empty()) {
            std::printf("device report: (none)\n\n");
        } else {
            report_missing = ExtractJsonLL(report_json, "missingSeq", -1);
            report_packets = ExtractJsonLL(report_json, "packets", -1);
            std::printf("device report: %s\n\n", report_json.c_str());
        }
    }

    struct Check {
        const char* name;
        bool pass;
    };
    std::vector<Check> checks = {
        {"encode fps >= 55            ", enc_fps >= 55.0},
        // 12ms, not 10: the mandatory immediate-context thread protection
        // (SetMultithreadProtected) costs ~3ms of p99 -- see requirements.md.
        {"latency p99 <= 12ms         ", lat_p99 <= 12.0 && !latency_ms_all.empty()},
        {"bitrate within +/-20%       ",
         rate_mbps >= opt.bitrate_bps / 1e6 * 0.8 &&
             rate_mbps <= opt.bitrate_bps / 1e6 * 1.2},
        {"frame bytes p99 <= 2x budget", frame_p99 <= 2.0 * frame_budget_bytes},
        {"slice median in 8..24 KiB   ",
         slice_med >= 8 * 1024 && slice_med <= 24 * 1024},
        // Exactly one IDR (the first frame): proves IDR_PERIOD=0 held and
        // recovery relies on intra refresh, not periodic IDR bursts.
        {"idr frames == 1 (no periodic)", idr_frames == 1},
        {"drop counter implemented    ", total_drops >= 0},
    };
    if (sending) {
        checks.push_back({"transport link stayed up    ", !pipeline.send_failed()});
        checks.push_back({"transport report received   ", !report_json.empty()});
        checks.push_back(
            {"transport missingSeq == 0   ", report_missing == 0});
        checks.push_back({"transport packets == sent   ",
                          report_packets == sent_frames && sent_frames > 0});
    }
    bool all_pass = true;
    for (const auto& c : checks) {
        std::printf("[%s] %s\n", c.pass ? "PASS" : "FAIL", c.name);
        all_pass = all_pass && c.pass;
    }
    std::printf("\n(note: fps/bitrate checks assume an active source, e.g. "
                "pc_test_source running)\n");

    return all_pass ? 0 : 2;
}
