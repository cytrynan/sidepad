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
#include "cursor_compositor.h"
#include "input_injector.h"
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

    // Stop collecting the whole-run series (latency_ms_all_ etc.) -- they
    // exist for the CLI's post-run summary and grow ~25MB/hour at 120fps.
    // The daemon calls this before Start(); the running window and the
    // atomic counters keep working. Irreversible for this instance.
    void DisableFullSeries() { collect_full_series_ = false; }

    // --- Running statistics (safe while streaming) ---
    // Latency samples accumulated since the previous call (drain thread
    // pushes under a mutex; this swaps the window out). For the daemon's
    // periodic CSV; encoded_frames() below is atomic and equally safe.
    // Cursor-only frames are excluded: they re-encode a static desktop,
    // sit in the low tail, and would dilute the p99 that the +2ms
    // acceptance bound is judged on (review 2026-08-11 B-2).
    std::vector<double> TakeLatencyWindow();

    // Cursor-only frames submitted (capture side) and frames where the
    // pointer sprite was actually drawn. Both cumulative since Start();
    // safe while streaming. Lets a judge separate desktop fps from
    // cursor-induced fps after the fact, and proves a cursor was really
    // composited (review 2026-08-11 B-1/B-2).
    long long cursor_only_frames() const {
        return cursor_only_frames_.load(std::memory_order_relaxed);
    }
    long long cursor_drawn_frames() const { return cursor_.drawn(); }

    // Touch injection counters (InputInjector; see input_injector.h).
    // Cumulative across the process lifetime -- unlike the frame counters
    // above they are not reset per Start(), because touch messages can
    // arrive on the receiver thread even between sessions (e.g. right after
    // a reconnect, before Start() runs) and the daemon's CSV wants a
    // monotonic count either way.
    long long touch_moves() const { return injector_.moves(); }
    long long touch_taps() const { return injector_.taps(); }
    long long touch_scrolls() const { return injector_.scrolls(); }
    long long touch_inject_errors() const { return injector_.inject_errors(); }

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

    // Pointer compositing (capture thread only). Fail-soft: when Init
    // fails the pipeline streams without a visible cursor, as before.
    CursorCompositor cursor_;
    bool cursor_ok_ = false;
    long long last_cursor_submit_qpc_ = 0;  // rate limit for cursor-only frames

    // Encode / outputs
    AmfEncoderConfig config_;
    double frame_budget_bytes_ = 0.0;
    AmfEncoder encoder_;
    StreamSender sender_;
    FILE* dump_ = nullptr;
    InputInjector injector_;

    // Threads
    std::thread capture_thread_;
    std::thread drain_thread_;
    std::atomic<bool> stop_{false};          // stops both threads
    std::atomic<bool> capture_stop_{false};  // stops the capture thread only
    bool started_ = false;

    // pts -> submit-time QPC (+ cursor-only flag) for submit->output
    // latency. Guarded: the capture thread inserts, the drain thread
    // erases.
    struct InflightInfo {
        long long submit_qpc;
        bool cursor_only;
    };
    std::mutex inflight_mutex_;
    std::unordered_map<long long, InflightInfo> inflight_submit_qpc_;
    long long qpc_freq_ = 1;

    // Latency window for TakeLatencyWindow (mutex-guarded, tiny critical
    // sections; the per-frame cost is one uncontended lock).
    std::mutex window_mutex_;
    std::vector<double> window_lat_;
    bool collect_full_series_ = true;  // set false before Start (daemon)

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
    // Capture thread writes, stats/CSV path reads while streaming.
    std::atomic<long long> cursor_only_frames_{0};
    std::atomic<bool> send_failed_{false};
    // fatal_error_ is written by the capture thread, then fatal_ is set with
    // release order; Stop() reads fatal_ (acquire) to decide whether to skip
    // the flush wait, and fatal_error_ itself is read after the join.
    std::atomic<bool> fatal_{false};
    std::string fatal_error_;
};
