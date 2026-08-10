// pc/src/cursor_compositor.h
//
// Draws the mouse pointer into the captured frame before encode.
// DXGI desktop duplication delivers the desktop WITHOUT the hardware
// cursor; a display-only extension is unusable if the pointer vanishes
// whenever it enters the iPad screen (.steering/20260810-usable-display).
//
// Usage (capture thread only):
//   Init() once after the D3D11 device exists;
//   OnFrameInfo() every AcquireNextFrame to ingest shape/position updates;
//   Compose() to get the texture to submit (captured frame + pointer).
//
// Design notes:
//   - Two private textures: `desktop_` keeps the latest clean desktop so a
//     cursor-only update can be re-composited without a fresh frame;
//     `work_` is the render target actually submitted (the encoder copies
//     it into its own pool at submit, so reuse per frame is safe).
//   - MONOCHROME / MASKED_COLOR inversion (XOR) pixels are approximated as
//     opaque mid-gray: visible on both white and black backgrounds. Exact
//     XOR needs a dest-read blend and is out of scope (requirements.md).
//   - Shaders are compiled at runtime (D3DCompile) like the iPad side's
//     Metal renderer, so no build-time shader toolchain is added.

#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

class CursorCompositor {
public:
    // Creates the desktop/work textures and the tiny draw pipeline.
    // On failure returns false; the caller may keep running without cursor
    // compositing (fail-soft: display works, pointer is just missing).
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context, int width,
              int height, std::string* error);

    // Ingest pointer state from the current duplication frame. `dup` is
    // needed to fetch the shape buffer when frame_info reports one.
    void OnFrameInfo(IDXGIOutputDuplication* dup,
                     const DXGI_OUTDUPL_FRAME_INFO& frame_info);

    // True when frame_info carried a mouse position/shape update.
    static bool MouseUpdated(const DXGI_OUTDUPL_FRAME_INFO& frame_info) {
        return frame_info.LastMouseUpdateTime.QuadPart != 0 ||
               frame_info.PointerShapeBufferSize != 0;
    }

    // Compose the frame to submit. `captured` is the locked duplication
    // texture for a desktop update, or nullptr for a cursor-only update
    // (re-uses the stored desktop copy). Returns the texture to submit,
    // or nullptr when composition is impossible (not initialized, or a
    // cursor-only update arrived before any desktop frame).
    ID3D11Texture2D* Compose(ID3D11Texture2D* captured);

    bool ready() const { return initialized_; }

private:
    void UpdateShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info,
                     const std::vector<uint8_t>& data);
    void DrawPointer();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktop_;  // latest clean frame
    Microsoft::WRL::ComPtr<ID3D11Texture2D> work_;     // composited output
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> work_rtv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sprite_;   // cursor BGRA
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sprite_srv_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cbuffer_;     // dest rect in NDC
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;

    int width_ = 0;
    int height_ = 0;
    bool initialized_ = false;
    bool have_desktop_ = false;

    // Pointer state (capture thread only, no locking needed).
    bool visible_ = false;
    int pos_x_ = 0;
    int pos_y_ = 0;
    int sprite_w_ = 0;
    int sprite_h_ = 0;
    int hotspot_x_ = 0;
    int hotspot_y_ = 0;
    std::vector<uint8_t> shape_buffer_;  // scratch for GetFramePointerShape
};
