// pc/src/pipeline.h
//
// Capture -> AMF encode -> USB send pipeline, extracted from the encode
// spike (encode_main.cpp) so the CLI and the resident daemon (sidepadd,
// .steering/20260808-v1-resident-ops/) drive the same code path. The CLI
// keeps its exact console output and acceptance logic; Pipeline owns the
// capture/drain threads, the transport, and the per-frame statistics.
//
// Init is split into three steps (capture -> encoder -> outputs) purely so
// the CLI can keep printing its banner lines between them in the original
// order; callers that print nothing just call them back to back.
//
// Threading contract:
//   - InitCapture/InitEncoder/InitOutputs/Start/Stop: one controlling thread.
//   - Start() spawns the capture thread and the drain thread. The drain
//     thread prints the per-60-frame window report (same line the spike
//     printed) and sends frames synchronously -- a stalled link stops the
//     drain, fills AMF's input, and the capture side drops: backpressure,
//     no queue.
//   - stopping() may be polled from the controlling thread while running.
//   - Every stats accessor below the "post-Stop" marker is only valid after
//     Stop() returned (single-writer hand-off, as in the spike's post-join
//     reads).

#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "amf_encoder.h"
#include "stream_sender.h"

// Current refresh rate of a GDI display device (e.g. L"\\\\.\\DISPLAY3");
// 60 when the mode query fails. Shared with the CLI's --list-displays.
int DisplayRefreshHz(const wchar_t* device_name);

class Pipeline {
public:
    Pipeline() = default;
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // D3D11 device + desktop duplication for output `display_index` on the
    // default adapter, immediate-context thread protection included. On
    // failure returns false with a description (no "[fatal]" prefix) in
    // *error. Also raises the 1ms timer resolution for the drain poll loop.
    bool InitCapture(UINT display_index, std::string* error);

    // AMF encoder on the capture device (zero-copy), measured defaults from
    // AmfEncoderConfig. Requires InitCapture.
    bool InitEncoder(long long bitrate_bps, std::string* error);

    // Optional Annex-B dump file ("" = none) and transport target
    // ("" = stats only, "usb[:PORT]" = usbmux direct, "tcp:HOST:PORT").
    // Connects and sends MSG_RESET immediately. Requires InitEncoder.
    bool InitOutputs(const std::string& dump_path,
                     const std::string& send_target, std::string* error);

    // Spawns the threads. Resets every run statistic and error flag, so a
    // stopped Pipeline can be Start()ed again for a fresh run (the daemon's
    // reconnect cycle); note Stop() closes the dump file, which is not
    // reopened. No-op before a successful InitEncoder.
    void Start();
    // Stops capture first (the spike ended its capture loop at the deadline),
    // waits 250ms so the encoder can flush already-submitted frames (skipped
    // after a fatal capture error, as the spike joined immediately there),
    // then stops the drain thread and closes the dump file. Idempotent.
    void Stop();

    // True once the pipeline is winding down (send failure, capture loss, or
    // Stop). The controlling thread polls this to end a timed run early.
    bool stopping() const { return stop_.load(std::memory_order_relaxed); }

    // --- Geometry / config (valid after the respective Init step) ---
    int width() const { return width_; }
    int height() const { return height_; }
    int fps() const { return fps_; }
    long long bitrate_bps() const { return config_.bitrate_bps; }
    double frame_budget_bytes() const { return frame_budget_bytes_; }
    const AmfEncoder& encoder() const { return encoder_; }
    StreamSender& sender() { return sender_; }
    bool sending() const { return sender_.connected(); }

    // --- Running statistics (safe while streaming) ---
    // Latency samples accumulated since the previous call (drain thread
    // pushes under a mutex; this swaps the window out). For the daemon's
    // periodic CSV; encoded_frames() below is atomic and equally safe.
    std::vector<double> TakeLatencyWindow();

    // --- Post-Stop statistics (see threading contract) ---
    // Empty string = no fatal error. Formatted exactly like the spike's
    // stderr lines (including the "[fatal] ..." prefix) so the CLI can
    // print it verbatim.
    const std::string& fatal_error() const { return fatal_error_; }
    bool send_failed() const { return send_failed_.load(); }
    long long captured() const { return captured_; }
    long long submit_errors() const { return submit_errors_; }
    long long encoded_frames() const { return encoded_frames_.load(); }
    long long encoded_bytes() const { return encoded_bytes_; }
    long long idr_frames() const { return idr_frames_; }
    long long sent_frames() const { return sent_frames_; }
    const std::vector<double>& latency_ms_all() const { return latency_ms_all_; }
    const std::vector<long long>& frame_bytes_all() const { return frame_bytes_all_; }
    const std::vector<long long>& slice_bytes_all() const { return slice_bytes_all_; }

private:
    void CaptureLoop();
    void DrainLoop();

    // Capture
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    DXGI_OUTPUT_DESC output_desc_{};
    int width_ = 0;
    int height_ = 0;
    int fps_ = 60;
    bool timer_period_raised_ = false;

    // Encode / outputs
    AmfEncoderConfig config_;
    double frame_budget_bytes_ = 0.0;
    AmfEncoder encoder_;
    StreamSender sender_;
    FILE* dump_ = nullptr;

    // Threads
    std::thread capture_thread_;
    std::thread drain_thread_;
    std::atomic<bool> stop_{false};          // stops both threads
    std::atomic<bool> capture_stop_{false};  // stops the capture thread only
    bool started_ = false;

    // pts -> submit-time QPC for submit->output latency. Guarded: the
    // capture thread inserts, the drain thread erases.
    std::mutex inflight_mutex_;
    std::unordered_map<long long, long long> inflight_submit_qpc_;
    long long qpc_freq_ = 1;

    // Latency window for TakeLatencyWindow (mutex-guarded, tiny critical
    // sections; the per-frame cost is one uncontended lock).
    std::mutex window_mutex_;
    std::vector<double> window_lat_;

    // Whole-run series and counters (drain thread during the run, the
    // controlling thread after Stop).
    std::vector<double> latency_ms_all_;
    std::vector<long long> frame_bytes_all_;
    std::vector<long long> slice_bytes_all_;
    std::atomic<long long> encoded_frames_{0};
    long long encoded_bytes_ = 0;
    long long idr_frames_ = 0;
    long long sent_frames_ = 0;
    long long captured_ = 0;        // capture thread, read after Stop
    long long submit_errors_ = 0;   // capture thread, read after Stop
    std::atomic<bool> send_failed_{false};
    // fatal_error_ is written by the capture thread, then fatal_ is set with
    // release order; Stop() reads fatal_ (acquire) to decide whether to skip
    // the flush wait, and fatal_error_ itself is read after the join.
    std::atomic<bool> fatal_{false};
    std::string fatal_error_;
};
