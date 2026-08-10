// pc/src/cursor_compositor.cpp

#include "cursor_compositor.h"

#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;

namespace {

// Quad from SV_VertexID (triangle strip); dest rect arrives in NDC via cb.
const char kShaderSource[] = R"(
cbuffer Rect : register(b0) {
    float4 rect;  // x0, y0, x1, y1 in NDC
};
Texture2D sprite : register(t0);
SamplerState samp : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID) {
    float2 uv = float2(id & 1, id >> 1);
    VSOut o;
    o.pos = float4(lerp(rect.x, rect.z, uv.x), lerp(rect.y, rect.w, uv.y),
                   0.0, 1.0);
    o.uv = uv;
    return o;
}

float4 ps_main(VSOut i) : SV_Target {
    return sprite.Sample(samp, i.uv);
}
)";

bool CompileShader(const char* entry, const char* target, ID3DBlob** blob,
                   std::string* error) {
    ComPtr<ID3DBlob> errors;
    const HRESULT hr =
        D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, nullptr, nullptr,
                   nullptr, entry, target, 0, 0, blob, errors.GetAddressOf());
    if (FAILED(hr)) {
        *error = std::string("D3DCompile(") + entry + ") failed";
        if (errors) {
            error->append(": ");
            error->append(static_cast<const char*>(errors->GetBufferPointer()),
                          errors->GetBufferSize());
        }
        return false;
    }
    return true;
}

}  // namespace

bool CursorCompositor::Init(ID3D11Device* device, ID3D11DeviceContext* context,
                            int width, int height, std::string* error) {
    // Re-Init happens on the daemon's recovery path with a NEW device; every
    // resource (including the sprite) must be recreated on it, and the
    // "have a desktop copy" state starts over.
    initialized_ = false;
    have_desktop_ = false;
    sprite_.Reset();
    sprite_srv_.Reset();
    sprite_w_ = 0;
    sprite_h_ = 0;

    device_ = device;
    context_ = context;
    width_ = width;
    height_ = height;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = 0;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr,
                                        desktop_.GetAddressOf()))) {
        *error = "cursor: desktop copy texture creation failed";
        return false;
    }
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, work_.GetAddressOf()))) {
        *error = "cursor: work texture creation failed";
        return false;
    }
    if (FAILED(device_->CreateRenderTargetView(work_.Get(), nullptr,
                                               work_rtv_.GetAddressOf()))) {
        *error = "cursor: work RTV creation failed";
        return false;
    }

    ComPtr<ID3DBlob> vs_blob, ps_blob;
    if (!CompileShader("vs_main", "vs_5_0", vs_blob.GetAddressOf(), error) ||
        !CompileShader("ps_main", "ps_5_0", ps_blob.GetAddressOf(), error)) {
        return false;
    }
    if (FAILED(device_->CreateVertexShader(vs_blob->GetBufferPointer(),
                                           vs_blob->GetBufferSize(), nullptr,
                                           vs_.GetAddressOf())) ||
        FAILED(device_->CreatePixelShader(ps_blob->GetBufferPointer(),
                                          ps_blob->GetBufferSize(), nullptr,
                                          ps_.GetAddressOf()))) {
        *error = "cursor: shader object creation failed";
        return false;
    }

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 16;  // float4
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&cb, nullptr, cbuffer_.GetAddressOf()))) {
        *error = "cursor: constant buffer creation failed";
        return false;
    }

    D3D11_SAMPLER_DESC samp{};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;  // cursor pixels stay crisp
    samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(device_->CreateSamplerState(&samp, sampler_.GetAddressOf()))) {
        *error = "cursor: sampler creation failed";
        return false;
    }

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&blend, blend_.GetAddressOf()))) {
        *error = "cursor: blend state creation failed";
        return false;
    }

    initialized_ = true;
    return true;
}

void CursorCompositor::OnFrameInfo(IDXGIOutputDuplication* dup,
                                   const DXGI_OUTDUPL_FRAME_INFO& frame_info) {
    if (!initialized_) return;

    if (frame_info.LastMouseUpdateTime.QuadPart != 0) {
        // Visible=false means the pointer left this output (or is hidden);
        // the position fields are only meaningful while visible.
        visible_ = frame_info.PointerPosition.Visible != FALSE;
        if (visible_) {
            pos_x_ = frame_info.PointerPosition.Position.x;
            pos_y_ = frame_info.PointerPosition.Position.y;
        }
    }

    if (frame_info.PointerShapeBufferSize != 0) {
        shape_buffer_.resize(frame_info.PointerShapeBufferSize);
        UINT required = 0;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO info{};
        if (SUCCEEDED(dup->GetFramePointerShape(
                static_cast<UINT>(shape_buffer_.size()), shape_buffer_.data(),
                &required, &info))) {
            UpdateShape(info, shape_buffer_);
        }
    }
}

void CursorCompositor::UpdateShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info,
                                   const std::vector<uint8_t>& data) {
    // Normalize every shape type to a straight-alpha BGRA sprite.
    int w = static_cast<int>(info.Width);
    int h = static_cast<int>(info.Height);
    if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        h /= 2;  // AND mask on top of XOR mask, each Height/2 rows
    }
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return;

    std::vector<uint32_t> bgra(static_cast<size_t>(w) * h, 0);
    constexpr uint32_t kInvertApprox = 0xFF808080;  // opaque mid-gray

    switch (info.Type) {
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR: {
            for (int y = 0; y < h; ++y) {
                const uint8_t* row = data.data() + y * info.Pitch;
                for (int x = 0; x < w; ++x) {
                    bgra[y * w + x] =
                        reinterpret_cast<const uint32_t*>(row)[x];
                }
            }
            break;
        }
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME: {
            // 1bpp masks, MSB first. AND=1,XOR=0 -> transparent;
            // AND=0 -> opaque black/white; AND=1,XOR=1 -> invert (approx).
            for (int y = 0; y < h; ++y) {
                const uint8_t* and_row = data.data() + y * info.Pitch;
                const uint8_t* xor_row = data.data() + (y + h) * info.Pitch;
                for (int x = 0; x < w; ++x) {
                    const int bit = 7 - (x & 7);
                    const bool and_bit = (and_row[x >> 3] >> bit) & 1;
                    const bool xor_bit = (xor_row[x >> 3] >> bit) & 1;
                    uint32_t px;
                    if (and_bit) {
                        px = xor_bit ? kInvertApprox : 0x00000000;
                    } else {
                        px = xor_bit ? 0xFFFFFFFF : 0xFF000000;
                    }
                    bgra[y * w + x] = px;
                }
            }
            break;
        }
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: {
            // Alpha byte 0 -> opaque RGB; 0xFF -> XOR with screen (approx).
            for (int y = 0; y < h; ++y) {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(
                    data.data() + y * info.Pitch);
                for (int x = 0; x < w; ++x) {
                    const uint32_t px = row[x];
                    bgra[y * w + x] = (px & 0xFF000000)
                                          ? kInvertApprox
                                          : (px | 0xFF000000);
                }
            }
            break;
        }
        default:
            return;
    }

    // Recreate the sprite texture when the size changes; else update in place.
    if (!sprite_ || w != sprite_w_ || h != sprite_h_) {
        sprite_.Reset();
        sprite_srv_.Reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(w);
        desc.Height = static_cast<UINT>(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = bgra.data();
        init.SysMemPitch = static_cast<UINT>(w * 4);
        if (FAILED(device_->CreateTexture2D(&desc, &init,
                                            sprite_.GetAddressOf())) ||
            FAILED(device_->CreateShaderResourceView(
                sprite_.Get(), nullptr, sprite_srv_.GetAddressOf()))) {
            sprite_.Reset();
            sprite_srv_.Reset();
            return;
        }
        sprite_w_ = w;
        sprite_h_ = h;
    } else {
        context_->UpdateSubresource(sprite_.Get(), 0, nullptr, bgra.data(),
                                    static_cast<UINT>(w * 4), 0);
    }
    hotspot_x_ = static_cast<int>(info.HotSpot.x);
    hotspot_y_ = static_cast<int>(info.HotSpot.y);
}

ID3D11Texture2D* CursorCompositor::Compose(ID3D11Texture2D* captured) {
    if (!initialized_) return captured;  // fail-soft: pass through
    if (captured) {
        context_->CopyResource(desktop_.Get(), captured);
        have_desktop_ = true;
    }
    if (!have_desktop_) return nullptr;  // cursor-only before first frame

    context_->CopyResource(work_.Get(), desktop_.Get());
    if (visible_ && sprite_srv_) {
        DrawPointer();
    }
    return work_.Get();
}

void CursorCompositor::DrawPointer() {
    const float x0 = static_cast<float>(pos_x_ - hotspot_x_);
    const float y0 = static_cast<float>(pos_y_ - hotspot_y_);
    // Pixel rect -> NDC (y flipped).
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(cbuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                             &mapped))) {
        return;
    }
    float* rect = static_cast<float*>(mapped.pData);
    rect[0] = x0 / width_ * 2.0f - 1.0f;
    rect[1] = 1.0f - y0 / height_ * 2.0f;
    rect[2] = (x0 + sprite_w_) / width_ * 2.0f - 1.0f;
    rect[3] = 1.0f - (y0 + sprite_h_) / height_ * 2.0f;
    context_->Unmap(cbuffer_.Get(), 0);

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(width_);
    vp.Height = static_cast<float>(height_);
    vp.MaxDepth = 1.0f;

    // Minimal state set each call; nothing else on this context draws, but
    // restore-free state bleed into AMF's CopyResource path is harmless
    // (copies ignore pipeline state).
    ID3D11RenderTargetView* rtv = work_rtv_.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    const float blend_factor[4] = {0, 0, 0, 0};
    context_->OMSetBlendState(blend_.Get(), blend_factor, 0xFFFFFFFF);
    context_->RSSetViewports(1, &vp);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->IASetInputLayout(nullptr);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, cbuffer_.GetAddressOf());
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 1, sprite_srv_.GetAddressOf());
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    context_->Draw(4, 0);

    // Unbind the RTV so the texture can be read by later copies without a
    // hazard warning.
    ID3D11RenderTargetView* null_rtv = nullptr;
    context_->OMSetRenderTargets(1, &null_rtv, nullptr);
}
