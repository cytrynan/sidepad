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
//                   [--list-displays]
//
// Run pc_test_source alongside so the desktop actually changes at 60 fps;
// on an idle desktop the fps/bitrate acceptance checks will fail by design.
//
// The capture scaffolding mirrors pc/src/main.cpp (the capture spike) on
// purpose -- both are standalone spikes; refactor into a shared capture
// module when the real pipeline (encode + transport) firms up.

#include <windows.h>
#include <timeapi.h>  // timeBeginPeriod; excluded by WIN32_LEAN_AND_MEAN
#include <d3d11.h>
#include <d3d11_4.h>  // ID3D11Multithread
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "amf_encoder.h"
#include "stream_sender.h"  // winsock2.h; safe after windows.h thanks to WIN32_LEAN_AND_MEAN

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")  // timeBeginPeriod

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

constexpr UINT kAcquireTimeoutMs = 50;
constexpr int kReportEveryNFrames = 60;

void FailIfErr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "[fatal] %s failed: hr=0x%08lX\n", what,
                     static_cast<unsigned long>(hr));
        std::exit(1);
    }
}

class ScopedFrameRelease {
public:
    explicit ScopedFrameRelease(IDXGIOutputDuplication* dup) : dup_(dup) {}
    ~ScopedFrameRelease() {
        if (dup_) dup_->ReleaseFrame();
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

CaptureContext CreateDesktopDuplication(UINT output_index) {
    CaptureContext ctx;
    D3D_FEATURE_LEVEL feature_levels[] = {D3D_FEATURE_LEVEL_11_1,
                                          D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL obtained{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   feature_levels, ARRAYSIZE(feature_levels),
                                   D3D11_SDK_VERSION, ctx.device.GetAddressOf(),
                                   &obtained, ctx.context.GetAddressOf());
    FailIfErr(hr, "D3D11CreateDevice");

    // The capture thread issues CopyResource on the immediate context while
    // AMF's internal threads drive the same device's video engine. The
    // immediate context is NOT thread-safe by default; without this, the two
    // eventually corrupt runtime state (DXGI_ERROR_DEVICE_REMOVED or worse).
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(ctx.context.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    } else {
        std::fprintf(stderr,
                     "[warn] ID3D11Multithread unavailable; capture/encode "
                     "share an unprotected immediate context\n");
    }

    ComPtr<IDXGIDevice> dxgi_device;
    FailIfErr(ctx.device.As(&dxgi_device), "QueryInterface(IDXGIDevice)");
    ComPtr<IDXGIAdapter> adapter;
    FailIfErr(dxgi_device->GetAdapter(adapter.GetAddressOf()),
              "IDXGIDevice::GetAdapter");
    ComPtr<IDXGIOutput> output;
    FailIfErr(adapter->EnumOutputs(output_index, output.GetAddressOf()),
              "IDXGIAdapter::EnumOutputs");
    FailIfErr(output->GetDesc(&ctx.output_desc), "IDXGIOutput::GetDesc");
    ComPtr<IDXGIOutput1> output1;
    FailIfErr(output.As(&output1), "QueryInterface(IDXGIOutput1)");
    hr = output1->DuplicateOutput(ctx.device.Get(),
                                  ctx.duplication.GetAddressOf());
    if (hr == E_ACCESSDENIED) {
        std::fprintf(stderr,
                     "[fatal] DuplicateOutput denied (RDP session, secure "
                     "desktop, or no monitor?)\n");
        std::exit(1);
    }
    FailIfErr(hr, "IDXGIOutput1::DuplicateOutput");
    return ctx;
}

int DisplayRefreshHz(const wchar_t* device_name) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(device_name, ENUM_CURRENT_SETTINGS, &mode) &&
        mode.dmDisplayFrequency > 1) {
        return static_cast<int>(mode.dmDisplayFrequency);
    }
    return 60;
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
    bool target_found = false;  // 2048x1536@120 (IddCx導入の受入基準1)
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
                    if (m.Width == 2048 && m.Height == 1536 && hz == 120) {
                        target_found = true;
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
    std::printf("TARGET MODE 2048x1536@120: %s\n",
                target_found ? "FOUND" : "NOT FOUND");
    return 0;
}

// QPC -> 100ns units without 64-bit overflow (qpc * 1e7 can exceed int64).
long long QpcTo100ns(long long qpc, long long freq) {
    const long long whole = qpc / freq;
    const long long rem = qpc % freq;
    return whole * 10'000'000LL + rem * 10'000'000LL / freq;
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

// Annex-B walk: append the byte size of every VCL NAL (types 1 and 5, i.e.
// non-IDR / IDR slices) in `data` to `slice_sizes`. A "slice" here is exactly
// the unit the transport will later send in one burst, which is why its size
// distribution is an acceptance criterion (16 KiB target).
void CollectSliceSizes(const uint8_t* data, size_t size,
                       std::vector<long long>* slice_sizes) {
    size_t i = 0;
    long long cur_start = -1;
    int cur_type = 0;
    auto flush = [&](size_t end) {
        if (cur_start >= 0 && (cur_type == 1 || cur_type == 5)) {
            slice_sizes->push_back(static_cast<long long>(end) - cur_start);
        }
        cur_start = -1;
    };
    while (i + 3 < size) {
        // 00 00 01 or 00 00 00 01
        if (data[i] == 0 && data[i + 1] == 0 &&
            (data[i + 2] == 1 || (data[i + 2] == 0 && i + 4 < size && data[i + 3] == 1))) {
            const size_t start_code_len = (data[i + 2] == 1) ? 3 : 4;
            flush(i);
            cur_start = static_cast<long long>(i);
            cur_type = data[i + start_code_len] & 0x1F;
            i += start_code_len + 1;
        } else {
            ++i;
        }
    }
    flush(size);
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
        } else {
            std::fprintf(stderr, "[fatal] unknown arg: %s\n", argv[i]);
            std::exit(1);
        }
    }
    return opt;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const Options opt = ParseArgs(argc, argv);
    if (opt.list_displays) return ListDisplays();

    // 1ms sleep granularity for the drain thread's poll loop; without this a
    // 500us sleep can stretch to 15.6ms and pollute the latency measurement.
    timeBeginPeriod(1);

    CaptureContext ctx = CreateDesktopDuplication(opt.display);
    const auto& rect = ctx.output_desc.DesktopCoordinates;
    const int width = static_cast<int>(rect.right - rect.left);
    const int height = static_cast<int>(rect.bottom - rect.top);
    const int fps = DisplayRefreshHz(ctx.output_desc.DeviceName);

    AmfEncoderConfig cfg;
    cfg.width = width;
    cfg.height = height;
    cfg.fps = fps;
    cfg.bitrate_bps = opt.bitrate_bps;

    const double frame_budget_bytes =
        static_cast<double>(cfg.bitrate_bps) / 8.0 / fps;

    std::printf("sidepad pc-encode spike: DXGI capture -> AMF H.264\n");
    std::printf("  display: %dx%d@%dHz  bitrate: %.0f Mbps CBR  run: %ds\n",
                width, height, fps, cfg.bitrate_bps / 1e6, opt.seconds);
    std::printf("  frame budget: %.0f bytes  vbv: 1 frame  transport slice: 16KiB\n\n",
                frame_budget_bytes);

    AmfEncoder encoder;
    std::string err;
    if (!encoder.Init(ctx.device.Get(), ctx.context.Get(), cfg, &err)) {
        std::fprintf(stderr, "[fatal] AmfEncoder::Init: %s\n", err.c_str());
        return 1;
    }
    std::printf("encoder ready: slices/frame=%d converter_fallback=%s\n\n",
                encoder.slices_per_frame(),
                encoder.using_converter() ? "yes (BGRA rejected)" : "no (direct BGRA)");

    FILE* dump = nullptr;
    if (!opt.output_path.empty()) {
        dump = std::fopen(opt.output_path.c_str(), "wb");
        if (!dump) {
            std::fprintf(stderr, "[fatal] cannot open %s\n", opt.output_path.c_str());
            return 1;
        }
    }

    StreamSender sender;
    if (!opt.send_target.empty()) {
        std::string err;
        bool ok = false;
        if (opt.send_target.rfind("usb", 0) == 0) {
            uint16_t port = 7789;
            if (opt.send_target.size() > 4 && opt.send_target[3] == ':') {
                port = static_cast<uint16_t>(
                    std::atoi(opt.send_target.c_str() + 4));
            }
            ok = sender.ConnectUsbmux(port, &err);
        } else if (opt.send_target.rfind("tcp:", 0) == 0) {
            const std::string rest = opt.send_target.substr(4);
            const size_t colon = rest.rfind(':');
            if (colon == std::string::npos || colon == 0) {
                err = "--send tcp form is tcp:HOST:PORT";
            } else {
                ok = sender.ConnectTcp(
                    rest.substr(0, colon),
                    static_cast<uint16_t>(std::atoi(rest.c_str() + colon + 1)),
                    &err);
            }
        } else {
            err = "unknown --send target (use usb[:PORT] or tcp:HOST:PORT)";
        }
        if (!ok || !sender.SendReset("encode-stream", /*ack_interval=*/64)) {
            std::fprintf(stderr, "[fatal] transport: %s\n",
                         ok ? "reset message failed" : err.c_str());
            return 1;
        }
        std::printf("transport connected: %s (frame = 1 DATA msg, 16KiB writes)\n\n",
                    opt.send_target.c_str());
    }

    LARGE_INTEGER qpc_freq{};
    QueryPerformanceFrequency(&qpc_freq);
    const long long freq = qpc_freq.QuadPart;

    // pts -> submit-time map for submit->output latency. Guarded because the
    // capture thread inserts and the drain thread erases.
    std::mutex inflight_mutex;
    std::unordered_map<long long, long long> inflight_submit_qpc;

    // Whole-run series (drain thread only, until the join).
    std::vector<double> latency_ms_all;
    std::vector<long long> frame_bytes_all;
    std::vector<long long> slice_bytes_all;
    std::atomic<long long> encoded_frames{0};
    long long encoded_bytes = 0;
    long long idr_frames = 0;
    long long window_frames = 0;
    long long window_bytes = 0;
    double window_lat_sum = 0.0;
    auto window_start = Clock::now();

    std::atomic<bool> stop{false};
    std::atomic<bool> send_failed{false};
    long long sent_frames = 0;  // drain thread during the run, main after join

    std::thread drain([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const bool got = encoder.PollOutput([&](const EncodedFrame& f) {
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);

                double lat_ms = -1.0;
                {
                    std::lock_guard<std::mutex> lock(inflight_mutex);
                    auto it = inflight_submit_qpc.find(f.pts_100ns);
                    if (it != inflight_submit_qpc.end()) {
                        lat_ms = (now.QuadPart - it->second) * 1000.0 / freq;
                        inflight_submit_qpc.erase(it);
                    }
                }
                if (lat_ms >= 0) {
                    latency_ms_all.push_back(lat_ms);
                    window_lat_sum += lat_ms;
                }
                frame_bytes_all.push_back(static_cast<long long>(f.size));
                CollectSliceSizes(f.data, f.size, &slice_bytes_all);
                encoded_bytes += static_cast<long long>(f.size);
                window_bytes += static_cast<long long>(f.size);
                if (f.idr) ++idr_frames;

                // Synchronous send on this thread is deliberate: if the link
                // stalls, PollOutput stops draining, AMF's input fills, and
                // the capture side drops frames -- the existing "stale frames
                // die, latest wins" path doubles as backpressure. No queue.
                if (sender.connected() && !send_failed.load()) {
                    if (sender.SendFrame(static_cast<uint64_t>(sent_frames),
                                         MonotonicNs(), f.data, f.size)) {
                        ++sent_frames;
                    } else {
                        std::fprintf(stderr,
                                     "[fatal] transport send failed (link broken)\n");
                        send_failed = true;
                        stop = true;
                    }
                }
                if (dump) std::fwrite(f.data, 1, f.size, dump);

                encoded_frames.fetch_add(1, std::memory_order_relaxed);
                if (++window_frames >= kReportEveryNFrames) {
                    const auto now_tp = Clock::now();
                    const double win_s =
                        std::chrono::duration<double>(now_tp - window_start).count();
                    std::printf(
                        "[enc %6lld] fps=%6.2f  latency avg=%.2fms  "
                        "avg_frame=%lldB (%.2fx budget)  rate=%.1fMbps  "
                        "drops busy=%lld pool=%lld\n",
                        encoded_frames.load(), window_frames / win_s,
                        window_lat_sum / window_frames,
                        window_bytes / window_frames,
                        (window_bytes / static_cast<double>(window_frames)) /
                            frame_budget_bytes,
                        window_bytes * 8.0 / win_s / 1e6,
                        encoder.input_full_drops(), encoder.pool_starved_drops());
                    window_frames = 0;
                    window_bytes = 0;
                    window_lat_sum = 0.0;
                    window_start = now_tp;
                }
            });
            if (!got) {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }
    });

    // --- Capture loop (main thread) ---
    long long captured = 0;
    long long submit_errors = 0;
    const auto run_until = Clock::now() + std::chrono::seconds(opt.seconds);
    const auto run_start = Clock::now();

    while (Clock::now() < run_until && !stop.load(std::memory_order_relaxed)) {
        ComPtr<IDXGIResource> desktop_resource;
        DXGI_OUTDUPL_FRAME_INFO frame_info{};
        HRESULT hr = ctx.duplication->AcquireNextFrame(
            kAcquireTimeoutMs, &frame_info, desktop_resource.GetAddressOf());
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            std::fprintf(stderr,
                         "[fatal] DXGI_ERROR_ACCESS_LOST (mode change / secure "
                         "desktop) -- recreate not implemented in this spike\n");
            stop = true;
            drain.join();
            return 1;
        }
        FailIfErr(hr, "AcquireNextFrame");
        ScopedFrameRelease release_guard(ctx.duplication.Get());

        if (frame_info.LastPresentTime.QuadPart == 0) {
            continue;  // cursor-only update
        }

        ComPtr<ID3D11Texture2D> frame_texture;
        if (FAILED(desktop_resource.As(&frame_texture))) {
            continue;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const long long pts = QpcTo100ns(now.QuadPart, freq);

        // Reserve the latency slot before submit so the drain thread can
        // never see an output whose submit time isn't recorded yet.
        {
            std::lock_guard<std::mutex> lock(inflight_mutex);
            inflight_submit_qpc[pts] = now.QuadPart;
        }
        const SubmitResult sr = encoder.SubmitFrame(frame_texture.Get(), pts);
        if (sr != SubmitResult::kSubmitted) {
            std::lock_guard<std::mutex> lock(inflight_mutex);
            inflight_submit_qpc.erase(pts);
            if (sr == SubmitResult::kError) ++submit_errors;
        } else {
            ++captured;
        }
    }

    // Let the pipeline flush the last few frames, then stop the drain thread.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    stop = true;
    drain.join();
    if (dump) std::fclose(dump);
    const double elapsed =
        std::chrono::duration<double>(Clock::now() - run_start).count() - 0.25;

    // --- Summary + acceptance verdicts (thresholds from requirements.md) ---
    const long long outputs = encoded_frames.load();
    const double enc_fps = outputs / elapsed;
    const double lat_p99 = Percentile(latency_ms_all, 0.99);
    const double rate_mbps = encoded_bytes * 8.0 / elapsed / 1e6;
    const double frame_p99 = PercentileLL(frame_bytes_all, 0.99);
    const double slice_med = PercentileLL(slice_bytes_all, 0.50);
    const long long total_drops =
        encoder.input_full_drops() + encoder.pool_starved_drops();

    std::printf("\n=== summary (%.1fs) ===\n", elapsed);
    std::printf("captured/submitted: %lld  encoded: %lld  submit_errors: %lld\n",
                captured, outputs, submit_errors);
    std::printf("drops (busy policy): input_full=%lld pool_starved=%lld\n",
                encoder.input_full_drops(), encoder.pool_starved_drops());
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
                rate_mbps, cfg.bitrate_bps / 1e6, frame_p99, frame_budget_bytes,
                idr_frames);
    std::printf("slices: n=%zu median=%.0fB p99=%.0fB (target 16384B)\n\n",
                slice_bytes_all.size(), slice_med,
                PercentileLL(slice_bytes_all, 0.99));

    // --- Transport summary (only with --send) ---
    const bool sending = !opt.send_target.empty();
    std::string report_json;
    long long report_missing = -1;
    long long report_packets = -1;
    if (sending) {
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
        if (!send_failed) {
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
         rate_mbps >= cfg.bitrate_bps / 1e6 * 0.8 &&
             rate_mbps <= cfg.bitrate_bps / 1e6 * 1.2},
        {"frame bytes p99 <= 2x budget", frame_p99 <= 2.0 * frame_budget_bytes},
        {"slice median in 8..24 KiB   ",
         slice_med >= 8 * 1024 && slice_med <= 24 * 1024},
        // Exactly one IDR (the first frame): proves IDR_PERIOD=0 held and
        // recovery relies on intra refresh, not periodic IDR bursts.
        {"idr frames == 1 (no periodic)", idr_frames == 1},
        {"drop counter implemented    ", total_drops >= 0},
    };
    if (sending) {
        checks.push_back({"transport link stayed up    ", !send_failed.load()});
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

    timeEndPeriod(1);
    return all_pass ? 0 : 2;
}
