/*
 * dx11.cpp — the native Windows front end for the emulated Pi 4.
 *
 * Sprint U1: the guest framebuffer, decoded on the GPU.  Each frame the
 * UI thread pulls the fb view over ui/dx11.h's boundary (config snapshot
 * plus host pointers into guest RAM, no locks), uploads the raw bytes to
 * a dynamic texture, decodes them to linear RGB with a per-format pixel
 * shader, and stretches the decoded surface over the whole client area —
 * the window is the monitor, so every mode fills it, as the Pi's GPU
 * fills a real display.  8 bpp palette and 32 bpp are the two formats
 * RISC OS's BCMVideo ever asks for; the rest of the decoder set is the
 * same source with different constants.
 *
 * This file includes no QEMU headers — they are C, not C++ — and talks
 * to the emulation only through ui/dx11.h's extern "C" boundary.
 */

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <zlib.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

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
    /* mouse messages forwarded, for the debug log */
    unsigned mouse_moves;
    unsigned mouse_buttons;
} dx11;

/* The per-mode pipeline: everything that depends on the fb config. */
static struct {
    uint32_t generation;
    uint32_t bpp;
    uint32_t xres, yres, pitch, rows;

    ID3D11Texture2D *raw;           /* guest bytes, pitch x rows */
    ID3D11ShaderResourceView *raw_srv;
    ID3D11Texture2D *palette;       /* 256 x 1 RGBA8 */
    ID3D11ShaderResourceView *pal_srv;
    uint8_t pal_cache[256 * 4];

    ID3D11Texture2D *decoded;       /* linear RGB, xres x yres */
    ID3D11RenderTargetView *dec_rtv;
    ID3D11ShaderResourceView *dec_srv;

    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;          /* per-format decode */
    ID3D11PixelShader *scale_ps;
    ID3D11Buffer *cbuf;
    ID3D11SamplerState *linear;
    bool up;
} fb;

static const wchar_t DX11_CLASS[] = L"qemu-dx11";
static const wchar_t DX11_TITLE[] = L"RISC OS 5 — Raspberry Pi 4 (QEMU)";

/* Development log: dx11-debug.txt next to the CWD, one line per notable
 * event, so failures on a windowed app are not lost to OutputDebugString. */
static void dx11_log(const char *fmt, ...)
{
    FILE *f = fopen("dx11-debug.txt", "a");
    if (!f) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void dx11_screenshot(void);     /* PrintScreen: decoded surface to a PNG */

/* ------------------------------------------------------------------ */
/* Grab: the guest owns the keyboard and the pointer until Ctrl+Alt+G  */

static struct {
    bool on;
} kbd;

static void dx11_mouse_centre(HWND h);
/* mouse state, see the Mouse section */
static struct {
    bool have_last;
    int last_x, last_y;
} mouse;

static void dx11_set_grab(bool on)
{
    if (on == kbd.on) {
        return;
    }
    kbd.on = on;
    dx11_glue_grab(on);             /* hook swallows Alt+Tab / Win keys */
    if (on) {
        RECT r;
        GetWindowRect(dx11.hwnd, &r);
        ClipCursor(&r);             /* the pointer lives in the window */
        ShowCursor(FALSE);
        dx11_mouse_centre(dx11.hwnd);
    } else {
        ClipCursor(nullptr);
        ShowCursor(TRUE);
        mouse.have_last = false;
    }
    dx11_log("grab %s", on ? "on" : "off");
}

/* ------------------------------------------------------------------ */
/* Mouse: ordinary window messages, turned into the relative motion a  */
/* USB mouse reports. Grabbed, the host cursor is warped back to the   */
/* centre after every move so motion is unbounded; ungrabbed, the      */
/* deltas are simply the cursor's movement over the client area.       */

static void dx11_mouse_centre(HWND h)
{
    RECT r;
    POINT c;

    GetClientRect(h, &r);
    c.x = (r.right - r.left) / 2;
    c.y = (r.bottom - r.top) / 2;
    mouse.last_x = c.x;
    mouse.last_y = c.y;
    mouse.have_last = true;
    ClientToScreen(h, &c);
    SetCursorPos(c.x, c.y);
}

static void dx11_mouse_move(HWND h, int x, int y)
{
    /* No leave tracking: a return after leaving the window is one large
     * delta, which is the motion the user actually made. */
    if (mouse.have_last) {
        int dx = x - mouse.last_x, dy = y - mouse.last_y;
        if (dx || dy) {
            dx11_glue_mouse_rel(dx, dy);
            dx11.mouse_moves++;
        }
    }
    mouse.last_x = x;
    mouse.last_y = y;
    mouse.have_last = true;
    if (kbd.on) {
        RECT r;
        GetClientRect(h, &r);
        if (x != (r.right - r.left) / 2 || y != (r.bottom - r.top) / 2) {
            dx11_mouse_centre(h);   /* its own WM_MOUSEMOVE is a zero delta */
        }
    }
}

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
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) {
            dx11_set_grab(false);   /* never hold the host pointer hostage */
        }
        /* DefWindowProc gives an activated window the keyboard focus;
         * without it the window is foreground but focusless, and raw
         * mouse input is not delivered to it. */
        return DefWindowProcW(h, msg, w, l);
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        /* Ctrl+Alt+G is the release, and never reaches the guest */
        if (w == 'G' && (GetKeyState(VK_CONTROL) & 0x8000)
                     && (GetKeyState(VK_MENU) & 0x8000)) {
            dx11_set_grab(!kbd.on);
            return 0;
        }
        if (w == VK_SNAPSHOT) {
            return 0;               /* PrintScreen acts on the up event */
        }
        if (l & (1u << 30)) {
            return 0;               /* auto-repeat: the guest does its own */
        }
        dx11_glue_key(true, l);
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        /* PrintScreen only ever arrives as an up event; no down precedes
         * it, so the screenshot hook lives here. */
        if (w == VK_SNAPSHOT) {
            dx11_screenshot();
            return 0;
        }
        dx11_glue_key(false, l);
        return 0;
    case WM_MOUSEMOVE:
        dx11_mouse_move(h, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
        SetCapture(h);                  /* the up arrives even outside */
        dx11_glue_mouse_btn(msg == WM_LBUTTONDOWN ? 0
                            : msg == WM_MBUTTONDOWN ? 1 : 2, true);
        dx11.mouse_buttons++;
        return 0;
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
        dx11_glue_mouse_btn(msg == WM_LBUTTONUP ? 0
                            : msg == WM_MBUTTONUP ? 1 : 2, false);
        if (!(w & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON))) {
            ReleaseCapture();
        }
        return 0;
    case WM_MOUSEWHEEL:
        dx11_glue_mouse_wheel(GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA);
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
    /* without a class brush the pre-first-Present client area is whatever
     * the desktop left behind -- the white flash; own it as black */
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
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
    /*
     * The first ShowWindow of a process honours wShowWindow from
     * STARTUPINFO over its argument -- a launcher started with "hidden"
     * steals this call and leaves the window invisible. Call it twice:
     * the first absorbs the override, the second shows for real.
     */
    ShowWindow(dx11.hwnd, SW_SHOW);
    ShowWindow(dx11.hwnd, SW_SHOW);
    UpdateWindow(dx11.hwnd);

    return true;
}

/* ------------------------------------------------------------------ */
/* D3D11: device, flip-model swap chain                                */

static bool dx11_create_device(void)
{
    /* DX11_DEBUG=1 asks for the debug layer (needs the Graphics Tools
     * feature): the runtime then names the exact offending call. */
    UINT flags = getenv("DX11_DEBUG") ? D3D11_CREATE_DEVICE_DEBUG : 0;
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
/* Shaders: one source, one decode variant per format                  */

static const char SHADER_SRC[] = R"xxx(

struct Params {
    uint4 dim;     /* xres, yres, pitch(bytes), bpp */
    uint4 misc;    /* xoffset(px), yoffset(rows), pixo, unused */
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID)
{
    /* one big triangle covering the target; uv happens to be the same
     * (0..2) coordinates the position is built from, so the interpolated
     * uv runs 0..1 across the visible area with y=0 at the top */
    VSOut o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}

#ifdef DECODE

Texture2D<uint>   raw : register(t0);
Texture2D<float4> pal : register(t1);

cbuffer params : register(b0) { Params P; }

float4 ps_main(VSOut v) : SV_Target
{
    uint x = (uint)v.pos.x;
    uint y = (uint)v.pos.y;

    /* visible pixel -> buffer coordinates, pan included */
    uint bx = x + P.misc.x;
    uint by = y + P.misc.y;

    uint r = 0, g = 0, b = 0;   /* palettised returns early; fxc still
                                 * compiles the tail return and rejects
                                 * uninitialised reads (X4000) */
#if BPP == 32
    r = raw.Load(int3(bx * 4 + 0, by, 0)).r;
    g = raw.Load(int3(bx * 4 + 1, by, 0)).r;
    b = raw.Load(int3(bx * 4 + 2, by, 0)).r;
    if (P.misc.z == 0) { uint t = r; r = b; b = t; }   /* BGR order */
#elif BPP == 24
    r = raw.Load(int3(bx * 3 + 0, by, 0)).r;
    g = raw.Load(int3(bx * 3 + 1, by, 0)).r;
    b = raw.Load(int3(bx * 3 + 2, by, 0)).r;
#elif BPP == 16
    uint w = raw.Load(int3(bx * 2, by, 0)).r
           | (raw.Load(int3(bx * 2 + 1, by, 0)).r << 8);   /* RGB565 */
    r = ((w >> 11) & 31) * 255 / 31;
    g = ((w >>  5) & 63) * 255 / 63;
    b = ((w      ) & 31) * 255 / 31;
#else
    /* palettised: 8 bpp, or sub-byte 1/2/4 with LSB-first packing */
    uint bpp = P.dim.w;
    uint byte = raw.Load(int3(bx * bpp / 8, by, 0)).r;
    uint idx = (byte >> ((bx * bpp) & 7)) & ((1u << bpp) - 1);
    float4 c = pal.Load(int3(idx, 0, 0));
    return float4(c.rgb, 1);
#endif
    return float4(r / 255.0, g / 255.0, b / 255.0, 1);
}

#else /* the scale pass: decoded surface over the whole client area */

Texture2D<float4> src : register(t0);
SamplerState lin : register(s0);

float4 ps_main(VSOut v) : SV_Target
{
    float4 c = src.Sample(lin, v.uv);
    return float4(c.rgb, 1);
}

#endif
)xxx";

static ID3D11PixelShader *compile_ps(const char *target,
                                     const char **defines /* name,value pairs */,
                                     size_t npairs)
{
    D3D_SHADER_MACRO macros[8] = {};
    size_t n = 0;
    for (size_t i = 0; i + 1 < 2 * npairs && n + 1 < 7; i += 2) {
        macros[n].Name = defines[i];
        macros[n].Definition = defines[i + 1];
        n++;
    }

    ID3DBlob *code = nullptr, *errs = nullptr;
    HRESULT hr = D3DCompile(SHADER_SRC, strlen(SHADER_SRC), nullptr, macros,
                            nullptr, "ps_main", target, 0, 0, &code, &errs);
    if (FAILED(hr)) {
        dx11_log("ps compile failed: %#x", (unsigned)hr);
        if (errs) {
            dx11_log("ps compile: %s", (const char *)errs->GetBufferPointer());
            errs->Release();
        }
        return nullptr;
    }
    ID3D11PixelShader *ps = nullptr;
    hr = dx11.device->CreatePixelShader(code->GetBufferPointer(),
                                        code->GetBufferSize(), nullptr, &ps);
    code->Release();
    if (FAILED(hr)) {
        dx11_log("CreatePixelShader failed: %#x", (unsigned)hr);
    }
    return ps;
}

static ID3D11VertexShader *compile_vs(void)
{
    ID3DBlob *code = nullptr, *errs = nullptr;
    HRESULT hr = D3DCompile(SHADER_SRC, strlen(SHADER_SRC), nullptr, nullptr,
                            nullptr, "vs_main", "vs_4_0", 0, 0, &code, &errs);
    if (FAILED(hr)) {
        dx11_log("vs compile failed: %#x", (unsigned)hr);
        if (errs) {
            dx11_log("vs compile: %s", (const char *)errs->GetBufferPointer());
            errs->Release();
        }
        return nullptr;
    }
    ID3D11VertexShader *vs = nullptr;
    hr = dx11.device->CreateVertexShader(code->GetBufferPointer(),
                                         code->GetBufferSize(), nullptr, &vs);
    code->Release();
    if (FAILED(hr)) {
        dx11_log("CreateVertexShader failed: %#x", (unsigned)hr);
    }
    return vs;
}

/* ------------------------------------------------------------------ */
/* The per-mode pipeline                                               */

static void fb_release_pipeline(void)
{
    if (fb.raw_srv) fb.raw_srv->Release();
    if (fb.raw) fb.raw->Release();
    if (fb.pal_srv) fb.pal_srv->Release();
    if (fb.palette) fb.palette->Release();
    if (fb.dec_srv) fb.dec_srv->Release();
    if (fb.dec_rtv) fb.dec_rtv->Release();
    if (fb.decoded) fb.decoded->Release();
    if (fb.ps) fb.ps->Release();
    fb.raw_srv = nullptr;
    fb.raw = nullptr;
    fb.pal_srv = nullptr;
    fb.palette = nullptr;
    fb.dec_srv = nullptr;
    fb.dec_rtv = nullptr;
    fb.decoded = nullptr;
    fb.ps = nullptr;
    fb.up = false;
    memset(fb.pal_cache, 0xaa, sizeof(fb.pal_cache)); /* force re-upload */
}

/* The decoder set: RISC OS only ever asks for 8 and 32 bpp, the rest are
 * the same source with different constants and cost nothing to keep. All
 * variants are compiled up front, so a broken decoder is a start-up log
 * line rather than a blank screen on the first mode change. */
static const uint32_t FB_BPPS[] = { 32, 8, 16, 24, 4, 2, 1 };
static ID3D11PixelShader *fb_ps_all[7];

static bool fb_compile_shaders(void)
{
    char bppval[16];
    const char *defines[] = { "DECODE", "1", "BPP", bppval };
    size_t ok = 0;

    fb.vs = compile_vs();
    fb.scale_ps = compile_ps("ps_4_0", nullptr, 0);
    if (!fb.vs || !fb.scale_ps) {
        return false;                   /* nothing works without these */
    }
    /*
     * Compile every variant, not just up to the first failure: a later
     * mode must find its decoder either present or logged-absent, never
     * a half-filled table behind an early return.
     */
    for (size_t i = 0; i < sizeof(FB_BPPS) / sizeof(FB_BPPS[0]); i++) {
        snprintf(bppval, sizeof(bppval), "%u", FB_BPPS[i]);
        fb_ps_all[i] = compile_ps("ps_4_0", defines, 2);
        if (fb_ps_all[i]) {
            ok++;
        } else {
            dx11_log("decoder for %u bpp failed to compile", FB_BPPS[i]);
        }
    }
    dx11_log("%zu of %zu decoders compiled", ok,
             sizeof(FB_BPPS) / sizeof(FB_BPPS[0]));
    return ok > 0;
}

static bool fb_build_pipeline(const Dx11FbView *v)
{
    HRESULT hr;

    fb_release_pipeline();

    /* shaders first: the decoders are picked from the pre-compiled set
     * below, which must exist before anything references them */
    if (!fb.vs && !fb_compile_shaders()) {
        return false;
    }

    /* raw guest bytes: one texel per byte */
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = v->pitch;
    td.Height = v->rows;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = dx11.device->CreateTexture2D(&td, nullptr, &fb.raw);
    if (FAILED(hr)) {
        dx11_log("raw texture %ux%u failed: %#x", v->pitch, v->rows, (unsigned)hr);
        return false;
    }
    /* a null desc gives a view in the resource's own format, which is
     * what every texture here wants; an explicit desc has to repeat the
     * format exactly or CreateShaderResourceView rejects it */
    hr = dx11.device->CreateShaderResourceView(fb.raw, nullptr, &fb.raw_srv);
    if (FAILED(hr)) {
        dx11_log("raw SRV failed: %#x", (unsigned)hr);
        return false;
    }

    /* the palette at the VideoCore RAM base: 0x00BBGGRR words, which as
     * little-endian bytes are R,G,B,x — an RGBA8 texture straight copy */
    D3D11_TEXTURE2D_DESC pd = {};
    pd.Width = 256;
    pd.Height = 1;
    pd.MipLevels = 1;
    pd.ArraySize = 1;
    pd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    pd.Usage = D3D11_USAGE_DYNAMIC;
    pd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    pd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = dx11.device->CreateTexture2D(&pd, nullptr, &fb.palette);
    if (FAILED(hr)) {
        dx11_log("palette texture failed: %#x", (unsigned)hr);
        return false;
    }
    hr = dx11.device->CreateShaderResourceView(fb.palette, nullptr, &fb.pal_srv);
    if (FAILED(hr)) {
        dx11_log("palette SRV failed: %#x", (unsigned)hr);
        return false;
    }

    /* the decoded, linear-RGB surface the decoders render into */
    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = v->xres;
    dd.Height = v->yres;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = dx11.device->CreateTexture2D(&dd, nullptr, &fb.decoded);
    if (FAILED(hr)) {
        dx11_log("decoded texture %ux%u failed: %#x", v->xres, v->yres, (unsigned)hr);
        return false;
    }
    hr = dx11.device->CreateRenderTargetView(fb.decoded, nullptr, &fb.dec_rtv);
    if (FAILED(hr)) {
        dx11_log("decoded RTV failed: %#x", (unsigned)hr);
        return false;
    }
    hr = dx11.device->CreateShaderResourceView(fb.decoded, nullptr, &fb.dec_srv);
    if (FAILED(hr)) {
        dx11_log("decoded SRV failed: %#x", (unsigned)hr);
        return false;
    }

    /* the decoder for this mode */
    size_t i;
    for (i = 0; i < sizeof(FB_BPPS) / sizeof(FB_BPPS[0]); i++) {
        if (FB_BPPS[i] == v->bpp) {
            break;
        }
    }
    if (i == sizeof(FB_BPPS) / sizeof(FB_BPPS[0])) {
        dx11_log("no decoder for bpp=%u", v->bpp);
        return false;
    }
    if (!fb_ps_all[i]) {
        dx11_log("decoder for bpp=%u did not compile (see log)", v->bpp);
        return false;                   /* logged clear-screen, never a call */
    }
    fb.ps = fb_ps_all[i];
    fb.ps->AddRef();             /* released by fb_release_pipeline */

    /* constant buffer: rebuilt per config, updated on every build */
    struct { uint32_t dim[4]; uint32_t misc[4]; } cb = {
        { v->xres, v->yres, v->pitch, v->bpp },
        { v->xoffset, v->yoffset, v->pixo, 0 },
    };
    if (fb.cbuf) {
        dx11.context->UpdateSubresource(fb.cbuf, 0, nullptr, &cb, 0, 0);
    } else {
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.ByteWidth = sizeof(cb);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = dx11.device->CreateBuffer(&bd, nullptr, &fb.cbuf);
        if (FAILED(hr)) {
            dx11_log("constant buffer failed: %#x", (unsigned)hr);
            return false;
        }
        dx11.context->UpdateSubresource(fb.cbuf, 0, nullptr, &cb, 0, 0);
    }

    if (fb.linear) {
        /* sampler kept across modes */
    } else {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = dx11.device->CreateSamplerState(&sd, &fb.linear);
        if (FAILED(hr)) {
            dx11_log("sampler failed: %#x", (unsigned)hr);
            return false;
        }
    }

    fb.generation = v->generation;
    fb.bpp = v->bpp;
    fb.xres = v->xres;
    fb.yres = v->yres;
    fb.pitch = v->pitch;
    fb.rows = v->rows;
    fb.up = true;
    dx11_log("pipeline built: gen %u, %ux%u, pitch %u, bpp %u, pan %u,%u",
             v->generation, v->xres, v->yres, v->pitch, v->bpp,
             v->xoffset, v->yoffset);
    return true;
}

/* The generation a failed build was tried against, so a failure is not
 * retried every frame (three texture allocations at 60 Hz for as long as
 * the mode lasts); a new mode clears it. */
static uint32_t fb_failed_generation;
static bool fb_failed;

/* One frame of the guest's screen.  Returns false if there is nothing to
 * show yet (clear instead). */
static uint32_t frame_count;

static void fb_upload(const Dx11FbView *v)
{
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT hr;

    hr = dx11.context->Map(fb.raw, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
    if (SUCCEEDED(hr)) {
        const uint8_t *src = (const uint8_t *)v->fb;
        for (uint32_t y = 0; y < v->rows; y++) {
            memcpy((uint8_t *)map.pData + (size_t)y * map.RowPitch,
                   src + (size_t)y * v->pitch, v->pitch);
        }
        dx11.context->Unmap(fb.raw, 0);
    } else if (frame_count % 300 == 0) {
        dx11_log("raw Map failed: %#x", (unsigned)hr);
    }

    if (memcmp(fb.pal_cache, v->palette, sizeof(fb.pal_cache)) != 0) {
        memcpy(fb.pal_cache, v->palette, sizeof(fb.pal_cache));
        hr = dx11.context->Map(fb.palette, 0, D3D11_MAP_WRITE_DISCARD, 0,
                               &map);
        if (SUCCEEDED(hr)) {
            memcpy(map.pData, fb.pal_cache, sizeof(fb.pal_cache));
            dx11.context->Unmap(fb.palette, 0);
        } else {
            dx11_log("palette Map failed: %#x", (unsigned)hr);
        }
    }
}

static bool dx11_render_frame(void)
{
    Dx11FbView v;

    if (++frame_count % 300 == 0) {
        dx11_log("frame %u: pipeline %s, fb gen %u, %ux%u bpp %u, "
                 "mouse moves %u, button events %u",
                 frame_count, fb.up ? "up" : "down",
                 fb.generation, fb.xres, fb.yres, fb.bpp,
                 dx11.mouse_moves, dx11.mouse_buttons);
    }

    if (!dx11_glue_fb_view(&v)) {
        if (frame_count % 300 == 0) {
            dx11_log("frame %u: no fb view yet", frame_count);
        }
        return false;
    }
    if (!fb.up || v.generation != fb.generation) {
        if (fb_failed && v.generation == fb_failed_generation) {
            return false;               /* same mode, same failure */
        }
        if (!fb_build_pipeline(&v)) {
            fb_failed = true;
            fb_failed_generation = v.generation;
            return false;
        }
        fb_failed = false;
    }

    fb_upload(&v);

    /* decode pass: raw bytes -> linear RGB */
    ID3D11ShaderResourceView *srvs[2] = { fb.raw_srv, fb.pal_srv };
    dx11.context->PSSetShaderResources(0, 2, srvs);
    dx11.context->PSSetShader(fb.ps, nullptr, 0);
    dx11.context->VSSetShader(fb.vs, nullptr, 0);
    dx11.context->PSSetConstantBuffers(0, 1, &fb.cbuf);
    dx11.context->OMSetRenderTargets(1, &fb.dec_rtv, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)v.xres, (float)v.yres, 0, 1 };
    dx11.context->RSSetViewports(1, &vp);
    dx11.context->Draw(3, 0);

    /* scale pass: decoded surface over the whole client area */
    ID3D11ShaderResourceView *dec = fb.dec_srv;
    dx11.context->OMSetRenderTargets(1, &dx11.rtv, nullptr);
    dx11.context->PSSetShaderResources(0, 1, &dec);
    dx11.context->PSSetShader(fb.scale_ps, nullptr, 0);
    dx11.context->PSSetSamplers(0, 1, &fb.linear);
    RECT client;
    GetClientRect(dx11.hwnd, &client);
    D3D11_VIEWPORT vp2 = { 0, 0,
                           (float)(client.right - client.left),
                           (float)(client.bottom - client.top), 0, 1 };
    dx11.context->RSSetViewports(1, &vp2);
    dx11.context->Draw(3, 0);

    /* unbind the decoded SRV before a later frame makes it a render target
     * again; leaving it bound is a resource hazard the runtime only warns
     * about under the debug layer */
    ID3D11ShaderResourceView *none = nullptr;
    dx11.context->PSSetShaderResources(0, 1, &none);
    return true;
}

/* ------------------------------------------------------------------ */
/* Screenshot: the decoded surface to a PNG file                       */

static void png_write_chunk(FILE *f, const char tag[4], const void *data,
                            uint32_t len)
{
    uint8_t hdr[8] = { (uint8_t)(len >> 24), (uint8_t)(len >> 16),
                       (uint8_t)(len >> 8), (uint8_t)len,
                       (uint8_t)tag[0], (uint8_t)tag[1],
                       (uint8_t)tag[2], (uint8_t)tag[3] };
    uint32_t crc = (uint32_t)crc32(0, &hdr[4], 4);
    if (len) {
        crc = (uint32_t)crc32(crc, (const Bytef *)data, len);
    }
    uint8_t tail[4] = { (uint8_t)(crc >> 24), (uint8_t)(crc >> 16),
                        (uint8_t)(crc >> 8), (uint8_t)crc };
    fwrite(hdr, 1, 8, f);
    if (len) {
        fwrite(data, 1, len, f);
    }
    fwrite(tail, 1, 4, f);
}

void dx11_screenshot(void)
{
    if (!fb.up || !fb.decoded) {
        return;
    }

    D3D11_TEXTURE2D_DESC sd = {};
    fb.decoded->GetDesc(&sd);
    D3D11_TEXTURE2D_DESC staging = sd;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *tex = nullptr;
    if (FAILED(dx11.device->CreateTexture2D(&staging, nullptr, &tex))) {
        return;
    }
    dx11.context->CopyResource(tex, fb.decoded);

    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(dx11.context->Map(tex, 0, D3D11_MAP_READ, 0, &map))) {
        tex->Release();
        return;
    }

    static int shot;
    char name[64];
    snprintf(name, sizeof(name), "dx11-screenshot-%03d.png", ++shot);
    FILE *f = fopen(name, "wb");
    if (f) {
        uint8_t ihdr[13] = {
            (uint8_t)(sd.Width >> 24), (uint8_t)(sd.Width >> 16),
            (uint8_t)(sd.Width >> 8), (uint8_t)sd.Width,
            (uint8_t)(sd.Height >> 24), (uint8_t)(sd.Height >> 16),
            (uint8_t)(sd.Height >> 8), (uint8_t)sd.Height,
            8, 2, 0, 0, 0,             /* 8-bit truecolour RGB */
        };
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        png_write_chunk(f, "IHDR", ihdr, 13);

        /* filter byte 0 (none) + RGB rows, deflated into one IDAT */
        uint32_t row = sd.Width * 3 + 1;
        uint8_t *raw_rows = (uint8_t *)malloc((size_t)row * sd.Height);
        for (uint32_t y = 0; y < sd.Height; y++) {
            const uint8_t *src =
                (const uint8_t *)map.pData + (size_t)y * map.RowPitch;
            uint8_t *dst = raw_rows + (size_t)y * row;
            dst[0] = 0;
            for (uint32_t x = 0; x < sd.Width; x++) {
                dst[1 + x * 3 + 0] = src[x * 4 + 0];
                dst[1 + x * 3 + 1] = src[x * 4 + 1];
                dst[1 + x * 3 + 2] = src[x * 4 + 2];
            }
        }
        uLongf zlen = compressBound((uLong)row * sd.Height);
        uint8_t *zbuf = (uint8_t *)malloc(zlen);
        if (zbuf && compress2(zbuf, &zlen, raw_rows,
                              (uLong)row * sd.Height, 6) == Z_OK) {
            png_write_chunk(f, "IDAT", zbuf, (uint32_t)zlen);
        }
        free(zbuf);
        free(raw_rows);
        png_write_chunk(f, "IEND", nullptr, 0);
        fclose(f);
    }

    dx11.context->Unmap(tex, 0);
    tex->Release();
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
    /* The low-level keyboard hook must be installed from the thread that
     * pumps messages, which is this one; it only acts while grabbed. */
    dx11_glue_kbd_hook_window(dx11.hwnd);

    /* A frame: pull the guest's screen, upload, decode, scale, present
     * at vsync.  Nothing here holds a QEMU lock. */
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
            if (!dx11_render_frame()) {
                dx11.context->ClearRenderTargetView(dx11.rtv, clear);
            }
            dx11.swap->Present(1 /* vsync */, 0);
        } else {
            Sleep(1);
        }
    }

    dx11_backend_request_shutdown();
    dx11_glue_fb_done();

    if (fb.cbuf) fb.cbuf->Release();
    if (fb.linear) fb.linear->Release();
    if (fb.vs) fb.vs->Release();
    if (fb.scale_ps) fb.scale_ps->Release();
    for (size_t i = 0; i < sizeof(fb_ps_all) / sizeof(fb_ps_all[0]); i++) {
        if (fb_ps_all[i]) fb_ps_all[i]->Release();
    }
    fb_release_pipeline();
    if (dx11.rtv) dx11.rtv->Release();
    if (dx11.swap) dx11.swap->Release();
    if (dx11.context) dx11.context->Release();
    if (dx11.device) dx11.device->Release();
    dx11.ready = false;

    /* Returning from here means returning from qemu_main's stand-in, which
     * unwinds main() while the main-loop thread is inside qemu_cleanup() --
     * the process dies under it. The main loop calls exit(); outlive it. */
    for (;;) {
        Sleep(INFINITE);
    }
    return 0;                           /* not reached */
}
