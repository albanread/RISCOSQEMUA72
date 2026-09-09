/*
 * dx11.cpp — the native Windows front end for the emulated Pi 4.
 *
 * Sprint U0: the window owns the main thread.  QEMU's main loop runs on
 * the "qemu_main" thread through the hand-off system/main.c provides;
 * this thread owns the Win32 window and a D3D11 swap chain that clears
 * at vsync.  The framebuffer decode shaders arrive in Sprint U1; for
 * now the window shows a dark frame while the emulation boots behind
 * it, verified over QMP.
 *
 * This file includes no QEMU headers — they are C, not C++ — and talks
 * to the emulation only through ui/dx11.h's extern "C" boundary.
 */

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "ui/dx11.h"

/* ------------------------------------------------------------------ */
/* State, owned by the UI thread only                                  */

static struct {
    HWND hwnd;
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGISwapChain *swap;
    ID3D11RenderTargetView *rtv;
    bool ready;
    bool lost;
} dx11;

static const wchar_t DX11_CLASS[] = L"qemu-dx11";
static const wchar_t DX11_TITLE[] = L"RISC OS 5 — Raspberry Pi 4 (QEMU)";

/* ------------------------------------------------------------------ */
/* Window                                                              */

static LRESULT CALLBACK dx11_wndproc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_DESTROY:
        dx11.lost = true;
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (dx11.swap && w != SIZE_MINIMIZED) {
            if (dx11.rtv) {
                dx11.rtv->Release();
                dx11.rtv = nullptr;
            }
            dx11.swap->ResizeBuffers(0, LOWORD(l), HIWORD(l),
                                     DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        }
        return 0;
    default:
        return DefWindowProcW(h, msg, w, l);
    }
}

static bool dx11_create_window(void)
{
    WNDCLASSW wc = {};
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = dx11_wndproc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = DX11_CLASS;
    if (!RegisterClassW(&wc)) {
        return false;
    }

    /* Default client size: the EDID mode the firmware answers with. */
    RECT r = { 0, 0, 800, 600 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    dx11.hwnd = CreateWindowExW(
        0, DX11_CLASS, DX11_TITLE, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!dx11.hwnd) {
        return false;
    }
    ShowWindow(dx11.hwnd, SW_SHOW);
    UpdateWindow(dx11.hwnd);
    return true;
}

/* ------------------------------------------------------------------ */
/* D3D11: device, flip-model swap chain, per-frame clear at vsync      */

static bool dx11_create_device(void)
{
    UINT flags = 0;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION,
        &dx11.device, &fl, &dx11.context);
    if (FAILED(hr)) {
        /* WARP fallback: software rendering, correctness first (U3). */
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION,
            &dx11.device, &fl, &dx11.context);
        if (FAILED(hr)) {
            return false;
        }
    }

    RECT client;
    GetClientRect(dx11.hwnd, &client);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferDesc.Width  = client.right;
    scd.BufferDesc.Height = client.bottom;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count  = 1;
    scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount       = 2;
    scd.OutputWindow      = dx11.hwnd;
    scd.Windowed          = TRUE;
    scd.SwapEffect        = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    /* The factory must come from the device's adapter or ResizeBuffers
     * is refused later: IDXGIDevice -> IDXGIAdapter -> IDXGIFactory. */
    IDXGIDevice *dxdev = nullptr;
    IDXGIAdapter *adapter = nullptr;
    IDXGIFactory *factory = nullptr;
    if (SUCCEEDED(dx11.device->QueryInterface(IID_PPV_ARGS(&dxdev)))
        && SUCCEEDED(dxdev->GetAdapter(&adapter))
        && SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        adapter->Release();
    } else {
        if (adapter) adapter->Release();
        if (dxdev) dxdev->Release();
        CreateDXGIFactory(IID_PPV_ARGS(&factory));
    }
    if (dxdev) dxdev->Release();
    hr = factory->CreateSwapChain(dx11.device, &scd, &dx11.swap);
    factory->Release();
    if (FAILED(hr)) {
        /* Older drivers: the blt model still clears a window. */
        scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        IDXGIFactory *f2 = nullptr;
        CreateDXGIFactory(IID_PPV_ARGS(&f2));
        hr = f2->CreateSwapChain(dx11.device, &scd, &dx11.swap);
        f2->Release();
        if (FAILED(hr)) {
            return false;
        }
    }
    return true;
}

static bool dx11_acquire_target(void)
{
    if (dx11.rtv) {
        return true;
    }
    ID3D11Texture2D *back = nullptr;
    if (FAILED(dx11.swap->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        return false;
    }
    HRESULT hr = dx11.device->CreateRenderTargetView(
        back, nullptr, &dx11.rtv);
    back->Release();
    return SUCCEEDED(hr);
}

/* ------------------------------------------------------------------ */
/* The boundary, called from ui/dx11.c                                 */

extern "C" int dx11_backend_init(void)
{
    if (!dx11_create_window()) {
        return -1;
    }
    if (!dx11_create_device()) {
        return -1;
    }
    dx11.ready = true;
    return 0;
}

extern "C" int dx11_backend_main(void)
{
    if (!dx11.ready) {
        return 1;
    }
    /* A frame: clear the back buffer and present at vsync.  Nothing here
     * touches QEMU state; U1 turns this into upload+decode+scale. */
    const float clear[] = { 0.05f, 0.05f, 0.08f, 1.0f };

    while (!dx11.lost) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                dx11.lost = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (dx11.lost) {
            break;
        }
        if (dx11_acquire_target()) {
            dx11.context->ClearRenderTargetView(dx11.rtv, clear);
            dx11.swap->Present(1 /* vsync */, 0);
        } else {
            Sleep(1);
        }
    }

    dx11_backend_request_shutdown();

    if (dx11.rtv) dx11.rtv->Release();
    if (dx11.swap) dx11.swap->Release();
    if (dx11.context) dx11.context->Release();
    if (dx11.device) dx11.device->Release();
    dx11.ready = false;
    return 0;
}
