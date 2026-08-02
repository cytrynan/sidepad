// pc/src/main.cpp
//
// DXGI Desktop Duplication capture spike.
//
// Goal: prove out the cheapest possible path from "primary monitor changed"
// to "we're holding a GPU texture of the new frame", and measure how fast
// that loop actually goes on this machine (Ryzen 7 8845HS + Radeon 780M).
//
// This is a technical spike only:
//   - No encoding (AMF/AMD Advanced Media Framework would go where marked
//     below, operating directly on the ID3D11Texture2D we already have on
//     the GPU -- no CPU readback needed for that step).
//   - No network transport (would follow encoding, sending the encoded
//     bitstream to the iPad side).
//   - No cursor compositing, no multi-monitor, no HDR handling.
//
// Usage: pc_capture_spike [run_seconds] [output_index]
//
// Build: see pc/README.md / pc/CMakeLists.txt.

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

// Report FPS/latency stats once per this many frames captured.
constexpr int kReportEveryNFrames = 60;

// Timeout passed to AcquireNextFrame. If the desktop is idle (nothing
// changed), DXGI simply doesn't return a frame until something does or the
// timeout elapses, so treat a timeout as "no error, no frame" and loop
// again -- do not treat it as a failure.
//
// Kept short on purpose: a long timeout makes an idle stretch look like one
// giant blocking call and destroys the resolution of the "where did the
// window's wall clock go" breakdown below.
constexpr UINT kAcquireTimeoutMs = 50;

// Inter-frame intervals are bucketed by how many display refresh periods
// they span. Bucket k means "this frame arrived k vsyncs after the previous
// one"; k==1 is a perfect no-drop stream, k>1 means the *source* missed that
// many vsync deadlines. Anything past the last bucket lands in the overflow.
constexpr int kMaxVsyncBucket = 8;

void FailIfErr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "[fatal] %s failed: hr=0x%08lX\n", what,
                     static_cast<unsigned long>(hr));
        std::exit(1);
    }
}

// Thin RAII wrapper so a failed frame doesn't leak the duplication's
// internal frame lock (ReleaseFrame must be called once per successful
// AcquireNextFrame, on every code path, including early returns).
class ScopedFrameRelease {
public:
    explicit ScopedFrameRelease(IDXGIOutputDuplication* dup) : dup_(dup) {}
    ~ScopedFrameRelease() {
        if (dup_) {
            dup_->ReleaseFrame();
        }
    }
    ScopedFrameRelease(const ScopedFrameRelease&) = delete;
    ScopedFrameRelease& operator=(const ScopedFrameRelease&) = delete;

private:
    IDXGIOutputDuplication* dup_;
};

struct CaptureContext {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    DXGI_OUTPUT_DESC output_desc{};
};

// Everything measured over one reporting window. The point of the breakdown
// is to answer "where did this window's wall clock actually go", because a
// low fps number on its own cannot distinguish "the desktop stopped changing"
// from "the desktop changed slowly" from "we failed to keep up".
struct WindowStats {
    long long frames = 0;
    double latency_sum_ms = 0.0;
    double latency_max_ms = 0.0;

    // AcquireNextFrame returned DXGI_ERROR_WAIT_TIMEOUT: the desktop image
    // did not change at all during the timeout. Pure idle.
    long long timeouts = 0;
    double timeout_ms = 0.0;

    // AcquireNextFrame succeeded but LastPresentTime == 0: only cursor
    // shape/position changed, no new desktop image.
    long long cursor_only = 0;
    double cursor_only_ms = 0.0;

    // AccumulatedFrames = how many desktop presents the OS coalesced into
    // the frame it just handed us. 1 means we kept up; >1 means presents
    // happened that we never saw as separate frames.
    long long accumulated_sum = 0;
    UINT accumulated_max = 0;
    long long coalesced_events = 0;  // frames where AccumulatedFrames > 1

    // Source-side present intervals, derived from LastPresentTime (a QPC
    // timestamp stamped by the compositor, not by our loop). This is
    // independent of how promptly we call AcquireNextFrame.
    long long src_intervals = 0;
    double src_interval_sum_ms = 0.0;
    double src_interval_max_ms = 0.0;

    int vsync_bucket[kMaxVsyncBucket + 2] = {};  // [0]=sub-vsync, last=overflow

    void Reset() { *this = WindowStats{}; }
};

// Sets up a D3D11 device on the adapter that owns the requested output, then
// asks DXGI for a Desktop Duplication handle on that output. This must be
// re-created if it ever fails with DXGI_ERROR_ACCESS_LOST (e.g. on
// resolution change, GPU driver reset, or a UAC secure-desktop switch) --
// not handled in this spike, noted in README as a known gap.
CaptureContext CreateDesktopDuplication(UINT output_index) {
    CaptureContext ctx;

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL obtained_level{};

    HRESULT hr = D3D11CreateDevice(
        nullptr,  // default adapter (the one driving the primary output)
        D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0,  // no D3D11_CREATE_DEVICE_DEBUG: keep this runnable without the
            // optional "Graphics Tools" Windows feature installed
        feature_levels, ARRAYSIZE(feature_levels), D3D11_SDK_VERSION,
        ctx.device.GetAddressOf(), &obtained_level, ctx.context.GetAddressOf());
    FailIfErr(hr, "D3D11CreateDevice");

    ComPtr<IDXGIDevice> dxgi_device;
    hr = ctx.device.As(&dxgi_device);
    FailIfErr(hr, "QueryInterface(IDXGIDevice)");

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(adapter.GetAddressOf());
    FailIfErr(hr, "IDXGIDevice::GetAdapter");

    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(output_index, output.GetAddressOf());
    FailIfErr(hr, "IDXGIAdapter::EnumOutputs");

    hr = output->GetDesc(&ctx.output_desc);
    FailIfErr(hr, "IDXGIOutput::GetDesc");

    ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    FailIfErr(hr, "QueryInterface(IDXGIOutput1)");

    hr = output1->DuplicateOutput(ctx.device.Get(), ctx.duplication.GetAddressOf());
    if (hr == E_ACCESSDENIED) {
        std::fprintf(stderr,
                      "[fatal] DuplicateOutput denied access.\n"
                      "        Common causes: running over RDP, no monitor "
                      "attached, or another process (e.g. a secure desktop /\n"
                      "        UAC prompt) currently holds exclusive access. "
                      "Close any elevated prompts and try from a real console\n"
                      "        session (not RDP), then retry.\n");
        std::exit(1);
    }
    FailIfErr(hr, "IDXGIOutput1::DuplicateOutput");

    return ctx;
}

// The monitor's refresh period, used to express inter-frame gaps in units
// of "missed vsyncs" rather than raw milliseconds. Falls back to 60 Hz if
// the mode query fails, which only affects how the histogram is labelled.
double RefreshPeriodMs(const wchar_t* device_name) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(device_name, ENUM_CURRENT_SETTINGS, &mode) &&
        mode.dmDisplayFrequency > 1) {
        return 1000.0 / static_cast<double>(mode.dmDisplayFrequency);
    }
    return 1000.0 / 60.0;
}

void PrintWindowReport(long long frame_total, const WindowStats& s,
                       double window_seconds, double refresh_period_ms) {
    const double fps = s.frames / window_seconds;
    const double avg_latency_ms = s.latency_sum_ms / s.frames;

    // Wall clock accounting. Anything left over is time we spent outside of
    // AcquireNextFrame entirely (our own loop overhead + printing).
    const double window_ms = window_seconds * 1000.0;
    const double accounted_ms = s.latency_sum_ms + s.timeout_ms + s.cursor_only_ms;
    const double other_ms = window_ms - accounted_ms;

    // Desktop presents the OS actually performed during this window, vs the
    // number we got handed as distinct frames.
    const double source_fps = s.accumulated_sum / window_seconds;
    const double src_avg_interval_ms =
        s.src_intervals > 0 ? s.src_interval_sum_ms / s.src_intervals : 0.0;

    std::printf(
        "[frame %6lld] fps=%6.2f  acquire_latency avg=%.3fms max=%.3fms "
        "(window=%d frames / %.3fs)\n",
        frame_total, fps, avg_latency_ms, s.latency_max_ms, kReportEveryNFrames,
        window_seconds);

    std::printf(
        "               wall: acquire=%.0fms idle_timeout=%.0fms(%lldx) "
        "cursor_only=%.0fms(%lldx) other=%.0fms\n",
        s.latency_sum_ms, s.timeout_ms, s.timeouts, s.cursor_only_ms,
        s.cursor_only, other_ms);

    std::printf(
        "               source: presents=%lld (%.2f/s) coalesced=%lld "
        "max_accum=%u | src_interval avg=%.2fms max=%.2fms\n",
        s.accumulated_sum, source_fps, s.coalesced_events, s.accumulated_max,
        src_avg_interval_ms, s.src_interval_max_ms);

    std::printf("               vsync gaps (%.2fms period):", refresh_period_ms);
    for (int i = 0; i <= kMaxVsyncBucket + 1; ++i) {
        if (s.vsync_bucket[i] == 0) {
            continue;
        }
        if (i == 0) {
            std::printf(" <1:%d", s.vsync_bucket[i]);
        } else if (i == kMaxVsyncBucket + 1) {
            std::printf(" %d+:%d", kMaxVsyncBucket + 1, s.vsync_bucket[i]);
        } else {
            std::printf(" %dx:%d", i, s.vsync_bucket[i]);
        }
    }
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    const int run_seconds = (argc > 1) ? std::atoi(argv[1]) : 15;
    const UINT output_index =
        (argc > 2) ? static_cast<UINT>(std::atoi(argv[2])) : 0u;

    // Unbuffered: when stdout is redirected to a file the CRT switches to
    // full buffering, and a run that is killed rather than exiting cleanly
    // loses every line it printed.
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("sidepad pc-capture spike: DXGI Desktop Duplication\n");
    std::printf("Running for %d seconds on output %u, reporting every %d frames...\n\n",
                 run_seconds, output_index, kReportEveryNFrames);

    CaptureContext ctx = CreateDesktopDuplication(output_index);

    std::wstring device_name(ctx.output_desc.DeviceName);
    std::wprintf(L"Capturing output: %ls (%ldx%ld .. %ldx%ld)\n", device_name.c_str(),
                 ctx.output_desc.DesktopCoordinates.left,
                 ctx.output_desc.DesktopCoordinates.top,
                 ctx.output_desc.DesktopCoordinates.right,
                 ctx.output_desc.DesktopCoordinates.bottom);

    const double refresh_period_ms = RefreshPeriodMs(device_name.c_str());
    std::printf("Display refresh period: %.3f ms (%.1f Hz)\n\n", refresh_period_ms,
                1000.0 / refresh_period_ms);

    // LastPresentTime is a QPC timestamp, so we need the QPC frequency to
    // turn deltas into milliseconds.
    LARGE_INTEGER qpc_freq{};
    QueryPerformanceFrequency(&qpc_freq);
    const double qpc_ticks_per_ms = static_cast<double>(qpc_freq.QuadPart) / 1000.0;

    const auto run_until = Clock::now() + std::chrono::seconds(run_seconds);

    // Latency here = wall-clock time spent inside AcquireNextFrame, i.e.
    // "how long did we block waiting for DXGI to hand us the next frame's
    // texture". It does NOT include present-to-photon latency on the
    // monitor side, GPU render time for the frame's own content, or
    // anything downstream of this process (encode/send). It's the number
    // that matters for "how much of our frame budget does capture eat".
    long long frame_count_total = 0;
    WindowStats stats;
    LONGLONG prev_present_qpc = 0;
    auto window_start = Clock::now();

    while (Clock::now() < run_until) {
        ComPtr<IDXGIResource> desktop_resource;
        DXGI_OUTDUPL_FRAME_INFO frame_info{};

        const auto acquire_start = Clock::now();
        HRESULT hr = ctx.duplication->AcquireNextFrame(
            kAcquireTimeoutMs, &frame_info, desktop_resource.GetAddressOf());
        const auto acquire_end = Clock::now();
        const double call_ms =
            std::chrono::duration<double, std::milli>(acquire_end - acquire_start)
                .count();

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // Nothing changed on screen within the timeout window. Not an
            // error -- just means the desktop is idle. Loop again, but book
            // the time so the window breakdown stays honest.
            ++stats.timeouts;
            stats.timeout_ms += call_ms;
            continue;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            // Happens on resolution/mode change, GPU reset, or switching to
            // a secure desktop. A real implementation must recreate the
            // whole CaptureContext here. The spike just reports and exits.
            std::fprintf(stderr,
                          "[fatal] DXGI_ERROR_ACCESS_LOST -- duplication "
                          "interface needs to be recreated (not implemented "
                          "in this spike).\n");
            return 1;
        }
        FailIfErr(hr, "AcquireNextFrame");

        // RAII: guarantees ReleaseFrame runs exactly once per successful
        // AcquireNextFrame, even though we `continue`/return on some paths
        // below (frame_info.LastPresentTime == 0 case, etc).
        ScopedFrameRelease release_guard(ctx.duplication.Get());

        // AcquireNextFrame can succeed with LastPresentTime == 0 when only
        // cursor state changed and no new desktop image is available yet.
        // Treat that as "no frame" for FPS purposes.
        if (frame_info.LastPresentTime.QuadPart == 0) {
            ++stats.cursor_only;
            stats.cursor_only_ms += call_ms;
            continue;
        }

        // --- This is the point where a real pipeline would hand the frame
        // off to the encoder ---
        //
        //   ComPtr<ID3D11Texture2D> frame_texture;
        //   desktop_resource.As(&frame_texture);
        //   // frame_texture lives in GPU memory on ctx.device already.
        //   // AMF (AMD Advanced Media Framework) can wrap this D3D11
        //   // texture directly via AMFContext::CreateSurfaceFromDX11Native,
        //   // avoiding a CPU readback (map/copy) entirely -- that's the
        //   // whole point of doing capture and encode on the same device.
        //   amf_encoder.SubmitFrame(frame_texture);
        //
        // Then the encoded bitstream would go out over the C2/USB/Wi-Fi
        // transport to the iPad app. Neither step is implemented here.

        ++frame_count_total;
        ++stats.frames;
        stats.latency_sum_ms += call_ms;
        if (call_ms > stats.latency_max_ms) {
            stats.latency_max_ms = call_ms;
        }

        stats.accumulated_sum += frame_info.AccumulatedFrames;
        if (frame_info.AccumulatedFrames > stats.accumulated_max) {
            stats.accumulated_max = frame_info.AccumulatedFrames;
        }
        if (frame_info.AccumulatedFrames > 1) {
            ++stats.coalesced_events;
        }

        // Gap between this present and the previous one, as timestamped by
        // the compositor. Bucketing it by refresh period turns "the source
        // dropped N vsyncs" into a directly readable histogram.
        if (prev_present_qpc != 0) {
            const double gap_ms =
                static_cast<double>(frame_info.LastPresentTime.QuadPart -
                                    prev_present_qpc) /
                qpc_ticks_per_ms;
            ++stats.src_intervals;
            stats.src_interval_sum_ms += gap_ms;
            if (gap_ms > stats.src_interval_max_ms) {
                stats.src_interval_max_ms = gap_ms;
            }

            int bucket = static_cast<int>(gap_ms / refresh_period_ms + 0.5);
            if (bucket < 0) {
                bucket = 0;
            }
            if (bucket > kMaxVsyncBucket) {
                bucket = kMaxVsyncBucket + 1;
            }
            ++stats.vsync_bucket[bucket];
        }
        prev_present_qpc = frame_info.LastPresentTime.QuadPart;

        if (stats.frames >= kReportEveryNFrames) {
            const auto window_end = Clock::now();
            const double window_seconds =
                std::chrono::duration<double>(window_end - window_start).count();

            PrintWindowReport(frame_count_total, stats, window_seconds,
                              refresh_period_ms);

            stats.Reset();
            window_start = window_end;
        }

        // release_guard's destructor calls ReleaseFrame here, at end of
        // scope, whichever path got us out of the loop body.
    }

    std::printf("\nDone. Total frames captured: %lld\n", frame_count_total);
    return 0;
}
