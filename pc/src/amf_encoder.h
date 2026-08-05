// pc/src/amf_encoder.h
//
// AMF (AMD Advanced Media Framework) H.264 encoder wrapper for the sidepad
// PC-side pipeline. All the numeric defaults come straight from the
// 2026-08-06 USB datalink measurements
// (.steering/20260803-usb-datalink-probe/result.md):
//
//   - transport slice = 16 KiB  -> SLICES_PER_FRAME = ceil(frame_budget/16Ki)
//   - 40 Mbps CBR default        (measured one-way p99 2.5 ms at this rate)
//   - VBV buffer = 1 frame       (hiccups are rare & single -> drop, don't queue)
//   - no periodic IDR            (an IDR burst would blow the per-frame budget;
//                                 intra-refresh spreads I-macroblocks instead)
//
// The AMF runtime (amfrt64.dll) ships with the Radeon driver and is loaded
// dynamically; nothing is linked at build time. Headers are vendored under
// pc/third_party/AMF (v1.5.2, MIT).

#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "core/Factory.h"
#include "core/Surface.h"
#include "components/Component.h"

struct AmfEncoderConfig {
    int width = 0;
    int height = 0;
    int fps = 60;
    long long bitrate_bps = 40'000'000;   // measured-safe default (probe: p99 2.5ms)
    int transport_slice_bytes = 16 * 1024;  // probe's best write size
    int surface_pool_size = 4;
};

// One encoded access unit as handed to the output sink. `data` is only valid
// for the duration of the sink callback.
struct EncodedFrame {
    const uint8_t* data = nullptr;
    size_t size = 0;
    long long pts_100ns = 0;
    bool idr = false;
};

enum class SubmitResult {
    kSubmitted,
    kDroppedBusy,  // pool starved or encoder input full: frame intentionally
                   // dropped (the "no deep buffer, latest wins" policy)
    kError,
};

class AmfEncoder final : public amf::AMFSurfaceObserver {
public:
    AmfEncoder() = default;
    ~AmfEncoder() { Shutdown(); }
    AmfEncoder(const AmfEncoder&) = delete;
    AmfEncoder& operator=(const AmfEncoder&) = delete;

    // `device`/`immediate_context` must be the ones the capture textures live
    // on (zero-copy: AMF wraps them, no CPU readback). On failure returns
    // false and describes why in *error.
    bool Init(ID3D11Device* device, ID3D11DeviceContext* immediate_context,
              const AmfEncoderConfig& config, std::string* error);

    // Called from the capture thread while the duplication frame is still
    // locked: copies `src` into a pool texture (GPU->GPU) and submits it.
    SubmitResult SubmitFrame(ID3D11Texture2D* src, long long pts_100ns);

    // Called from the drain thread. Invokes `sink` for at most one encoded
    // frame. Returns true if a frame was delivered, false if none was ready.
    bool PollOutput(const std::function<void(const EncodedFrame&)>& sink);

    void Shutdown();

    int slices_per_frame() const { return slices_per_frame_; }
    // True when the runtime rejected direct BGRA input and an AMFVideoConverter
    // (BGRA -> NV12) had to be inserted before the encoder.
    bool using_converter() const { return converter_ != nullptr; }
    long long pool_starved_drops() const { return pool_starved_drops_.load(); }
    long long input_full_drops() const { return input_full_drops_.load(); }

    // amf::AMFSurfaceObserver -- fired (on an AMF internal thread) when the
    // encoder is done reading a wrapped texture; returns it to the free pool.
    void AMF_STD_CALL OnSurfaceDataRelease(amf::AMFSurface* surface) override;

private:
    bool ConfigureEncoderProperties(const AmfEncoderConfig& config,
                                    std::string* error);

    HMODULE amf_dll_ = nullptr;
    amf::AMFFactory* factory_ = nullptr;  // owned by the runtime, not released
    amf::AMFContextPtr context_;
    amf::AMFComponentPtr encoder_;
    amf::AMFComponentPtr converter_;  // only on the BGRA-rejected fallback path

    ID3D11DeviceContext* immediate_context_ = nullptr;  // not owned

    // Fixed pool of BGRA textures the capture frames are copied into. A
    // texture leaves `free_textures_` on submit and comes back via
    // OnSurfaceDataRelease. Pool exhaustion == encoder can't keep up == drop.
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> pool_;
    std::vector<ID3D11Texture2D*> free_textures_;
    std::mutex pool_mutex_;

    int slices_per_frame_ = 1;
    // Atomic: bumped from the capture thread AND (converter path) the drain
    // thread, read from the drain thread's window reports.
    std::atomic<long long> pool_starved_drops_{0};
    std::atomic<long long> input_full_drops_{0};
};
