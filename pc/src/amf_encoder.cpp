// pc/src/amf_encoder.cpp -- see header for the design-input provenance.

#include "amf_encoder.h"

#include <cmath>
#include <cstdio>

#include "core/Version.h"
#include "components/VideoEncoderVCE.h"
#include "components/VideoConverter.h"

using Microsoft::WRL::ComPtr;

namespace {

// Property helper: required properties abort Init with a message; optional
// ones (properties that may not exist on older driver runtimes) just warn.
template <typename T>
bool SetRequired(amf::AMFPropertyStorage* storage, const wchar_t* name,
                 const T& value, std::string* error) {
    AMF_RESULT res = storage->SetProperty(name, value);
    if (res != AMF_OK) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "SetProperty(%ls) failed: %d", name,
                      static_cast<int>(res));
        *error = buf;
        return false;
    }
    return true;
}

template <typename T>
void SetOptional(amf::AMFPropertyStorage* storage, const wchar_t* name,
                 const T& value) {
    AMF_RESULT res = storage->SetProperty(name, value);
    if (res != AMF_OK) {
        std::fprintf(stderr, "[warn] optional encoder property %ls rejected (%d)\n",
                     name, static_cast<int>(res));
    }
}

}  // namespace

bool AmfEncoder::Init(ID3D11Device* device,
                      ID3D11DeviceContext* immediate_context,
                      const AmfEncoderConfig& config, std::string* error) {
    immediate_context_ = immediate_context;

    // --- Minimal runtime loader. amfrt64.dll comes with the Radeon driver. ---
    amf_dll_ = ::LoadLibraryW(AMF_DLL_NAME);
    if (!amf_dll_) {
        *error = "LoadLibrary(amfrt64.dll) failed -- is an AMD GPU driver installed?";
        return false;
    }
    auto init_fn = reinterpret_cast<AMFInit_Fn>(
        ::GetProcAddress(amf_dll_, AMF_INIT_FUNCTION_NAME));
    if (!init_fn) {
        *error = "GetProcAddress(AMFInit) failed";
        return false;
    }
    AMF_RESULT res = init_fn(AMF_FULL_VERSION, &factory_);
    if (res != AMF_OK || !factory_) {
        *error = "AMFInit failed: " + std::to_string(static_cast<int>(res));
        return false;
    }

    res = factory_->CreateContext(&context_);
    if (res != AMF_OK) {
        *error = "AMFFactory::CreateContext failed: " + std::to_string(res);
        return false;
    }
    // Same D3D11 device as capture: the whole point -- frames never leave VRAM.
    res = context_->InitDX11(device);
    if (res != AMF_OK) {
        *error = "AMFContext::InitDX11 failed: " + std::to_string(res);
        return false;
    }

    res = factory_->CreateComponent(context_, AMFVideoEncoderVCE_AVC, &encoder_);
    if (res != AMF_OK) {
        *error = "CreateComponent(AVC encoder) failed: " + std::to_string(res);
        return false;
    }

    if (!ConfigureEncoderProperties(config, error)) {
        return false;
    }

    // Preferred path: hand BGRA straight to the encoder (recent runtimes
    // convert internally). If this runtime refuses, fall back to the
    // documented AMFVideoConverter (BGRA -> NV12 on GPU) in front of it.
    res = encoder_->Init(amf::AMF_SURFACE_BGRA, config.width, config.height);
    if (res != AMF_OK) {
        std::fprintf(stderr,
                     "[warn] encoder Init(BGRA) rejected (%d); inserting "
                     "AMFVideoConverter BGRA->NV12\n",
                     static_cast<int>(res));
        res = factory_->CreateComponent(context_, AMFVideoConverter, &converter_);
        if (res != AMF_OK) {
            *error = "CreateComponent(converter) failed: " + std::to_string(res);
            return false;
        }
        if (!SetRequired(converter_, AMF_VIDEO_CONVERTER_MEMORY_TYPE,
                         static_cast<amf_int64>(amf::AMF_MEMORY_DX11), error) ||
            !SetRequired(converter_, AMF_VIDEO_CONVERTER_OUTPUT_FORMAT,
                         static_cast<amf_int64>(amf::AMF_SURFACE_NV12), error)) {
            return false;
        }
        res = converter_->Init(amf::AMF_SURFACE_BGRA, config.width, config.height);
        if (res != AMF_OK) {
            *error = "converter Init(BGRA) failed: " + std::to_string(res);
            return false;
        }
        res = encoder_->Init(amf::AMF_SURFACE_NV12, config.width, config.height);
        if (res != AMF_OK) {
            *error = "encoder Init(NV12) failed: " + std::to_string(res);
            return false;
        }
    }

    // Blocking QueryOutput removes the poll-loop sleep granularity from the
    // measured latency -- but only on the direct path. On the converter
    // fallback the drain loop must keep pumping converter->encoder, and a
    // 50ms block on an empty encoder would stall that pump (frames would sit
    // in the converter's output queue for up to the full timeout).
    SetOptional(encoder_, AMF_VIDEO_ENCODER_QUERY_TIMEOUT,
                static_cast<amf_int64>(converter_ ? 0 : 50));

    // --- Texture pool the capture frames are copied into. ---
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(config.width);
    desc.Height = static_cast<UINT>(config.height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    for (int i = 0; i < config.surface_pool_size; ++i) {
        ComPtr<ID3D11Texture2D> tex;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf());
        if (FAILED(hr)) {
            *error = "CreateTexture2D(pool) failed";
            return false;
        }
        free_textures_.push_back(tex.Get());
        pool_.push_back(std::move(tex));
    }
    return true;
}

bool AmfEncoder::ConfigureEncoderProperties(const AmfEncoderConfig& config,
                                            std::string* error) {
    // USAGE must be set first: it configures the whole low-latency parameter
    // set (no B-frames etc.); everything after is a targeted override.
    if (!SetRequired(encoder_, AMF_VIDEO_ENCODER_USAGE,
                     static_cast<amf_int64>(
                         AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY),
                     error)) {
        return false;
    }

    const amf_int64 bitrate = config.bitrate_bps;
    // Design input #3 (buffer depth): VBV = exactly one frame's worth of
    // bits. Caps how far a single frame can overshoot the per-frame budget,
    // which is what keeps every frame inside the measured 5-10 ms transfer
    // window. Deep VBV would let the encoder smooth over multiple frames --
    // exactly what the "drop late frames, show newest" policy does not want.
    const amf_int64 vbv_bits = bitrate / config.fps;

    // Design input #2 (slice granularity): make one encoded slice NAL land
    // near the transport's measured sweet spot (16 KiB writes -> p99 2.5 ms).
    const double frame_budget_bytes =
        static_cast<double>(bitrate) / 8.0 / config.fps;
    slices_per_frame_ = static_cast<int>(
        std::ceil(frame_budget_bytes / config.transport_slice_bytes));
    if (slices_per_frame_ < 1) {
        slices_per_frame_ = 1;
    }

    // Intra refresh: with periodic IDR disabled, refresh the frame in
    // stripes so the whole surface re-intras over ~1 second (fps frames).
    const amf_int64 total_mbs =
        ((config.width + 15) / 16) * static_cast<amf_int64>((config.height + 15) / 16);
    const amf_int64 mbs_per_slot = (total_mbs + config.fps - 1) / config.fps;

    struct { const wchar_t* name; amf_int64 value; } required[] = {
        {AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
         AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR},
        {AMF_VIDEO_ENCODER_TARGET_BITRATE, bitrate},
        {AMF_VIDEO_ENCODER_PEAK_BITRATE, bitrate},
        {AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, vbv_bits},
        {AMF_VIDEO_ENCODER_IDR_PERIOD, 0},  // no periodic IDR bursts
        {AMF_VIDEO_ENCODER_SLICES_PER_FRAME, slices_per_frame_},
        {AMF_VIDEO_ENCODER_B_PIC_PATTERN, 0},
        {AMF_VIDEO_ENCODER_QUALITY_PRESET,
         AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED},
        {AMF_VIDEO_ENCODER_PROFILE, AMF_VIDEO_ENCODER_PROFILE_HIGH},
    };
    for (const auto& p : required) {
        if (!SetRequired(encoder_, p.name, p.value, error)) {
            return false;
        }
    }

    if (!SetRequired(encoder_, AMF_VIDEO_ENCODER_FRAMESIZE,
                     AMFConstructSize(config.width, config.height), error) ||
        !SetRequired(encoder_, AMF_VIDEO_ENCODER_FRAMERATE,
                     AMFConstructRate(static_cast<amf_uint32>(config.fps), 1),
                     error)) {
        return false;
    }

    // Intra refresh is REQUIRED, not optional: with IDR_PERIOD=0 it is the
    // only recovery point in the stream. If a runtime rejected it we would
    // ship a stream that can never resynchronize after a single lost byte.
    if (!SetRequired(encoder_, AMF_VIDEO_ENCODER_INTRA_REFRESH_NUM_MBS_PER_SLOT,
                     mbs_per_slot, error)) {
        return false;
    }

    // May not exist on every runtime revision -- warn-and-continue.
    SetOptional(encoder_, AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
    // Latency trims: no pre-analysis lookahead, no motion-boost second pass,
    // single reference frame. ULL usage should imply most of these; set them
    // explicitly so a driver update can't silently re-enable them.
    SetOptional(encoder_, AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE, false);
    SetOptional(encoder_, AMF_VIDEO_ENCODER_HIGH_MOTION_QUALITY_BOOST_ENABLE,
                false);
    SetOptional(encoder_, AMF_VIDEO_ENCODER_MAX_NUM_REFRAMES,
                static_cast<amf_int64>(1));
    return true;
}

SubmitResult AmfEncoder::SubmitFrame(ID3D11Texture2D* src, long long pts_100ns) {
    ID3D11Texture2D* dst = nullptr;
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        if (!free_textures_.empty()) {
            dst = free_textures_.back();
            free_textures_.pop_back();
        }
    }
    if (!dst) {
        // Encoder still owns every pool texture => it is not keeping up.
        // Buffer-depth policy: drop this frame, a newer one will come.
        ++pool_starved_drops_;
        return SubmitResult::kDroppedBusy;
    }

    // GPU->GPU copy, queued before the caller releases the duplication frame.
    immediate_context_->CopyResource(dst, src);

    amf::AMFSurfacePtr surface;
    AMF_RESULT res = context_->CreateSurfaceFromDX11Native(
        dst, &surface, this);
    if (res != AMF_OK) {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        free_textures_.push_back(dst);
        return SubmitResult::kError;
    }
    surface->SetPts(pts_100ns);

    amf::AMFComponent* input = converter_ ? converter_.GetPtr() : encoder_.GetPtr();
    res = input->SubmitInput(surface);
    if (res == AMF_INPUT_FULL) {
        // Dropping here (not blocking) is the design decision: the measured
        // link hiccups are rare and single, so the freshest frame always wins.
        // Our surface ref dies at scope exit -> OnSurfaceDataRelease returns
        // the texture to the pool.
        ++input_full_drops_;
        return SubmitResult::kDroppedBusy;
    }
    if (res != AMF_OK) {
        return SubmitResult::kError;
    }
    return SubmitResult::kSubmitted;
}

bool AmfEncoder::PollOutput(const std::function<void(const EncodedFrame&)>& sink) {
    // Fallback path only: pump converted NV12 surfaces into the encoder.
    if (converter_) {
        while (true) {
            amf::AMFDataPtr converted;
            AMF_RESULT res = converter_->QueryOutput(&converted);
            if (res != AMF_OK || !converted) {
                break;
            }
            res = encoder_->SubmitInput(converted);
            if (res == AMF_INPUT_FULL) {
                ++input_full_drops_;  // converted frame dropped, same policy
            }
        }
    }

    amf::AMFDataPtr data;
    AMF_RESULT res = encoder_->QueryOutput(&data);
    if (res != AMF_OK || !data) {
        return false;  // AMF_REPEAT: nothing ready yet
    }
    amf::AMFBufferPtr buffer(data);
    if (!buffer) {
        return false;
    }

    EncodedFrame frame;
    frame.data = static_cast<const uint8_t*>(buffer->GetNative());
    frame.size = buffer->GetSize();
    frame.pts_100ns = buffer->GetPts();
    amf_int64 data_type = -1;
    if (buffer->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &data_type) ==
        AMF_OK) {
        frame.idr = (data_type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR);
    }
    sink(frame);
    return true;
}

void AMF_STD_CALL AmfEncoder::OnSurfaceDataRelease(amf::AMFSurface* surface) {
    amf::AMFPlane* plane = surface->GetPlaneAt(0);
    if (!plane) {
        return;
    }
    auto* tex = static_cast<ID3D11Texture2D*>(plane->GetNative());
    std::lock_guard<std::mutex> lock(pool_mutex_);
    for (const auto& pooled : pool_) {
        if (pooled.Get() == tex) {
            free_textures_.push_back(tex);
            return;
        }
    }
}

void AmfEncoder::Shutdown() {
    if (converter_) {
        converter_->Terminate();
        converter_ = nullptr;
    }
    if (encoder_) {
        encoder_->Terminate();
        encoder_ = nullptr;
    }
    if (context_) {
        context_->Terminate();
        context_ = nullptr;
    }
    factory_ = nullptr;
    pool_.clear();
    free_textures_.clear();
    if (amf_dll_) {
        ::FreeLibrary(amf_dll_);
        amf_dll_ = nullptr;
    }
}
