// fsrlite - window upscaler / sharpen overlay.
//
// Flow:
//   1. Game runs borderless-windowed at a low internal resolution.
//   2. fsrlite captures that window via Windows Graphics Capture.
//   3. Each frame goes through EASU + RCAS and is presented in a click-through,
//      non-activating, topmost overlay covering the whole monitor.
//   4. The game keeps keyboard/mouse focus; you play through the overlay.
//
// Aspect: the capture is fitted into the monitor with letterbox/pillarbox,
// never stretched, so a 4:3 or ultrawide window stays correct.

#include "fsrlite.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowsapp.lib")

namespace wgc  = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
namespace wd3d = winrt::Windows::Graphics::DirectX::Direct3D11;
namespace wfnd = winrt::Windows::Foundation;

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------------ globals
static const wchar_t* kWndClass  = L"fsrlite_overlay";
static const int kHotkeyExit     = 1;   // Ctrl+Alt+Q
static const int kHotkeySharpen  = 2;   // Ctrl+Alt+S cycles sharpness
static const int kHotkeyToggle   = 3;   // Ctrl+Alt+U bypass upscale
static const UINT_PTR kTimerSync = 1;

struct App
{
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    context;
    ComPtr<IDXGISwapChain1>        swapchain;
    ComPtr<ID3D11RenderTargetView> backbufferRtv;

    wd3d::IDirect3DDevice           winrtDevice{ nullptr };
    wgc::GraphicsCaptureItem        item{ nullptr };
    wgc::Direct3D11CaptureFramePool framePool{ nullptr };
    wgc::GraphicsCaptureSession     session{ nullptr };
    wfnd::event_token               frameToken{};
    bool                            frameHooked = false;

    Upscaler upscaler;

    ComPtr<ID3D11Texture2D>          frameCopy;
    ComPtr<ID3D11ShaderResourceView> frameSrv;

    HWND     target  = nullptr;
    HWND     hwnd    = nullptr;
    HMONITOR monitor = nullptr;

    UINT frameW = 0, frameH = 0;
    UINT outW   = 0, outH   = 0;
    D3D11_VIEWPORT vp{};

    std::atomic<bool>  bypass{ false };
    std::atomic<float> sharpness{ 0.8f };

    std::wstring error;
};

static App g_app;

// The immediate context is created on the UI thread, but frames are rendered
// from the WGC callback thread. D3D11 contexts are not thread-safe: every path
// that touches the device, context or swapchain takes this lock.
static std::mutex g_renderLock;

// ------------------------------------------------------------------- device
static bool CreateDevice()
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &g_app.device, &got, &g_app.context);

    if (FAILED(hr))
        return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(g_app.device.As(&dxgiDevice)))
        return false;

    winrt::com_ptr<::IInspectable> inspectable;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put())))
        return false;

    g_app.winrtDevice = inspectable.as<wd3d::IDirect3DDevice>();
    return true;
}

// ----------------------------------------------------------------- viewport
// Letterbox/pillarbox fit of srcW:srcH inside outW:outH.
static D3D11_VIEWPORT FitViewport(UINT srcW, UINT srcH, UINT outW, UINT outH)
{
    D3D11_VIEWPORT vp{};
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    if (!srcW || !srcH || !outW || !outH)
    {
        vp.Width  = static_cast<float>(outW);
        vp.Height = static_cast<float>(outH);
        return vp;
    }

    const float srcAspect = static_cast<float>(srcW) / static_cast<float>(srcH);
    const float outAspect = static_cast<float>(outW) / static_cast<float>(outH);

    if (srcAspect > outAspect)
    {
        // wider than the screen: fill width, bars on top/bottom
        vp.Width  = static_cast<float>(outW);
        vp.Height = static_cast<float>(outW) / srcAspect;
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = (static_cast<float>(outH) - vp.Height) * 0.5f;
    }
    else
    {
        // taller than the screen: fill height, bars on the left/right
        vp.Height = static_cast<float>(outH);
        vp.Width  = static_cast<float>(outH) * srcAspect;
        vp.TopLeftX = (static_cast<float>(outW) - vp.Width) * 0.5f;
        vp.TopLeftY = 0.0f;
    }

    return vp;
}

// ---------------------------------------------------------------- swapchain
static bool CreateSwapchain(HWND hwnd, UINT w, UINT h)
{
    g_app.backbufferRtv.Reset();
    g_app.swapchain.Reset();

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(g_app.device.As(&dxgiDevice)))
        return false;

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter)))
        return false;

    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
        return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width            = w;
    desc.Height           = h;
    desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount      = 2;
    desc.SwapEffect       = DXGI_SWAP_EFFECT_DISCARD;
    desc.Scaling          = DXGI_SCALING_STRETCH;
    desc.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;

    if (FAILED(factory->CreateSwapChainForHwnd(
            g_app.device.Get(), hwnd, &desc, nullptr, nullptr, &g_app.swapchain)))
        return false;

    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(g_app.swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))))
        return false;

    if (FAILED(g_app.device->CreateRenderTargetView(backbuffer.Get(), nullptr, &g_app.backbufferRtv)))
        return false;

    g_app.outW = w;
    g_app.outH = h;
    return true;
}

static void ResizeSwapchain(UINT w, UINT h)
{
    if (!g_app.swapchain || w == 0 || h == 0)
        return;

    std::lock_guard<std::mutex> lock(g_renderLock);

    if (w == g_app.outW && h == g_app.outH)
        return;

    g_app.backbufferRtv.Reset();
    g_app.context->OMSetRenderTargets(0, nullptr, nullptr);
    g_app.context->Flush();

    if (SUCCEEDED(g_app.swapchain->ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0)))
    {
        ComPtr<ID3D11Texture2D> backbuffer;
        if (SUCCEEDED(g_app.swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))))
            g_app.device->CreateRenderTargetView(backbuffer.Get(), nullptr, &g_app.backbufferRtv);

        g_app.outW = w;
        g_app.outH = h;

        // the letterbox fit depends on the window size, recompute it
        if (g_app.frameW && g_app.frameH)
        {
            g_app.vp = FitViewport(g_app.frameW, g_app.frameH, w, h);
            g_app.upscaler.Rebuild(g_app.device.Get(), g_app.frameW, g_app.frameH,
                                   static_cast<UINT>(g_app.vp.Width),
                                   static_cast<UINT>(g_app.vp.Height));
        }
    }
}

// ------------------------------------------------------------------ capture
static wgc::GraphicsCaptureItem CreateItemForWindow(HWND hwnd)
{
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(interop->CreateForWindow(
        hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(),
        winrt::put_abi(item)));
    return item;
}

static void OnFrameArrived(wgc::Direct3D11CaptureFramePool const& pool,
                           wfnd::IInspectable const&)
{
    auto frame = pool.TryGetNextFrame();
    if (!frame)
        return;

    auto size = frame.ContentSize();
    const UINT w = static_cast<UINT>(size.Width);
    const UINT h = static_cast<UINT>(size.Height);
    if (w == 0 || h == 0)
        return;

    auto access = frame.Surface().as<
        ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(access->GetInterface(IID_PPV_ARGS(&tex))))
        return;

    std::lock_guard<std::mutex> lock(g_renderLock);

    if (!g_app.backbufferRtv || g_app.outW == 0 || g_app.outH == 0)
        return;

    if (w != g_app.frameW || h != g_app.frameH || !g_app.frameSrv)
    {
        g_app.frameW = w;
        g_app.frameH = h;
        g_app.frameSrv.Reset();
        g_app.frameCopy.Reset();

        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        td.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
        td.Usage          = D3D11_USAGE_DEFAULT;
        td.CPUAccessFlags = 0;
        td.MiscFlags      = 0;

        if (FAILED(g_app.device->CreateTexture2D(&td, nullptr, &g_app.frameCopy)))
            return;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format                    = td.Format;
        srvd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels       = 1;
        srvd.Texture2D.MostDetailedMip = 0;

        if (FAILED(g_app.device->CreateShaderResourceView(g_app.frameCopy.Get(), &srvd, &g_app.frameSrv)))
            return;

        g_app.vp = FitViewport(w, h, g_app.outW, g_app.outH);
        g_app.upscaler.Rebuild(g_app.device.Get(), w, h,
                               static_cast<UINT>(g_app.vp.Width),
                               static_cast<UINT>(g_app.vp.Height));
    }

    g_app.context->CopyResource(g_app.frameCopy.Get(), tex.Get());

    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    g_app.context->ClearRenderTargetView(g_app.backbufferRtv.Get(), clear);

    g_app.upscaler.SetSharpness(g_app.sharpness.load(std::memory_order_relaxed));

    if (g_app.bypass.load(std::memory_order_relaxed))
        g_app.upscaler.Copy(g_app.context.Get(), g_app.frameSrv.Get(),
                            g_app.backbufferRtv.Get(), g_app.vp);
    else
        g_app.upscaler.Process(g_app.context.Get(), g_app.frameSrv.Get(),
                               g_app.backbufferRtv.Get(), g_app.vp);
    // No vsync: the callback is already paced by the game's frames, and
    // blocking here would stall the capture thread.
    g_app.swapchain->Present(0, 0);
}

static bool StartCapture(HWND target)
{
    try
    {
        g_app.item = CreateItemForWindow(target);
        auto size = g_app.item.Size();

        g_app.framePool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            g_app.winrtDevice,
            wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            size);

        g_app.session = g_app.framePool.CreateCaptureSession(g_app.item);

        try { g_app.session.IsBorderRequired(false); }
        catch (...) { /* older build, capture border stays */ }

        g_app.session.IsCursorCaptureEnabled(false);

        g_app.frameToken = g_app.framePool.FrameArrived(
            [](wgc::Direct3D11CaptureFramePool const& pool, wfnd::IInspectable const& args)
            {
                OnFrameArrived(pool, args);
            });
        g_app.frameHooked = true;

        g_app.session.StartCapture();
        return true;
    }
    catch (winrt::hresult_error const& e)
    {
        g_app.error = e.message().c_str();
        return false;
    }
}

static void StopCapture()
{
    if (g_app.framePool && g_app.frameHooked)
    {
        g_app.framePool.FrameArrived(g_app.frameToken);
        g_app.frameHooked = false;
    }
    if (g_app.session)   { g_app.session.Close();   g_app.session = nullptr; }
    if (g_app.framePool) { g_app.framePool.Close(); g_app.framePool = nullptr; }
    g_app.item = nullptr;
}

// ------------------------------------------------------------------- window
static bool MonitorRect(HMONITOR mon, RECT* out)
{
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi))
        return false;
    *out = mi.rcMonitor;
    return true;
}

static void FitOverlayToMonitor(HWND hwnd, HMONITOR mon)
{
    RECT rc{};
    if (!MonitorRect(mon, &rc))
        return;

    SetWindowPos(hwnd, HWND_TOPMOST,
                 rc.left, rc.top,
                 rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void SyncOverlaySize(HWND hwnd)
{
    RECT rc{};
    GetClientRect(hwnd, &rc);
    ResizeSwapchain(static_cast<UINT>(rc.right - rc.left),
                    static_cast<UINT>(rc.bottom - rc.top));
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_HOTKEY:
        if (wp == kHotkeyExit)
        {
            PostQuitMessage(0);
        }
        else if (wp == kHotkeySharpen)
        {
            float s = g_app.sharpness.load();
            s = (s >= 1.5f) ? 0.0f : s + 0.5f;
            g_app.sharpness.store(s);
        }
        else if (wp == kHotkeyToggle)
        {
            g_app.bypass.store(!g_app.bypass.load());
        }
        return 0;

    case WM_TIMER:
        if (wp == kTimerSync)
        {
            if (!IsWindow(g_app.target) || !IsWindowVisible(g_app.target))
            {
                PostQuitMessage(0);
                return 0;
            }

            HMONITOR mon = MonitorFromWindow(g_app.target, MONITOR_DEFAULTTONEAREST);
            if (mon != g_app.monitor)
            {
                g_app.monitor = mon;
                FitOverlayToMonitor(hwnd, mon);
                SyncOverlaySize(hwnd);
            }
        }
        return 0;

    case WM_DISPLAYCHANGE:
        FitOverlayToMonitor(hwnd, g_app.monitor);
        SyncOverlaySize(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --------------------------------------------------------------------- main
int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        wprintf(L"usage: fsrlite.exe <window-title-substring> [sharpness 0..2]\n");
        wprintf(L"  hotkeys: Ctrl+Alt+Q quit, Ctrl+Alt+S cycle sharpness, Ctrl+Alt+U bypass\n");
        return 1;
    }

    const std::wstring titleHint = argv[1];
    if (argc >= 3)
        g_app.sharpness.store(std::clamp(static_cast<float>(_wtof(argv[2])), 0.0f, 2.0f));

    HWND target = nullptr;
    for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT))
    {
        if (!IsWindowVisible(w)) continue;
        wchar_t buf[512]{};
        GetWindowTextW(w, buf, 512);
        if (wcsstr(buf, titleHint.c_str()))
        {
            target = w;
            break;
        }
    }

    if (!target)
    {
        wprintf(L"fsrlite: no visible window matching \"%ls\"\n", titleHint.c_str());
        return 2;
    }

    g_app.target  = target;
    g_app.monitor = MonitorFromWindow(target, MONITOR_DEFAULTTOPRIMARY);

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    if (!CreateDevice())
    {
        wprintf(L"fsrlite: D3D11 device creation failed\n");
        return 3;
    }

    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWndClass;
    if (!RegisterClassExW(&wc))
        return 4;

    // Click-through, non-activating, topmost. Deliberately NOT WS_EX_LAYERED:
    // a DXGI swapchain cannot present into a layered window, and the overlay
    // is fully opaque anyway.
    g_app.hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kWndClass, L"fsrlite overlay",
        WS_POPUP,
        0, 0, 640, 480,
        nullptr, nullptr, inst, nullptr);

    if (!g_app.hwnd)
        return 5;

    FitOverlayToMonitor(g_app.hwnd, g_app.monitor);
    ShowWindow(g_app.hwnd, SW_SHOWNOACTIVATE);

    RECT rc{};
    GetClientRect(g_app.hwnd, &rc);

    if (!CreateSwapchain(g_app.hwnd,
                         static_cast<UINT>(rc.right - rc.left),
                         static_cast<UINT>(rc.bottom - rc.top)))
    {
        wprintf(L"fsrlite: swapchain creation failed\n");
        return 6;
    }

    if (!g_app.upscaler.Init(g_app.device.Get()))
    {
        wprintf(L"fsrlite: upscaler init failed: %ls\n", g_app.upscaler.Error().c_str());
        return 7;
    }
    g_app.upscaler.SetSharpness(g_app.sharpness.load());

    RegisterHotKey(g_app.hwnd, kHotkeyExit,    MOD_CONTROL | MOD_ALT, 'Q');
    RegisterHotKey(g_app.hwnd, kHotkeySharpen, MOD_CONTROL | MOD_ALT, 'S');
    RegisterHotKey(g_app.hwnd, kHotkeyToggle,  MOD_CONTROL | MOD_ALT, 'U');
    SetTimer(g_app.hwnd, kTimerSync, 250, nullptr);

    if (!StartCapture(target))
    {
        wprintf(L"fsrlite: capture failed: %ls\n", g_app.error.c_str());
        return 8;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StopCapture();
    return 0;
}
