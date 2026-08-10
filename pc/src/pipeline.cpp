// pc/src/pipeline.cpp
//
// Implementation extracted 1:1 from encode_main.cpp (the encode spike);
// the window-report line, drop policy, latency bookkeeping, and the
// synchronous-send backpressure are unchanged on purpose -- the acceptance
// scripts from 20260806/20260807 re-run against this code to prove it
// (.steering/20260808-v1-resident-ops/design.md "検証手段").

#include "pipeline.h"

#include <timeapi.h>  // timeBeginPeriod; excluded by WIN32_LEAN_AND_MEAN
#include <d3d11_4.h>  // ID3D11Multithread

#include <chrono>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")  // timeBeginPeriod

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

constexpr UINT kAcquireTimeoutMs = 50;
constexpr int kReportEveryNFrames = 60;

// Formats "<what> failed: hr=0x%08lX" -- the body of the spike's FailIfErr
// message; callers prepend "[fatal] " when printing.
std::string HrError(HRESULT hr, const char* what) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s failed: hr=0x%08lX", what,
                  static_cast<unsigned long>(hr));
    return buf;
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

// QPC -> 100ns units without 64-bit overflow (qpc * 1e7 can exceed int64).
long long QpcTo100ns(long long qpc, long long freq) {
    const long long whole = qpc / freq;
    const long long rem = qpc % freq;
    return whole * 10'000'000LL + rem * 10'000'000LL / freq;
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

}  // namespace

int DisplayRefreshHz(const wchar_t* device_name) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(device_name, ENUM_CURRENT_SETTINGS, &mode) &&
        mode.dmDisplayFrequency > 1) {
        return static_cast<int>(mode.dmDisplayFrequency);
    }
    return 60;
}

Pipeline::~Pipeline() {
    Stop();
    if (timer_period_raised_) {
        timeEndPeriod(1);
    }
}

bool Pipeline::InitCapture(UINT display_index, std::string* error) {
    // 1ms sleep granularity for the drain thread's poll loop; without this a
    // 500us sleep can stretch to 15.6ms and pollute the latency measurement.
    if (!timer_period_raised_) {
        timeBeginPeriod(1);
        timer_period_raised_ = true;
    }

    // ReleaseAndGetAddressOf throughout: a re-Init (the daemon's recovery
    // path after ACCESS_LOST or a VDD toggle) must not leak the previous
    // device/duplication.
    D3D_FEATURE_LEVEL feature_levels[] = {D3D_FEATURE_LEVEL_11_1,
                                          D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL obtained{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   feature_levels, ARRAYSIZE(feature_levels),
                                   D3D11_SDK_VERSION,
                                   device_.ReleaseAndGetAddressOf(), &obtained,
                                   context_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        *error = HrError(hr, "D3D11CreateDevice");
        return false;
    }

    // The capture thread issues CopyResource on the immediate context while
    // AMF's internal threads drive the same device's video engine. The
    // immediate context is NOT thread-safe by default; without this, the two
    // eventually corrupt runtime state (DXGI_ERROR_DEVICE_REMOVED or worse).
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(context_.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    } else {
        std::fprintf(stderr,
                     "[warn] ID3D11Multithread unavailable; capture/encode "
                     "share an unprotected immediate context\n");
    }

    ComPtr<IDXGIDevice> dxgi_device;
    hr = device_.As(&dxgi_device);
    if (FAILED(hr)) {
        *error = HrError(hr, "QueryInterface(IDXGIDevice)");
        return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) {
        *error = HrError(hr, "IDXGIDevice::GetAdapter");
        return false;
    }
    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(display_index, output.GetAddressOf());
    if (FAILED(hr)) {
        *error = HrError(hr, "IDXGIAdapter::EnumOutputs");
        return false;
    }
    hr = output->GetDesc(&output_desc_);
    if (FAILED(hr)) {
        *error = HrError(hr, "IDXGIOutput::GetDesc");
        return false;
    }
    ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr)) {
        *error = HrError(hr, "QueryInterface(IDXGIOutput1)");
        return false;
    }
    hr = output1->DuplicateOutput(device_.Get(),
                                  duplication_.ReleaseAndGetAddressOf());
    if (hr == E_ACCESSDENIED) {
        *error = "DuplicateOutput denied (RDP session, secure desktop, or no "
                 "monitor?)";
        return false;
    }
    if (FAILED(hr)) {
        *error = HrError(hr, "IDXGIOutput1::DuplicateOutput");
        return false;
    }

    const auto& rect = output_desc_.DesktopCoordinates;
    width_ = static_cast<int>(rect.right - rect.left);
    height_ = static_cast<int>(rect.bottom - rect.top);
    fps_ = DisplayRefreshHz(output_desc_.DeviceName);

    // Fail-soft: without the compositor the stream still works, the pointer
    // is just invisible on the extended display (pre-20260810 behavior).
    std::string cursor_error;
    cursor_ok_ =
        cursor_.Init(device_.Get(), context_.Get(), width_, height_,
                     &cursor_error);
    if (!cursor_ok_) {
        std::fprintf(stderr, "[warn] cursor compositing disabled: %s\n",
                     cursor_error.c_str());
    }

    LARGE_INTEGER qpc_freq{};
    QueryPerformanceFrequency(&qpc_freq);
    qpc_freq_ = qpc_freq.QuadPart;
    return true;
}

bool Pipeline::InitEncoder(long long bitrate_bps, std::string* error) {
    config_.width = width_;
    config_.height = height_;
    config_.fps = fps_;
    config_.bitrate_bps = bitrate_bps;
    frame_budget_bytes_ = static_cast<double>(bitrate_bps) / 8.0 / fps_;
    return encoder_.Init(device_.Get(), context_.Get(), config_, error);
}

bool Pipeline::InitOutputs(const std::string& dump_path,
                           const std::string& send_target,
                           std::string* error) {
    if (!dump_path.empty()) {
        dump_ = std::fopen(dump_path.c_str(), "wb");
        if (!dump_) {
            *error = "cannot open " + dump_path;
            return false;
        }
    }
    if (!send_target.empty()) {
        bool ok = false;
        std::string err;
        if (send_target.rfind("usb", 0) == 0) {
            uint16_t port = 7789;
            if (send_target.size() > 4 && send_target[3] == ':') {
                port = static_cast<uint16_t>(std::atoi(send_target.c_str() + 4));
            }
            ok = sender_.ConnectUsbmux(port, &err);
        } else if (send_target.rfind("tcp:", 0) == 0) {
            const std::string rest = send_target.substr(4);
            const size_t colon = rest.rfind(':');
            if (colon == std::string::npos || colon == 0) {
                err = "--send tcp form is tcp:HOST:PORT";
            } else {
                ok = sender_.ConnectTcp(
                    rest.substr(0, colon),
                    static_cast<uint16_t>(std::atoi(rest.c_str() + colon + 1)),
                    &err);
            }
        } else {
            err = "unknown --send target (use usb[:PORT] or tcp:HOST:PORT)";
        }
        if (!ok || !sender_.SendReset("encode-stream", /*ack_interval=*/64)) {
            *error = std::string("transport: ") +
                     (ok ? "reset message failed" : err.c_str());
            return false;
        }
    }
    return true;
}

void Pipeline::Start() {
    if (started_ || !duplication_) return;
    started_ = true;

    // Fresh run: a re-Start (the daemon's reconnect cycle) must not inherit
    // the previous run's series, error flags, or -- worst -- stale inflight
    // pts entries that would show up as multi-second latency outliers.
    stop_.store(false, std::memory_order_relaxed);
    capture_stop_.store(false, std::memory_order_relaxed);
    send_failed_.store(false, std::memory_order_relaxed);
    fatal_.store(false, std::memory_order_relaxed);
    fatal_error_.clear();
    {
        std::lock_guard<std::mutex> lock(inflight_mutex_);
        inflight_submit_qpc_.clear();
    }
    latency_ms_all_.clear();
    frame_bytes_all_.clear();
    slice_bytes_all_.clear();
    {
        std::lock_guard<std::mutex> lock(window_mutex_);
        window_lat_.clear();
    }
    encoded_frames_.store(0, std::memory_order_relaxed);
    encoded_bytes_ = 0;
    idr_frames_ = 0;
    sent_frames_ = 0;
    captured_ = 0;
    submit_errors_ = 0;

    drain_thread_ = std::thread([this] { DrainLoop(); });
    capture_thread_ = std::thread([this] { CaptureLoop(); });
}

void Pipeline::Stop() {
    if (!started_) return;
    // Same shutdown timeline as the spike: the capture loop ends first (the
    // spike's ran out of its deadline on the main thread), then a 250ms tail
    // lets the encoder flush frames that were already submitted, then the
    // drain thread stops. After a fatal capture error the spike joined
    // immediately, so the flush is skipped.
    capture_stop_.store(true, std::memory_order_relaxed);
    if (capture_thread_.joinable()) capture_thread_.join();
    if (!fatal_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    stop_.store(true, std::memory_order_relaxed);
    if (drain_thread_.joinable()) drain_thread_.join();
    if (dump_) {
        std::fclose(dump_);
        dump_ = nullptr;
    }
    started_ = false;
}

std::vector<double> Pipeline::TakeLatencyWindow() {
    std::vector<double> out;
    std::lock_guard<std::mutex> lock(window_mutex_);
    out.swap(window_lat_);
    return out;
}

void Pipeline::CaptureLoop() {
    while (!stop_.load(std::memory_order_relaxed) &&
           !capture_stop_.load(std::memory_order_relaxed)) {
        ComPtr<IDXGIResource> desktop_resource;
        DXGI_OUTDUPL_FRAME_INFO frame_info{};
        HRESULT hr = duplication_->AcquireNextFrame(
            kAcquireTimeoutMs, &frame_info, desktop_resource.GetAddressOf());
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            fatal_error_ =
                "[fatal] DXGI_ERROR_ACCESS_LOST (mode change / secure "
                "desktop) -- recreate not implemented in this spike";
            fatal_.store(true, std::memory_order_release);
            stop_.store(true, std::memory_order_relaxed);
            return;
        }
        if (FAILED(hr)) {
            fatal_error_ = "[fatal] " + HrError(hr, "AcquireNextFrame");
            fatal_.store(true, std::memory_order_release);
            stop_.store(true, std::memory_order_relaxed);
            return;
        }
        ScopedFrameRelease release_guard(duplication_.Get());

        cursor_.OnFrameInfo(duplication_.Get(), frame_info);
        const bool desktop_updated = frame_info.LastPresentTime.QuadPart != 0;
        const bool mouse_updated = CursorCompositor::MouseUpdated(frame_info);

        // Without the compositor this reduces to the old behavior:
        // cursor-only updates are skipped.
        if (!desktop_updated && !(cursor_ok_ && mouse_updated)) {
            continue;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);

        // Rate-limit cursor-only frames to ~120Hz: a fast mouse must not
        // saturate the encoder or the link on an otherwise static screen.
        if (!desktop_updated) {
            const double since_ms = (now.QuadPart - last_cursor_submit_qpc_) *
                                    1000.0 / static_cast<double>(qpc_freq_);
            if (since_ms < 8.0) continue;
        }

        ComPtr<ID3D11Texture2D> frame_texture;
        if (desktop_updated && FAILED(desktop_resource.As(&frame_texture))) {
            continue;
        }

        ID3D11Texture2D* submit_texture = frame_texture.Get();
        if (cursor_ok_) {
            // nullptr = cursor-only: re-composite over the stored desktop.
            submit_texture = cursor_.Compose(frame_texture.Get());
            if (!submit_texture) continue;  // no desktop copy yet
        } else if (!submit_texture) {
            continue;
        }
        if (!desktop_updated) last_cursor_submit_qpc_ = now.QuadPart;

        const long long pts = QpcTo100ns(now.QuadPart, qpc_freq_);

        // Reserve the latency slot before submit so the drain thread can
        // never see an output whose submit time isn't recorded yet.
        {
            std::lock_guard<std::mutex> lock(inflight_mutex_);
            inflight_submit_qpc_[pts] = now.QuadPart;
        }
        const SubmitResult sr = encoder_.SubmitFrame(submit_texture, pts);
        if (sr != SubmitResult::kSubmitted) {
            std::lock_guard<std::mutex> lock(inflight_mutex_);
            inflight_submit_qpc_.erase(pts);
            if (sr == SubmitResult::kError) ++submit_errors_;
        } else {
            ++captured_;
        }
    }
}

void Pipeline::DrainLoop() {
    long long window_frames = 0;
    long long window_bytes = 0;
    double window_lat_sum = 0.0;
    auto window_start = Clock::now();

    while (!stop_.load(std::memory_order_relaxed)) {
        const bool got = encoder_.PollOutput([&](const EncodedFrame& f) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);

            double lat_ms = -1.0;
            {
                std::lock_guard<std::mutex> lock(inflight_mutex_);
                auto it = inflight_submit_qpc_.find(f.pts_100ns);
                if (it != inflight_submit_qpc_.end()) {
                    lat_ms = (now.QuadPart - it->second) * 1000.0 / qpc_freq_;
                    inflight_submit_qpc_.erase(it);
                }
            }
            if (lat_ms >= 0) {
                if (collect_full_series_) latency_ms_all_.push_back(lat_ms);
                window_lat_sum += lat_ms;
                {
                    std::lock_guard<std::mutex> lock(window_mutex_);
                    window_lat_.push_back(lat_ms);
                }
            }
            if (collect_full_series_) {
                frame_bytes_all_.push_back(static_cast<long long>(f.size));
                CollectSliceSizes(f.data, f.size, &slice_bytes_all_);
            }
            encoded_bytes_ += static_cast<long long>(f.size);
            window_bytes += static_cast<long long>(f.size);
            if (f.idr) ++idr_frames_;

            // Synchronous send on this thread is deliberate: if the link
            // stalls, PollOutput stops draining, AMF's input fills, and
            // the capture side drops frames -- the existing "stale frames
            // die, latest wins" path doubles as backpressure. No queue.
            if (sender_.connected() && !send_failed_.load()) {
                if (sender_.SendFrame(static_cast<uint64_t>(sent_frames_),
                                      MonotonicNs(), f.data, f.size)) {
                    ++sent_frames_;
                } else {
                    std::fprintf(stderr,
                                 "[fatal] transport send failed (link broken)\n");
                    send_failed_ = true;
                    stop_.store(true, std::memory_order_relaxed);
                }
            }
            if (dump_) std::fwrite(f.data, 1, f.size, dump_);

            encoded_frames_.fetch_add(1, std::memory_order_relaxed);
            if (++window_frames >= kReportEveryNFrames) {
                const auto now_tp = Clock::now();
                const double win_s =
                    std::chrono::duration<double>(now_tp - window_start).count();
                std::printf(
                    "[enc %6lld] fps=%6.2f  latency avg=%.2fms  "
                    "avg_frame=%lldB (%.2fx budget)  rate=%.1fMbps  "
                    "drops busy=%lld pool=%lld\n",
                    encoded_frames_.load(), window_frames / win_s,
                    window_lat_sum / window_frames,
                    window_bytes / window_frames,
                    (window_bytes / static_cast<double>(window_frames)) /
                        frame_budget_bytes_,
                    window_bytes * 8.0 / win_s / 1e6,
                    encoder_.input_full_drops(), encoder_.pool_starved_drops());
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
}
