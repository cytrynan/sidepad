// pc/src/testsource.cpp
//
// A deliberately boring, deliberately trustworthy screen-change generator,
// used as the control in the capture-fps investigation.
//
// Why this exists: DXGI Desktop Duplication is event driven -- it hands back
// a frame only when the desktop image actually changes. So the capture
// spike's fps can never exceed the rate at which something on screen
// repaints. Measuring capture against a WinForms window on a 16 ms timer
// measures the timer, not the capture: WinForms timers ride the ~15.6 ms
// system tick, are subject to message-pump jitter, and are throttled when
// the window is in the background.
//
// This source instead drives a D3D11 flip-model swap chain with
// Present(1, 0), so DWM paces it to the monitor's vblank. It reports the
// present rate it actually achieved, so the capture-side number always has
// a source-side number to be compared against. If both say 40 fps, capture
// is innocent; if the source says 100 and capture says 40, capture is not.
//
// Usage: pc_test_source [run_seconds] [output_index] [novsync]
//
// novsync: Present(0, 0) instead of Present(1, 0). Flip-model without
// ALLOW_TEARING still throttles on the present queue, so the achieved rate
// approximates the DWM composition rate of that monitor -- which is the
// question when a secondary display claims 120Hz but sources pace at 60
// (mixed-refresh setups; see .steering/20260807-iddcx-virtual-display/).

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kReportEveryNPresents = 60;
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;

void FailIfErr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "[fatal] %s failed: hr=0x%08lX\n", what,
                     static_cast<unsigned long>(hr));
        std::exit(1);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Places the window on the requested DXGI output so that the capture spike,
// which duplicates one specific output, is actually looking at it. Getting
// this wrong is silent: the source runs fine, the capture sees an idle
// desktop, and the run looks like a capture failure.
RECT OutputRect(UINT output_index) {
    RECT fallback{100, 100, 100 + kWindowWidth, 100 + kWindowHeight};

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(factory.GetAddressOf())))) {
        return fallback;
    }
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(0, adapter.GetAddressOf()))) {
        return fallback;
    }
    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(output_index, output.GetAddressOf()))) {
        return fallback;
    }
    DXGI_OUTPUT_DESC desc{};
    if (FAILED(output->GetDesc(&desc))) {
        return fallback;
    }

    const LONG x = desc.DesktopCoordinates.left + 80;
    const LONG y = desc.DesktopCoordinates.top + 80;
    return RECT{x, y, x + kWindowWidth, y + kWindowHeight};
}

}  // namespace

int main(int argc, char** argv) {
    const int run_seconds = (argc > 1) ? std::atoi(argv[1]) : 15;
    const UINT output_index =
        (argc > 2) ? static_cast<UINT>(std::atoi(argv[2])) : 0u;
    const bool vsync = !(argc > 3 && std::strcmp(argv[3], "novsync") == 0);

    // Unbuffered: when stdout is a file or pipe the CRT switches to full
    // buffering, so a run that gets killed rather than exiting cleanly loses
    // every line it ever printed. That is exactly how this program is used.
    setvbuf(stdout, nullptr, _IONBF, 0);

    const RECT place = OutputRect(output_index);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SidepadTestSource";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST, wc.lpszClassName, L"sidepad test source (vsync-paced)",
        WS_OVERLAPPEDWINDOW, place.left, place.top, kWindowWidth, kWindowHeight,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "[fatal] CreateWindowExW failed\n");
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapchain;

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   device.GetAddressOf(), nullptr,
                                   context.GetAddressOf());
    FailIfErr(hr, "D3D11CreateDevice");

    ComPtr<IDXGIDevice> dxgi_device;
    FailIfErr(device.As(&dxgi_device), "QueryInterface(IDXGIDevice)");
    ComPtr<IDXGIAdapter> adapter;
    FailIfErr(dxgi_device->GetAdapter(adapter.GetAddressOf()), "GetAdapter");
    ComPtr<IDXGIFactory2> factory;
    FailIfErr(adapter->GetParent(__uuidof(IDXGIFactory2),
                                 reinterpret_cast<void**>(factory.GetAddressOf())),
              "GetParent(IDXGIFactory2)");

    // Flip-model, 2 buffers: this is the path DWM composites without an
    // extra copy, and the one Present(1, 0) paces off the vblank.
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = kWindowWidth;
    scd.Height = kWindowHeight;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    FailIfErr(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &scd, nullptr,
                                              nullptr, swapchain.GetAddressOf()),
              "CreateSwapChainForHwnd");

    std::printf("sidepad test source: %s D3D11 present\n",
                vsync ? "vsync-paced" : "queue-throttled (novsync)");
    std::printf("Running for %d seconds on output %u, %dx%d at (%ld,%ld)\n\n",
                run_seconds, output_index, kWindowWidth, kWindowHeight, place.left,
                place.top);

    const auto run_until = Clock::now() + std::chrono::seconds(run_seconds);
    long long presents_total = 0;
    long long presents_window = 0;
    long long occluded_total = 0;
    long long occluded_window = 0;
    auto window_start = Clock::now();
    float phase = 0.0f;

    while (Clock::now() < run_until) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                std::printf("\nWindow closed early after %lld presents.\n",
                            presents_total);
                return 0;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        ComPtr<ID3D11Texture2D> back_buffer;
        FailIfErr(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                       reinterpret_cast<void**>(
                                           back_buffer.GetAddressOf())),
                  "GetBuffer");
        ComPtr<ID3D11RenderTargetView> rtv;
        FailIfErr(device->CreateRenderTargetView(back_buffer.Get(), nullptr,
                                                 rtv.GetAddressOf()),
                  "CreateRenderTargetView");

        // Every frame must differ from the last, otherwise the compositor
        // has nothing new to present and Desktop Duplication has nothing to
        // report -- which would silently recreate the very problem this
        // program exists to rule out.
        phase += 0.013f;
        const float color[4] = {0.5f + 0.5f * sinf(phase),
                                0.5f + 0.5f * sinf(phase + 2.09f),
                                0.5f + 0.5f * sinf(phase + 4.19f), 1.0f};
        context->ClearRenderTargetView(rtv.Get(), color);

        // Present(1, 0): block until the next vblank. This, not a timer, is
        // what makes the source rate trustworthy. (novsync: Present(0, 0),
        // rate limited by the present queue = ~DWM composition rate.)
        hr = swapchain->Present(vsync ? 1 : 0, 0);
        if (hr == DXGI_STATUS_OCCLUDED) {
            // The window is fully covered or minimised. DWM stops
            // compositing it, Present returns immediately, and the desktop
            // stops changing -- an idle stretch on the capture side that has
            // nothing to do with capture. Counted, so it can be ruled out.
            ++occluded_total;
            ++occluded_window;
            Sleep(1);
            continue;
        }
        FailIfErr(hr, "Present");

        ++presents_total;
        ++presents_window;

        if (presents_window >= kReportEveryNPresents) {
            const auto now = Clock::now();
            const double secs =
                std::chrono::duration<double>(now - window_start).count();
            std::printf("[present %6lld] source_fps=%6.2f (window=%lld / %.3fs) "
                        "occluded=%lld\n",
                        presents_total, presents_window / secs, presents_window,
                        secs, occluded_window);
            presents_window = 0;
            occluded_window = 0;
            window_start = now;
        }
    }

    std::printf("\nDone. Total presents: %lld (occluded skips: %lld)\n",
                presents_total, occluded_total);
    return 0;
}
