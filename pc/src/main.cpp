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
// Build: see pc/README.md / pc/CMakeLists.txt.

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

// How long the whole spike runs before exiting on its own.
constexpr auto kRunDuration = std::chrono::seconds(15);

// Report FPS/latency stats once per this many frames captured.
constexpr int kReportEveryNFrames = 60;

// Timeout passed to AcquireNextFrame. If the desktop is idle (nothing
// changed), DXGI simply doesn't return a frame until something does or the
// timeout elapses, so treat a timeout as "no error, no frame" and loop
// again -- do not treat it as a failure.
constexpr UINT kAcquireTimeoutMs = 500;

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

// Sets up a D3D11 device on the adapter that owns the primary output, then
// asks DXGI for a Desktop Duplication handle on that output. This must be
// re-created if it ever fails with DXGI_ERROR_ACCESS_LOST (e.g. on
// resolution change, GPU driver reset, or a UAC secure-desktop switch) --
// not handled in this spike, noted in README as a known gap.
CaptureContext CreateDesktopDuplication() {
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

    // Output 0 on the default adapter is the primary monitor in the common
    // case. A real product needs to enumerate outputs and let the user (or
    // Windows' "primary monitor" flag) pick; out of scope for the spike.
    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(0, output.GetAddressOf());
    FailIfErr(hr, "IDXGIAdapter::EnumOutputs(0)");

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

}  // namespace

int main() {
    std::printf("sidepad pc-capture spike: DXGI Desktop Duplication\n");
    std::printf("Running for %lld seconds, reporting every %d frames...\n\n",
                 static_cast<long long>(kRunDuration.count()), kReportEveryNFrames);

    CaptureContext ctx = CreateDesktopDuplication();

    std::wstring device_name(ctx.output_desc.DeviceName);
    std::wprintf(L"Capturing output: %ls (%ldx%ld .. %ldx%ld)\n", device_name.c_str(),
                 ctx.output_desc.DesktopCoordinates.left,
                 ctx.output_desc.DesktopCoordinates.top,
                 ctx.output_desc.DesktopCoordinates.right,
                 ctx.output_desc.DesktopCoordinates.bottom);

    const auto run_until = Clock::now() + kRunDuration;

    // Latency here = wall-clock time spent inside AcquireNextFrame, i.e.
    // "how long did we block waiting for DXGI to hand us the next frame's
    // texture". It does NOT include present-to-photon latency on the
    // monitor side, GPU render time for the frame's own content, or
    // anything downstream of this process (encode/send). It's the number
    // that matters for "how much of our frame budget does capture eat".
    long long frame_count_total = 0;
    long long frame_count_window = 0;
    double latency_sum_ms_window = 0.0;
    double latency_max_ms_window = 0.0;
    auto window_start = Clock::now();

    while (Clock::now() < run_until) {
        ComPtr<IDXGIResource> desktop_resource;
        DXGI_OUTDUPL_FRAME_INFO frame_info{};

        const auto acquire_start = Clock::now();
        HRESULT hr = ctx.duplication->AcquireNextFrame(
            kAcquireTimeoutMs, &frame_info, desktop_resource.GetAddressOf());
        const auto acquire_end = Clock::now();

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // Nothing changed on screen within the timeout window. Not an
            // error -- just means the desktop is idle. Loop again.
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

        const double latency_ms =
            std::chrono::duration<double, std::milli>(acquire_end - acquire_start)
                .count();

        ++frame_count_total;
        ++frame_count_window;
        latency_sum_ms_window += latency_ms;
        if (latency_ms > latency_max_ms_window) {
            latency_max_ms_window = latency_ms;
        }

        if (frame_count_window >= kReportEveryNFrames) {
            const auto window_end = Clock::now();
            const double window_seconds =
                std::chrono::duration<double>(window_end - window_start).count();
            const double fps = frame_count_window / window_seconds;
            const double avg_latency_ms = latency_sum_ms_window / frame_count_window;

            std::printf(
                "[frame %6lld] fps=%6.2f  acquire_latency avg=%.3fms max=%.3fms "
                "(window=%d frames / %.3fs)\n",
                frame_count_total, fps, avg_latency_ms, latency_max_ms_window,
                kReportEveryNFrames, window_seconds);

            frame_count_window = 0;
            latency_sum_ms_window = 0.0;
            latency_max_ms_window = 0.0;
            window_start = window_end;
        }

        // release_guard's destructor calls ReleaseFrame here, at end of
        // scope, whichever path got us out of the loop body.
    }

    std::printf("\nDone. Total frames captured: %lld\n", frame_count_total);
    return 0;
}
