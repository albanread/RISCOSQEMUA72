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
#include <dxgi1_3.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <shellapi.h>
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
    /* The flip chain is waitable: the frame-latency slot is waited for
     * with MsgWaitForMultipleObjectsEx so the message pump keeps running
     * while Present would otherwise block on DWM.  A plain Present(1)
     * that stalls five seconds gets the window ghosted by Windows, DWM
     * stops flipping it and the block becomes permanent.  Blt-model
     * fallback chains have no waitable object (swap_wait is null). */
    HANDLE swap_wait;
    UINT swap_flags;
    bool ready;
    bool lost;
    /* mouse messages forwarded, for the debug log */
    unsigned mouse_moves;
    unsigned mouse_buttons;
} dx11;

/* -display dx11 options, set once at init: how the guest screen is
 * magnified to the client area (sharp-bilinear by default) and whether
 * an optional CRT scanline mask rides on top at 2x+ magnification. */
enum Dx11Scaling { DX11_SCALING_LINEAR = 0, DX11_SCALING_SHARP, DX11_SCALING_NEAREST };
static struct {
    int scaling;                    /* Dx11Scaling */
    bool scanlines;
} video_opts = { DX11_SCALING_SHARP, false };

extern "C" void dx11_glue_video_opts(int scaling, int scanlines)
{
    if (scaling >= DX11_SCALING_LINEAR && scaling <= DX11_SCALING_NEAREST) {
        video_opts.scaling = scaling;
    }
    video_opts.scanlines = scanlines != 0;
}

/* The per-mode pipeline: everything that depends on the fb config. */
static struct {
    uint32_t generation;
    uint32_t bpp;
    uint32_t xres, yres, pitch, rows;

    /*
     * Two copies of the guest bytes, not one.  A copy the guest wrote
     * into while we were reading it splices two guest states together,
     * and during a window move that puts part of the window where it
     * used to be.  So the fresh copy goes into the buffer we are not
     * showing, and only becomes the shown one if it came out whole;
     * otherwise the previous good frame stays up and we try again.
     */
    ID3D11Buffer *raw[2];           /* guest bytes, pitch * rows, flat */
    ID3D11ShaderResourceView *raw_srv[2];
    unsigned raw_cur;               /* the one being displayed */
    bool have_good;                 /* a whole frame has been captured */
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
    ID3D11SamplerState *wrap;      /* backdrop tiles repeat */
    bool up;
} fb;

/* The guest's pointer sprite: a pipeline and a texture that do not
 * depend on the guest's mode, built once beside the scale pass.  The
 * VCHIQ peer answers the dispmanx requests the ROM sends for its
 * hardware pointer, so the sprite is composited here on top of the
 * scaled frame -- never written to guest RAM, sharp at any window
 * scale, and without the scanline mask: on a real Acorn machine the
 * sprite lay over the CRT, it was not part of its raster. */
#define DX11_CURSOR_TEXELS 64     /* words per side, as the peer defines */
static struct {
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Texture2D *image;        /* DX11_CURSOR_TEXELS^2, BGRA8 */
    ID3D11ShaderResourceView *image_srv;
    ID3D11Buffer *rect_cbuf;       /* float4: the NDC dest rect, VS b1 */
    ID3D11Buffer *dim_cbuf;        /* uint2: sprite size, PS b1 */
    ID3D11BlendState *blend;       /* straight source alpha */
    bool up;
    bool visible;
    uint32_t generation;
    int32_t x, y, w, h;            /* dest rect, display pixels */
    int32_t img_w, img_h;          /* the sprite's own resolution */
    int32_t disp_w, disp_h;        /* the display the rect is measured in */
} ptr;

static const wchar_t DX11_CLASS[] = L"qemu-dx11";
static const wchar_t DX11_TITLE[] = L"RISC OS 5 — Raspberry Pi 4 (QEMU)";

/* System-menu id for the snapshot entry (SC_* ids live at 0xF000+). */
#define DX11_SC_LOADSNAP 0x0100

/* ... and for the HostNet switch, and the note under it. */
#define DX11_SC_HOSTNET      0x0110
#define DX11_SC_HOSTNET_NOTE 0x0120

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

void dx11_screenshot(void);     /* PrintScreen: ask for the composited frame */
static void comp_capture(void);  /* ... which the frame loop answers */
static void comp_init(void);

/* The Backdrop submenu, defined with the rest of the backdrop below;
 * the window procedure above it opens and answers the menu. */
static void backdrop_build_menu(void);
static bool backdrop_command(UINT id);
static void backdrop_attach_menu(void);
static HMENU backdrop_menu_handle(void);

/* The HostNet switch in the window menu, defined after the Backdrop menu. */
static void hostnet_build_menu(HMENU sm);
static void hostnet_command(void);

/* ------------------------------------------------------------------ */
/* Grab: the guest owns the keyboard and the pointer until Ctrl+Alt+G  */

static struct {
    bool on;
} kbd;

static void dx11_mouse_centre(HWND h);
static void dx11_mouse_send_abs(int gx, int gy);
/* mouse state, see the Mouse section */
static struct {
    bool have_last;
    int last_x, last_y;
    /* the guest pointer position we last sent, in guest pixels: the
     * anchor for relative motion while grabbed (absolute while not) */
    int gx, gy;
    bool have_guest;
    int gxres, gyres;            /* guest screen size, from the fb view */
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
        if (!mouse.have_guest && mouse.gxres > 1) {
            mouse.gx = mouse.gxres / 2;
            mouse.gy = mouse.gyres / 2;
            mouse.have_guest = true;
        }
        dx11_mouse_centre(dx11.hwnd);
    } else {
        ClipCursor(nullptr);
        ShowCursor(TRUE);
        mouse.have_last = false;
        /* Re-align the guest arrow with the host cursor, which is where
         * the grab left it (the centre), not where motion ended. */
        if (mouse.have_guest) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(dx11.hwnd, &p);
            RECT c;
            GetClientRect(dx11.hwnd, &c);
            if (c.right > 0 && mouse.gxres > 1) {
                mouse.gx = p.x * (mouse.gxres - 1) / (c.right - 1);
                mouse.gy = p.y * (mouse.gyres - 1) / (c.bottom - 1);
                dx11_mouse_send_abs(mouse.gx, mouse.gy);
            }
        }
    }
    dx11_log("grab %s", on ? "on" : "off");
}

/* ------------------------------------------------------------------ */
/* Mouse: ordinary window messages.  The guest device is an absolute   */
/* tablet, so the pointer position is always sent as a coordinate:     */
/* ungrabbed that is simply the host cursor's place in the client area */
/* scaled to the guest screen (the two arrows never diverge); grabbed */
/* the host cursor is warped back to the centre after every move so    */
/* motion is unbounded, and the deltas accumulate into a virtual       */
/* position that is sent absolutely -- it cannot drift.                */

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

static void dx11_mouse_send_abs(int gx, int gy)
{
    if (mouse.gxres > 1 && mouse.gyres > 1) {
        if (gx < 0) gx = 0;
        if (gy < 0) gy = 0;
        if (gx > mouse.gxres - 1) gx = mouse.gxres - 1;
        if (gy > mouse.gyres - 1) gy = mouse.gyres - 1;
        dx11_glue_mouse_abs(gx, gy, mouse.gxres, mouse.gyres);
        mouse.gx = gx;
        mouse.gy = gy;
        mouse.have_guest = true;
        dx11.mouse_moves++;
    }
}

static void dx11_mouse_move(HWND h, int x, int y)
{
    if (mouse.gxres < 2 || mouse.gyres < 2) {
        return;                     /* no guest screen to map onto yet */
    }
    RECT c;
    GetClientRect(h, &c);
    if (c.right < 1 || c.bottom < 1) {
        return;
    }
    if (!kbd.on) {
        /* Ungrabbed: host cursor and guest arrow coincide. */
        dx11_mouse_send_abs(x * (mouse.gxres - 1) / (c.right - 1),
                            y * (mouse.gyres - 1) / (c.bottom - 1));
        mouse.have_last = false;
        return;
    }
    /* Grabbed: delta from the last position, accumulated into the
     * virtual guest position — scaled by guest-per-host pixels, so a
     * window larger than the guest screen does not run the pointer
     * into the edges at double speed.  The warp-to-centre after each
     * move bounds the host cursor without ending the motion. */
    if (mouse.have_last) {
        int dx = x - mouse.last_x, dy = y - mouse.last_y;
        if (dx || dy) {
            int cw = c.right - c.left;
            int ch = c.bottom - c.top;
            int gdx = (int)((long long)dx * mouse.gxres / (cw > 0 ? cw : 1));
            int gdy = (int)((long long)dy * mouse.gyres / (ch > 0 ? ch : 1));
            dx11_mouse_send_abs(mouse.gx + gdx, mouse.gy + gdy);
        }
    }
    mouse.last_x = x;
    mouse.last_y = y;
    mouse.have_last = true;
    if (x != (c.right - c.left) / 2 || y != (c.bottom - c.top) / 2) {
        dx11_mouse_centre(h);       /* its own WM_MOUSEMOVE is a zero delta */
    }
}

/* ------------------------------------------------------------------ */
/* Fullscreen: Alt+Enter swaps the frame for a borderless window on the
 * monitor the window is on; the same chord swaps it back.               */

static void dx11_toggle_fullscreen(void)
{
    static WINDOWPLACEMENT wp = { sizeof(wp) };
    DWORD style = GetWindowLongW(dx11.hwnd, GWL_STYLE);

    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetWindowPlacement(dx11.hwnd, &wp)
            && GetMonitorInfoW(MonitorFromWindow(dx11.hwnd,
                                                 MONITOR_DEFAULTTONEAREST), &mi)) {
            SetWindowLongW(dx11.hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(dx11.hwnd, HWND_TOP,
                         mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            dx11_log("fullscreen on");
        }
    } else {
        SetWindowLongW(dx11.hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(dx11.hwnd, &wp);
        SetWindowPos(dx11.hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                     | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        dx11_log("fullscreen off");
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
                                     DXGI_FORMAT_B8G8R8A8_UNORM,
                                     dx11.swap_flags);
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
        /* Alt+Enter is the fullscreen toggle, and never reaches the guest */
        if (msg == WM_SYSKEYDOWN && w == VK_RETURN
            && (GetKeyState(VK_MENU) & 0x8000)) {
            dx11_toggle_fullscreen();
            return 0;
        }
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
        /*
         * All three buttons belong to the guest.  RISC OS is built on
         * them -- Select, Menu, Adjust -- and Menu is the middle one,
         * the button the desktop uses most: a front end that keeps it
         * for itself leaves the guest unable to open a menu at all.
         * The grab is Ctrl+Alt+G, and since the tablet keeps the host
         * and guest pointers together there is nothing to grab for in
         * ordinary use anyway.
         */
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
    case WM_INITMENUPOPUP:
        /* The Backdrop submenu is read from the folder each time it opens,
         * so an image dropped in there is listed without a restart. */
        if ((HMENU)w == backdrop_menu_handle()) {
            backdrop_build_menu();
            return 0;
        }
        /* The window menu itself: HostNet's item follows its module file,
         * which Explorer or RISC OS can move as easily as the menu can.
         * The system still gets the message, to grey Move, Size and the
         * rest as it always does. */
        if (HIWORD(l) && (HMENU)w == GetSystemMenu(h, FALSE)) {
            hostnet_build_menu((HMENU)w);
        }
        return DefWindowProcW(h, msg, w, l);
    case WM_SYSCOMMAND:
        if ((w & 0xfff0) == DX11_SC_LOADSNAP) {
            dx11_glue_load_snapshot();
            return 0;
        }
        if ((w & 0xfff0) == DX11_SC_HOSTNET) {
            hostnet_command();
            return 0;
        }
        if (backdrop_command((UINT)(w & 0xfff0))) {
            return 0;
        }
        return DefWindowProcW(h, msg, w, l);
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

    /* Default client size: the EDID mode the firmware answers with,
     * scaled by the system DPI — the process is DPI-aware, so at 150%
     * an 800x600 client is physically small.  The scaler stretches any
     * size; this is just a readable default. */
    UINT dpi = 96;
    {
        typedef UINT (WINAPI *GDA)(void);
        HMODULE u = GetModuleHandleW(L"user32.dll");
        GDA gda = u ? (GDA)GetProcAddress(u, "GetDpiForSystem") : nullptr;
        if (gda) {
            dpi = gda();
        }
    }
    int scale = dpi / 96;
    if (scale < 1) {
        scale = 1;
    }
    RECT r = { 0, 0, 800 * scale, 600 * scale };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    dx11.hwnd = CreateWindowExW(
        0, DX11_CLASS, DX11_TITLE, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!dx11.hwnd) {
        return false;
    }
    /* The window menu carries the dev-loop convenience: a click on
     * "Load snapshot" rewinds the machine to the saved desktop. */
    HMENU sm = GetSystemMenu(dx11.hwnd, FALSE);
    if (sm) {
        AppendMenuW(sm, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sm, MF_STRING, DX11_SC_LOADSNAP, L"Load snapshot\tDesktop");
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
    scd.Flags             = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    dx11.swap_flags       = scd.Flags;

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
    if (SUCCEEDED(hr)) {
        /* Make the chain waitable: one latency slot, and the handle the
         * frame loop blocks on instead of blocking inside Present. */
        IDXGISwapChain2 *swap2 = nullptr;
        if (FAILED(dx11.swap->QueryInterface(IID_PPV_ARGS(&swap2)))) {
            dx11_log("swap chain: no IDXGISwapChain2 (hr=%08lx), "
                     "falling back to blt model", (unsigned long)hr);
            dx11.swap->Release();
            dx11.swap = nullptr;
        } else {
            HRESULT hrl = swap2->SetMaximumFrameLatency(1);
            dx11.swap_wait = swap2->GetFrameLatencyWaitableObject();
            if (FAILED(hrl) || dx11.swap_wait == nullptr) {
                dx11_log("swap chain: waitable setup failed, "
                         "falling back to blt model");
                if (dx11.swap_wait) {
                    CloseHandle(dx11.swap_wait);
                    dx11.swap_wait = nullptr;
                }
                swap2->Release();
                dx11.swap->Release();
                dx11.swap = nullptr;
            } else {
                swap2->Release();
            }
        }
    }
    if (!dx11.swap) {
        /* Older drivers: the blt model still clears a window, and its
         * Present copies rather than flips so it cannot wedge on DWM;
         * it just has no waitable object to pace on. */
        scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        scd.Flags      = 0;
        dx11.swap_flags = 0;
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
/* ------------------------------------------------------------------ */
/* The backdrop layer                                                   */

/*
 * -display dx11,backdrop=: a layer the host draws beneath the guest's
 * desktop (ROS_PRIVATE docs/hostnet.md's sibling, MACOS.md "The backdrop
 * layer").  The guest opts pixels in through the transfer byte of a 32bpp
 * pixel -- RISC OS's supremacy byte, which everything the OS draws leaves
 * at zero:
 *
 *   bits 7-6  the layer tag: 10 below (the layer beneath shows through),
 *             01 above (reserved: a layer drawn over the pixel), 00 and
 *             11 nothing, so the &FF some sprite tools write means nothing
 *   bits 5-0  reserved, zero
 *
 * A below pixel must also be the backdrop key colour, the Acorn theme's
 * sage, so an EOR drag box drawn over the backdrop changes the colour,
 * drops out of the key and stays visible.  The guest paints the tag by
 * tiling a sprite whose pixels carry it (riscos-pi4/tools/mkbacktile.py);
 * no RISC OS code changes.
 *
 * This is the Windows half of the Metal front end's feature, drawing the
 * same scene from the same numbers.
 */
enum Dx11Backdrop { DX11_BACKDROP_OFF = 0, DX11_BACKDROP_ACORN,
                    DX11_BACKDROP_ACORN_LIVE, DX11_BACKDROP_PICTURE,
                    DX11_BACKDROP_TILE };
#define DX11_BACKDROP_KEY   0x00B4C0B7u  /* 0x00BBGGRR: #B7C0B4 */
#define DX11_ICONBAR_PX     66u          /* guest px, square-pixel modes */

/* The shader constants, in the shape ui/dx11.cpp's HLSL `Params` reads. */
struct Dx11Params {
    uint32_t dim[4];
    uint32_t misc[4];
    uint32_t post[4];
    uint32_t bd[4];     /* backdrop: key 0x00BBGGRR, time ms, icon bar, mode */
    uint32_t bx[4];     /* backdrop: output px per tile px x1000, unused */
};

static struct {
    bool enabled;                   /* the gate: backdrop= other than off */
    int mode;                       /* which scene, Dx11Backdrop */
    ID3D11Texture2D *image;         /* tile or picture, if any */
    ID3D11ShaderResourceView *image_srv;
    double image_scale;             /* 2 for an @2x tile, else 1 */
    ULONGLONG t0;                   /* the live scene's clock origin, ms */
    char spec[512];
} backdrop = { false, DX11_BACKDROP_OFF, nullptr, nullptr, 1.0, 0, "off" };

/*
 * The backdrop's share of the shader constants: the key, a clock on the
 * scene's half-hour loop, the icon bar the acorn is centred above, and
 * which scene.  The loop keeps the float time small enough that the live
 * scene's sines do not lose precision after a long session.
 */
static void backdrop_params(Dx11Params *p)
{
    p->bd[0] = DX11_BACKDROP_KEY;
    p->bd[1] = (uint32_t)((GetTickCount64() - backdrop.t0) % 1800000ULL);
    p->bd[2] = DX11_ICONBAR_PX;
    p->bd[3] = (uint32_t)backdrop.mode;
    /* An output pixel per image pixel, or half of one for an @2x tile. */
    p->bx[0] = (uint32_t)(1000.0 / backdrop.image_scale + 0.5);
}

/*
 * An image for backdrop=<file>: anything WIC reads, converted to straight
 * BGRA8 and drawn over the key colour, so a transparent PNG sits on sage
 * rather than on black.  Capped at 4096 a side, as the Metal side caps it.
 * Returns false and leaves the texture null if the file cannot be read.
 */
#define DX11_BACKDROP_MAXPX 4096

static bool backdrop_load_image(const char *path)
{
    IWICImagingFactory *fac = nullptr;
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICFormatConverter *conv = nullptr;
    wchar_t wpath[MAX_PATH];
    UINT w = 0, h = 0;
    uint8_t *pix = nullptr;
    bool ok = false;
    HRESULT hr;

    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH) == 0) {
        return false;
    }
    /* The UI thread may or may not have COM up; either answer is fine, and
     * a second initialise is balanced by the uninitialise below. */
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&fac));
    if (SUCCEEDED(hr)) {
        hr = fac->CreateDecoderFromFilename(wpath, nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand,
                                            &dec);
    }
    if (SUCCEEDED(hr)) {
        hr = dec->GetFrame(0, &frame);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->GetSize(&w, &h);
    }
    if (SUCCEEDED(hr) && (w == 0 || h == 0 ||
                          w > DX11_BACKDROP_MAXPX || h > DX11_BACKDROP_MAXPX)) {
        dx11_log("backdrop: %s is %ux%u, past the %u limit", path, w, h,
                 DX11_BACKDROP_MAXPX);
        hr = E_FAIL;
    }
    if (SUCCEEDED(hr)) {
        hr = fac->CreateFormatConverter(&conv);
    }
    if (SUCCEEDED(hr)) {
        /* Straight (not premultiplied) BGRA: the shader composites the
         * image as opaque colour, and the fill below removes any alpha. */
        hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                              WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr)) {
        pix = (uint8_t *)malloc((size_t)w * h * 4);
        hr = pix ? conv->CopyPixels(nullptr, w * 4, w * h * 4, pix) : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) {
        /* Over the key colour, so transparency lands on sage. */
        for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
            uint8_t *q = pix + i * 4;
            unsigned a = q[3];
            q[0] = (uint8_t)((q[0] * a + 0xB4 * (255 - a)) / 255);
            q[1] = (uint8_t)((q[1] * a + 0xC0 * (255 - a)) / 255);
            q[2] = (uint8_t)((q[2] * a + 0xB7 * (255 - a)) / 255);
            q[3] = 0xff;
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = { pix, w * 4, 0 };
        hr = dx11.device->CreateTexture2D(&td, &sd, &backdrop.image);
        if (SUCCEEDED(hr)) {
            hr = dx11.device->CreateShaderResourceView(backdrop.image, nullptr,
                                                       &backdrop.image_srv);
        }
        if (SUCCEEDED(hr)) {
            dx11_log("backdrop: image %s (%ux%u)", path, w, h);
            ok = true;
        }
    }
    free(pix);
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (fac) fac->Release();
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
    if (!ok) {
        if (backdrop.image_srv) { backdrop.image_srv->Release(); }
        if (backdrop.image) { backdrop.image->Release(); }
        backdrop.image_srv = nullptr;
        backdrop.image = nullptr;
    }
    return ok;
}

/*
 * backdrop=off | none | acorn | acorn-live | tile:<file> | picture:<file>
 * | <file> (a bare path is a picture).  A tile whose name carries @2x
 * covers half an output pixel per image pixel.  An unreadable image falls
 * back to the acorn rather than to nothing, so the window is never a
 * mystery black.
 */
static void backdrop_set_scene(const char *spec)
{
    int mode = DX11_BACKDROP_OFF;
    const char *path = nullptr;

    if (backdrop.image_srv) { backdrop.image_srv->Release(); }
    if (backdrop.image) { backdrop.image->Release(); }
    backdrop.image_srv = nullptr;
    backdrop.image = nullptr;
    backdrop.image_scale = 1.0;

    if (!spec || !*spec || !strcmp(spec, "none") || !strcmp(spec, "off")) {
        mode = DX11_BACKDROP_OFF;
        spec = "none";
    } else if (!strcmp(spec, "acorn")) {
        mode = DX11_BACKDROP_ACORN;
    } else if (!strcmp(spec, "acorn-live")) {
        mode = DX11_BACKDROP_ACORN_LIVE;
    } else if (!strncmp(spec, "tile:", 5)) {
        mode = DX11_BACKDROP_TILE;
        path = spec + 5;
    } else if (!strncmp(spec, "picture:", 8)) {
        mode = DX11_BACKDROP_PICTURE;
        path = spec + 8;
    } else {
        mode = DX11_BACKDROP_PICTURE;
        path = spec;
    }
    if (path) {
        if (backdrop_load_image(path)) {
            if (mode == DX11_BACKDROP_TILE && strstr(path, "@2x")) {
                backdrop.image_scale = 2.0;
            }
        } else {
            mode = DX11_BACKDROP_ACORN;
            spec = "acorn";
            dx11_log("backdrop: cannot read %s; using acorn", path);
        }
    }
    backdrop.mode = mode;
    snprintf(backdrop.spec, sizeof(backdrop.spec), "%s", spec);
}

/*
 * The gate, from -display dx11,backdrop=.  "off" leaves the feature out
 * entirely: the decode ignores the transfer byte, nothing is drawn beneath
 * the desktop, and the Backdrop menu is not built.
 */
extern "C" void dx11_glue_backdrop(const char *spec)
{
    backdrop.t0 = GetTickCount64();
    if (!spec || !*spec || !strcmp(spec, "off")) {
        backdrop.enabled = false;
        backdrop.mode = DX11_BACKDROP_OFF;
        snprintf(backdrop.spec, sizeof(backdrop.spec), "off");
        return;
    }
    backdrop.enabled = true;
    backdrop_set_scene(spec);
    backdrop_attach_menu();
    dx11_log("backdrop: %s", backdrop.spec);
}


/* ------------------------------------------------------------------ */
/* The Backdrop menu                                                    */

/*
 * The window menu's Backdrop submenu, the Metal front end's twin (its
 * menuNeedsUpdate): the Acorn scenes, None, and Tiles and Pictures
 * listing the images in the backdrops folder -- rebuilt each time it
 * opens, so a file dropped into the folder is there next time.  A choice
 * applies at once.
 *
 * Items in a window menu report through WM_SYSCOMMAND, whose low four
 * bits the system reserves, so every identifier here is a multiple of 16.
 */
#define DX11_SC_BD_ACORN  0x0200
#define DX11_SC_BD_LIVE   0x0210
#define DX11_SC_BD_NONE   0x0220
#define DX11_SC_BD_OPEN   0x0230
#define DX11_SC_BD_IMG0   0x0300        /* one per listed image, step 16 */
#define DX11_SC_BD_IMGS   200           /* as many as the Metal menu lists */

static HMENU backdrop_menu;                       /* the submenu, if built */

static HMENU backdrop_menu_handle(void)
{
    return backdrop_menu;
}

/*
 * Hang the submenu on the window menu.  Called from dx11_glue_backdrop,
 * because the window is created before the display options are read and
 * this is the first moment we know the feature is wanted at all.
 */
static void backdrop_attach_menu(void)
{
    HMENU sm = dx11.hwnd ? GetSystemMenu(dx11.hwnd, FALSE) : nullptr;

    if (sm && !backdrop_menu) {
        backdrop_menu = CreatePopupMenu();
        AppendMenuW(sm, MF_POPUP, (UINT_PTR)backdrop_menu, L"Backdrop");
    }
}

static char *backdrop_menu_spec[DX11_SC_BD_IMGS]; /* what each item selects */

/* The folder the menu lists, Tiles and Pictures inside it. */
static void backdrop_folder(wchar_t *out, size_t n)
{
    wchar_t home[MAX_PATH];

    if (GetEnvironmentVariableW(L"QEMU_BACKDROPS", out, (DWORD)n)) {
        return;
    }
    if (!GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH)) {
        home[0] = 0;
    }
    _snwprintf(out, n, L"%ls\\Pictures\\RISC OS Backdrops", home);
}

/* Explorer's own order: digits as numbers, case ignored. */
static bool backdrop_name_before(const wchar_t *a, const wchar_t *b)
{
    int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT,
                            SORT_DIGITSASNUMBERS | NORM_IGNORECASE,
                            a, -1, b, -1, nullptr, nullptr, 0);

    return r == CSTR_LESS_THAN;
}

static bool backdrop_is_image(const wchar_t *name)
{
    static const wchar_t *ext[] = { L".png", L".jpg", L".jpeg", L".tif",
                                    L".tiff", L".gif", L".bmp", L".heic",
                                    L".heif", L".webp" };
    const wchar_t *dot = wcsrchr(name, L'.');

    if (!dot) {
        return false;
    }
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) {
        if (!_wcsicmp(dot, ext[i])) {
            return true;
        }
    }
    return false;
}

/*
 * Fill `menu` with the images in `dir`, each selecting "<kind>:<path>".
 * `next` is the identifier to hand out, moved on as they are used, so the
 * two lists share one run of identifiers.
 */
static void backdrop_list(HMENU menu, const wchar_t *dir, const char *kind,
                          const wchar_t *title, UINT *next)
{
    wchar_t pattern[MAX_PATH];
    wchar_t (*names)[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;

    names = (wchar_t (*)[MAX_PATH])calloc(DX11_SC_BD_IMGS, sizeof(*names));
    if (!names) {
        return;
    }
    _snwprintf(pattern, MAX_PATH, L"%ls\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                && fd.cFileName[0] != L'.'
                && backdrop_is_image(fd.cFileName)
                && n < DX11_SC_BD_IMGS) {
                wcsncpy(names[n], fd.cFileName, MAX_PATH - 1);
                n++;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    /* few enough that an insertion sort is the whole story */
    for (int i = 1; i < n; i++) {
        wchar_t tmp[MAX_PATH];
        int j = i;

        wcsncpy(tmp, names[i], MAX_PATH);
        while (j > 0 && backdrop_name_before(tmp, names[j - 1])) {
            wcsncpy(names[j], names[j - 1], MAX_PATH);
            j--;
        }
        wcsncpy(names[j], tmp, MAX_PATH);
    }

    for (int i = 0; i < n && *next < DX11_SC_BD_IMG0 + DX11_SC_BD_IMGS * 16;
         i++) {
        wchar_t path[MAX_PATH], label[MAX_PATH];
        char spec[MAX_PATH * 2];
        size_t used;
        int slot = (int)((*next - DX11_SC_BD_IMG0) / 16);
        wchar_t *dot;

        _snwprintf(path, MAX_PATH, L"%ls\\%ls", dir, names[i]);
        _snprintf(spec, sizeof(spec), "%s:", kind);
        used = strlen(spec);
        if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, spec + used,
                                 (int)(sizeof(spec) - used), nullptr, nullptr)) {
            continue;                   /* a path we could not carry */
        }
        free(backdrop_menu_spec[slot]);
        backdrop_menu_spec[slot] = _strdup(spec);

        wcsncpy(label, names[i], MAX_PATH - 1);
        label[MAX_PATH - 1] = 0;
        dot = wcsrchr(label, L'.');
        if (dot) {
            *dot = 0;                   /* the name, not the file name */
        }
        AppendMenuW(menu, MF_STRING, *next, label);
        if (backdrop_menu_spec[slot]
            && !strcmp(backdrop_menu_spec[slot], backdrop.spec)) {
            CheckMenuItem(menu, *next, MF_BYCOMMAND | MF_CHECKED);
        }
        *next += 16;
    }
    if (n == 0) {
        wchar_t note[160];

        _snwprintf(note, 160, L"No images in the %ls folder", title);
        AppendMenuW(menu, MF_STRING | MF_GRAYED | MF_DISABLED, 0, note);
    }
    free(names);
}

/*
 * Whether RISC OS is painting the below tag in the key colour right now,
 * for the menu's hint: 1 yes, 0 no, -1 not a 32bpp mode, -2 no frame yet.
 * Sampled on a coarse grid from the guest's own bytes, as the Metal front
 * end samples its newest uploaded frame.
 */
static int backdrop_guest_state(void)
{
    Dx11FbView v;

    if (!fb.up || !dx11_glue_fb_view(&v) || !v.fb) {
        return -2;
    }
    if (v.bpp != 32) {
        return -1;
    }
    for (uint32_t y = 0; y < v.yres && y + v.yoffset < v.rows; y += 7) {
        const uint8_t *row = (const uint8_t *)v.fb
                           + (size_t)(y + v.yoffset) * v.pitch;

        for (uint32_t x = 0; x < v.xres; x += 7) {
            const uint8_t *px = row + (size_t)(x + v.xoffset) * 4;
            uint32_t rgb = v.pixo ? (px[0] | px[1] << 8 | px[2] << 16)
                                  : (px[2] | px[1] << 8 | px[0] << 16);

            if ((px[3] & 0xC0) == 0x80 && rgb == DX11_BACKDROP_KEY) {
                return 1;
            }
        }
    }
    return 0;
}

/* One scene item, ticked when it is the scene in force. */
static void backdrop_scene_item(HMENU menu, const wchar_t *label,
                                const char *spec, UINT id)
{
    AppendMenuW(menu, MF_STRING, id, label);
    if (!strcmp(spec, backdrop.spec)) {
        CheckMenuItem(menu, id, MF_BYCOMMAND | MF_CHECKED);
    }
}

/* Rebuilt each time the submenu opens, so the folder is read afresh. */
static void backdrop_build_menu(void)
{
    wchar_t dir[MAX_PATH], sub[MAX_PATH];
    HMENU tiles, pictures;
    UINT next = DX11_SC_BD_IMG0;

    if (!backdrop_menu) {
        return;
    }
    while (DeleteMenu(backdrop_menu, 0, MF_BYPOSITION)) {
        /* emptied from the top */
    }
    switch (backdrop_guest_state()) {
    case 0:
        AppendMenuW(backdrop_menu, MF_STRING | MF_GRAYED | MF_DISABLED, 0,
                    L"RISC OS is not drawing a backdrop to show through");
        AppendMenuW(backdrop_menu, MF_SEPARATOR, 0, nullptr);
        break;
    case -1:
        AppendMenuW(backdrop_menu, MF_STRING | MF_GRAYED | MF_DISABLED, 0,
                    L"Backdrops need a 16 million colour screen mode");
        AppendMenuW(backdrop_menu, MF_SEPARATOR, 0, nullptr);
        break;
    default:
        break;
    }
    backdrop_scene_item(backdrop_menu, L"Acorn", "acorn", DX11_SC_BD_ACORN);
    backdrop_scene_item(backdrop_menu, L"Acorn, Moving", "acorn-live",
                        DX11_SC_BD_LIVE);
    backdrop_scene_item(backdrop_menu, L"None", "none", DX11_SC_BD_NONE);
    AppendMenuW(backdrop_menu, MF_SEPARATOR, 0, nullptr);

    backdrop_folder(dir, MAX_PATH);
    tiles = CreatePopupMenu();
    _snwprintf(sub, MAX_PATH, L"%ls\\Tiles", dir);
    backdrop_list(tiles, sub, "tile", L"Tiles", &next);
    AppendMenuW(backdrop_menu, MF_POPUP, (UINT_PTR)tiles, L"Tiles");

    pictures = CreatePopupMenu();
    _snwprintf(sub, MAX_PATH, L"%ls\\Pictures", dir);
    backdrop_list(pictures, sub, "picture", L"Pictures", &next);
    AppendMenuW(backdrop_menu, MF_POPUP, (UINT_PTR)pictures, L"Pictures");

    AppendMenuW(backdrop_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(backdrop_menu, MF_STRING, DX11_SC_BD_OPEN,
                L"Open Backdrops Folder");
}

/*
 * A choice from the menu: apply it at once.  Returns false for anything
 * that is not ours, so the window procedure passes it on.
 */
static bool backdrop_command(UINT id)
{
    if (!backdrop.enabled) {
        return false;
    }
    switch (id) {
    case DX11_SC_BD_ACORN:
        backdrop_set_scene("acorn");
        return true;
    case DX11_SC_BD_LIVE:
        backdrop_set_scene("acorn-live");
        return true;
    case DX11_SC_BD_NONE:
        backdrop_set_scene("none");
        return true;
    case DX11_SC_BD_OPEN: {
        wchar_t dir[MAX_PATH], sub[MAX_PATH];

        /* The folder is the invitation: make it, with the two folders the
         * menu lists, rather than opening something that is not there. */
        backdrop_folder(dir, MAX_PATH);
        CreateDirectoryW(dir, nullptr);
        _snwprintf(sub, MAX_PATH, L"%ls\\Tiles", dir);
        CreateDirectoryW(sub, nullptr);
        _snwprintf(sub, MAX_PATH, L"%ls\\Pictures", dir);
        CreateDirectoryW(sub, nullptr);
        ShellExecuteW(nullptr, L"open", dir, nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }
    default:
        break;
    }
    if (id >= DX11_SC_BD_IMG0 && id < DX11_SC_BD_IMG0 + DX11_SC_BD_IMGS * 16
        && (id - DX11_SC_BD_IMG0) % 16 == 0) {
        char *spec = backdrop_menu_spec[(id - DX11_SC_BD_IMG0) / 16];

        if (spec) {
            backdrop_set_scene(spec);
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* The HostNet switch                                                   */

/*
 * One item in the window menu, ticked while HostNet is on.  The switch is
 * where HostNet's module sits on the share -- Modules or Modules\Disabled
 * (ui/dx11.c has the detail) -- so the item is there only when the module
 * is in one of the two, and a machine with nothing to switch shows none.
 *
 * Rebuilt each time the window menu opens.  Under it, greyed, a note
 * whenever what the menu says is not what RISC OS is running -- which the
 * device knows, because HostNet rings it fifty times a second.
 */
static void hostnet_build_menu(HMENU sm)
{
    int state = dx11_glue_hostnet_state();
    const wchar_t *note = nullptr;

    DeleteMenu(sm, DX11_SC_HOSTNET, MF_BYCOMMAND);
    DeleteMenu(sm, DX11_SC_HOSTNET_NOTE, MF_BYCOMMAND);
    if (state == DX11_HOSTNET_NONE) {
        return;
    }

    bool on = state & DX11_HOSTNET_ON;
    bool lit = state & DX11_HOSTNET_LIT;
    bool running = state & DX11_HOSTNET_RUNNING;

    AppendMenuW(sm, MF_STRING | (on ? MF_CHECKED : MF_UNCHECKED),
                DX11_SC_HOSTNET, L"HostNet");
    if (on && !running) {
        /* Dark only if the file was moved by hand rather than from this
         * menu; the launcher lights it at the next start. */
        note = lit ? L"On when RISC OS next starts"
                   : L"On when you quit and start again";
    } else if (!on && running) {
        note = L"Off when RISC OS next starts";
    }
    if (note) {
        AppendMenuW(sm, MF_STRING | MF_GRAYED | MF_DISABLED,
                    DX11_SC_HOSTNET_NOTE, note);
    }
}

static void hostnet_command(void)
{
    int state = dx11_glue_hostnet_state();
    bool on;
    char why[1024];

    if (state == DX11_HOSTNET_NONE) {
        return;                     /* moved away since the menu opened */
    }
    on = !(state & DX11_HOSTNET_ON);
    if (dx11_glue_hostnet_switch(on, why, sizeof(why)) == 0) {
        dx11_log("hostnet: switched %s", on ? "on" : "off");
        return;
    }
    dx11_log("hostnet: %s", why);

    wchar_t wwhy[1024];

    if (!MultiByteToWideChar(CP_UTF8, 0, why, -1, wwhy, 1024)) {
        wcscpy(wwhy, L"The module could not be moved.");
    }
    MessageBoxW(dx11.hwnd, wwhy, L"HostNet", MB_ICONWARNING | MB_OK);
}

/* Shaders: one source, one decode variant per format                  */

static const char SHADER_SRC[] = R"xxx(

struct Params {
    uint4 dim;     /* xres, yres, pitch(bytes), bpp */
    uint4 misc;    /* xoffset(px), yoffset(rows), pixo, unused */
    uint4 post;    /* client w, client h, scaling, scanlines */
    uint4 bd;      /* backdrop: key 0x00BBGGRR, time ms, icon bar px, mode */
    uint4 bx;      /* backdrop: output px per tile px x1000, unused */
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

/* The pointer sprite's quad: one triangle covering the NDC rectangle
 * R = {x0, y0, x1, y1}, vertices overshooting to 2x the rectangle's
 * span so no pixel of the rectangle lands on the hypotenuse (a
 * corner-exact triangle loses its lower right half to the top-left
 * fill rule).  uv runs 0..1 across the rectangle; the overhang
 * samples past 1 and the fragment discards it. */
cbuffer rect : register(b1) { float4 R; }

VSOut vs_rect(uint id : SV_VertexID)
{
    VSOut o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(R.xy + t * (R.zw - R.xy), 0, 1);
    o.uv = t;
    return o;
}

#ifdef DECODE

/* The raw guest bytes as one flat store indexed by byte address --
 * the Metal read's twin.  A Texture2D tops out at 16384 texels wide,
 * which a 3840-wide 32 bpp mode's 15360-byte pitch sits just inside
 * of but anything wider overflows; a ByteAddressBuffer has no such
 * ceiling, and the pitch is arbitrary.  Loads are uint-granular, so
 * each bpp case extracts its bytes from aligned words. */
ByteAddressBuffer  raw : register(t0);
Texture2D<float4> pal : register(t1);

cbuffer params : register(b0) { Params P; }

uint fb_word(uint row, uint byte_off)
{
    return raw.Load(row * P.dim.z + (byte_off & ~3u));
}

uint fb_byte(uint row, uint byte_off)
{
    return (fb_word(row, byte_off) >> ((byte_off & 3u) * 8)) & 0xff;
}

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
    uint w = fb_word(by, bx * 4);          /* aligned by construction */
    r = w & 0xff;
    g = (w >> 8) & 0xff;
    b = (w >> 16) & 0xff;
    if (P.misc.z == 0) { uint t = r; r = b; b = t; }   /* BGR order */

    /* The transfer byte's layer tag, bits 7-6: 10 is below, and a below
     * pixel in the backdrop key colour is a hole the layer beneath shows
     * through -- transparent, premultiplied, so the scale pass's filter
     * blends the edge the way it blends everything else. */
    if (P.bd.w != 0 && ((w >> 24) & 0xC0) == 0x80
        && (r | (g << 8) | (b << 16)) == P.bd.x) {
        return float4(0, 0, 0, 0);
    }
#elif BPP == 24
    /* three bytes from a possibly unaligned offset; the per-byte
     * helper loads its own aligned word, and the compiler merges the
     * loads that land in the same word */
    uint o = bx * 3;
    r = fb_byte(by, o);
    g = fb_byte(by, o + 1);
    b = fb_byte(by, o + 2);
#elif BPP == 16
    uint w = fb_word(by, bx * 2);          /* two pixels per word */
    uint sh = (bx & 1u) * 16;              /* odd pixel is the high half */
    w >>= sh;
    r = ((w >> 11) & 31) * 255 / 31;
    g = ((w >>  5) & 63) * 255 / 63;
    b = ((w      ) & 31) * 255 / 31;
#else
    /* palettised: 8 bpp, or sub-byte 1/2/4 with LSB-first packing */
    uint bpp = P.dim.w;
    uint byte = fb_byte(by, bx * bpp / 8);
    uint idx = (byte >> ((bx * bpp) & 7)) & ((1u << bpp) - 1);
    float4 c = pal.Load(int3(idx, 0, 0));
    return float4(c.rgb, 1);
#endif
    return float4(r / 255.0, g / 255.0, b / 255.0, 1);
}

#else /* the scale pass: decoded surface over the whole client area */

Texture2D<float4> src : register(t0);
Texture2D<float4> bgimg : register(t1);      /* backdrop image, if any */
SamplerState lin : register(s0);
SamplerState rep : register(s1);             /* wrapping, for tiles */

cbuffer params : register(b0) { Params P; }

/* The Acorn mark (design/art/acorn.svg) as a signed distance, in the
 * SVG's own units about its centre, y down, positive inside.  The mark is
 * the union of the cap, a quadratic dome, and the nut, a cubic egg, grown
 * by the half-width of the outline stroke and clipped to the SVG's
 * 34-unit view box -- which is why the watermark the pinboard draws has a
 * flat top and a flat bottom.  Distances to the curved sides are
 * horizontal offsets corrected by the curve's slope.
 *
 * Ported from the Metal front end (ui/metal.m); the maths is the same
 * scene, so the two displays draw the same acorn. */
static float acorn_distance(float2 q)
{
    const float grow = 0.8;
    float ax = abs(q.x);

    /* Nothing of the mark reaches past 16 units across or 17 down, so a
     * pixel a unit clear of that box is plainly outside: say so, and skip
     * the curve solve below, which is most of the scene's cost. */
    float outside = max(ax - 16.0, abs(q.y) - 17.0);
    if (outside > 1.0) {
        return -outside;
    }

    /* cap: right side (0,-20) Q (16,-20) (15,-7): x = 32t - 17t^2, y = -20 + 13t^2 */
    float cy = clamp(q.y, -20.0, -7.0);
    float ct = sqrt((cy + 20.0) / 13.0);
    float cxb = 32.0 * ct - 17.0 * ct * ct;
    float ck = (32.0 - 34.0 * ct) / max(26.0 * ct, 1e-3);
    float cap = min((cxb - ax) / sqrt(1.0 + ck * ck), -7.0 - q.y);

    /* nut: right side (0,18) C (9,18) (15,7) (13,-7); y falls as t rises */
    float ny = clamp(q.y, -7.0, 18.0);
    float lo = 0.0, hi = 1.0;
    for (int i = 0; i < 18; i++) {
        float tt = 0.5 * (lo + hi), uu = 1.0 - tt;
        float by = uu * uu * uu * 18.0 + 3.0 * uu * uu * tt * 18.0
                 + 3.0 * uu * tt * tt * 7.0 - tt * tt * tt * 7.0;
        if (by > ny) { lo = tt; } else { hi = tt; }
    }
    float t = 0.5 * (lo + hi), u = 1.0 - t;
    float nxb = 3.0 * u * u * t * 9.0 + 3.0 * u * t * t * 15.0 + t * t * t * 13.0;
    float ndx = 3.0 * (u * u * 9.0 + 2.0 * u * t * 6.0 - t * t * 2.0);
    float ndy = 3.0 * (2.0 * u * t * -11.0 - t * t * 14.0);
    float nk = ndx / min(ndy, -1e-3);
    float nut = min(min((nxb - ax) / sqrt(1.0 + nk * nk), 18.0 - q.y), q.y + 7.0);

    return min(max(cap, nut) + grow, 17.0 - abs(q.y));
}

static float3 backdrop_scene(float2 px, float2 outPx, float2 srcPx)
{
    const float3 ground = float3(183.0, 192.0, 180.0) / 255.0;   /* #B7C0B4 */
    const float3 ghost  = float3(173.0, 183.0, 168.0) / 255.0;   /* #ADB7A8 */
    /* view px per guest px, per axis: a window the user has resized away
     * from the mode's shape stretches the desktop unevenly */
    float2 k = outPx / srcPx;
    /* 220 guest px for the SVG's 34 units, centred above the icon bar,
     * exactly where *Backdrop -Centre put the watermark sprite; the
     * centre follows the stretch, the mark keeps its shape */
    const float unitPx = 220.0 / 34.0;
    float2 centre = float2(srcPx.x * 0.5, (srcPx.y - float(P.bd.z)) * 0.5) * k;
    float d = acorn_distance((px - centre) / (unitPx * k.y));
    float cover = clamp(d * unitPx * k.y + 0.5, 0.0, 1.0);
    float3 c = lerp(ground, ghost, cover);

    if (P.bd.w == 2) {
        /* acorn-live: two soft lights drifting over the ground, one cool
         * and one warm, on a half-hour loop so the float time never loses
         * precision; and a faint vignette */
        float tau = 6.2831853 * float(P.bd.y) / 1800000.0;
        float2 uv = px / outPx;
        float aspect = outPx.x / outPx.y;
        float2 l1 = float2(0.5 + 0.34 * sin(7.0 * tau), 0.42 + 0.22 * sin(5.0 * tau + 1.3));
        float2 l2 = float2(0.5 + 0.34 * sin(11.0 * tau + 2.1), 0.58 + 0.22 * cos(9.0 * tau));
        float2 d1 = (uv - l1) * float2(aspect, 1.0);
        float2 d2 = (uv - l2) * float2(aspect, 1.0);
        c += 0.075 * exp(-dot(d1, d1) * 3.5) * float3(1.0, 1.0, 0.96);
        c += 0.05 * exp(-dot(d2, d2) * 4.5) * float3(0.95, 0.72, 0.42);
        float2 v = (uv - 0.5) * float2(aspect, 1.0);
        c *= 1.0 - 0.07 * dot(v, v);
        /* gradients this gentle band in 8 bits: dither them by less than
         * a level, with interleaved gradient noise */
        float n = frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
        c += (n - 0.5) / 255.0;
    }
    return c;
}

float4 ps_main(VSOut v) : SV_Target
{
    float2 outPx = float2(P.post.xy);         /* client size */
    float2 srcPx = float2(P.dim.xy);          /* decoded size */
    float2 st = v.pos.xy / outPx * srcPx;     /* source-pixel coords */
#if SCALING == 1
    /* Sharp bilinear: within each source texel the bilinear transition
     * is narrowed to a 1/ratio-wide band at the texel edge, so at 1:1
     * the image passes through untouched and at 2x+ it is crisp with
     * just enough filtering to avoid hard staircases. */
    float2 ratio = max(outPx / srcPx, 1.0);
    float2 halfw = 0.5 - 0.5 / ratio;         /* half the band width */
    float2 i = floor(st);
    float2 f = st - i - 0.5;                  /* -0.5..0.5 in-texel */
    st = i + 0.5 + clamp(f, -halfw, halfw);
#elif SCALING == 2
    st = floor(st) + 0.5;                     /* nearest */
#endif
    float4 c = src.Sample(lin, st / srcPx);
#if SCANLINES
    /* CRT flavour, only when magnified enough for a line to be two */
    if (P.post.w && outPx.y >= srcPx.y * 1.99) {
        if ((uint(v.pos.y) & 1)) {
            c.rgb *= 0.8;
        }
    }
#endif
    if (P.bd.w == 0) {
        return float4(c.rgb, 1);          /* no layer: the guest's pixels */
    }

    float3 bg;
    if (P.bd.w >= 3) {
        float iw, ih;
        bgimg.GetDimensions(iw, ih);
        float2 isz = float2(iw, ih);
        if (P.bd.w == 4) {
            /* tile: from the top left, each image pixel covering an
             * output pixel (half of one for an @2x tile) -- repeated by
             * the sampler, so the seams filter like the rest */
            float2 uv = v.pos.xy / (isz * float(P.bx.x) / 1000.0);
            bg = bgimg.Sample(rep, uv).rgb;
        } else {
            /* picture: fill the view, centred, cropping the overhang */
            float s = max(outPx.x / isz.x, outPx.y / isz.y);
            float2 uv = (v.pos.xy - 0.5 * outPx) / (s * isz) + 0.5;
            bg = bgimg.Sample(lin, uv).rgb;
        }
    } else {
        bg = backdrop_scene(v.pos.xy, outPx, srcPx);
    }
    /* The decoded texel is premultiplied, so this is "over": where the
     * guest tagged a hole its alpha is 0 and the layer shows whole, and
     * at a filtered edge it shows in proportion. */
    return float4(c.rgb + bg * (1.0 - c.a), 1);
}

#endif

#ifdef POINTER

/* The guest's pointer sprite.  The words are little-endian
 * 0xAARRGGBB, so in memory the bytes run B,G,R,A -- exactly what
 * DXGI_FORMAT_B8G8R8A8_UNORM describes, and the format does the
 * channel ordering for us.  So this is a float4 read returning the
 * colour already in 0..1 and already channel-correct: no swizzle
 * (that is the Metal twin's job, because it reads raw bytes) and no
 * divide.  Declaring the texture uint4 against a UNORM view is a
 * type mismatch the runtime answers with undefined data, which is
 * what once drew the sprite as garbage.  The triangle's overhang
 * past uv 1 is dropped here, and the blend is straight alpha so the
 * ROM's anti-fringe fill (transparent pixels carrying the
 * neighbouring colour at alpha 0) behaves exactly as it does
 * against the firmware's compositor. */
Texture2D<float4> img : register(t0);

cbuffer ptrdim : register(b1) { uint2 dim; }

float4 ps_pointer(VSOut v) : SV_Target
{
    if (v.uv.x > 1.0f || v.uv.y > 1.0f) {
        discard;
    }
    uint2 t = min((uint2)(v.uv * (float2)dim), (uint2)dim - 1u);
    return img.Load(int3(t, 0));
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

static ID3D11PixelShader *compile_ps_entry(const char *target,
                                           const char **defines,
                                           size_t npairs, const char *entry);
static ID3D11VertexShader *compile_vs_entry(const char *entry);

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


/* Entry-point variants for the pointer pass, which lives in the same
 * shader source behind POINTER. */
static ID3D11PixelShader *compile_ps_entry(const char *target,
                                           const char **defines,
                                           size_t npairs, const char *entry)
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
                            nullptr, entry, target, 0, 0, &code, &errs);
    if (FAILED(hr)) {
        dx11_log("%s compile failed: %#x", entry, (unsigned)hr);
        if (errs) {
            dx11_log("%s compile: %s", entry,
                     (const char *)errs->GetBufferPointer());
            errs->Release();
        }
        return nullptr;
    }
    ID3D11PixelShader *ps = nullptr;
    hr = dx11.device->CreatePixelShader(code->GetBufferPointer(),
                                        code->GetBufferSize(), nullptr, &ps);
    code->Release();
    if (FAILED(hr)) {
        dx11_log("CreatePixelShader %s failed: %#x", entry, (unsigned)hr);
    }
    return ps;
}

static ID3D11VertexShader *compile_vs_entry(const char *entry)
{
    ID3DBlob *code = nullptr, *errs = nullptr;
    HRESULT hr = D3DCompile(SHADER_SRC, strlen(SHADER_SRC), nullptr, nullptr,
                            nullptr, entry, "vs_4_0", 0, 0, &code, &errs);
    if (FAILED(hr)) {
        dx11_log("%s compile failed: %#x", entry, (unsigned)hr);
        if (errs) {
            dx11_log("%s compile: %s", entry,
                     (const char *)errs->GetBufferPointer());
            errs->Release();
        }
        return nullptr;
    }
    ID3D11VertexShader *vs = nullptr;
    hr = dx11.device->CreateVertexShader(code->GetBufferPointer(),
                                         code->GetBufferSize(), nullptr, &vs);
    code->Release();
    if (FAILED(hr)) {
        dx11_log("CreateVertexShader %s failed: %#x", entry, (unsigned)hr);
    }
    return vs;
}

/* The pointer pass: shaders, the sprite texture, its constant buffers
 * and the straight-alpha blend state.  None of it depends on the
 * guest's mode, so it is built once at start-up. */
static bool ptr_build(void)
{
    const char *pdef[] = { "POINTER", "1" };

    ptr.vs = compile_vs_entry("vs_rect");
    ptr.ps = compile_ps_entry("ps_4_0", pdef, 1, "ps_pointer");
    if (!ptr.vs || !ptr.ps) {
        return false;
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = DX11_CURSOR_TEXELS;
    td.Height = DX11_CURSOR_TEXELS;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;    /* the words are BGRA */
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = dx11.device->CreateTexture2D(&td, nullptr, &ptr.image);
    if (FAILED(hr)) {
        dx11_log("pointer texture failed: %#x", (unsigned)hr);
        return false;
    }
    hr = dx11.device->CreateShaderResourceView(ptr.image, nullptr,
                                               &ptr.image_srv);
    if (FAILED(hr)) {
        dx11_log("pointer SRV failed: %#x", (unsigned)hr);
        return false;
    }

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = 0;
    bd.ByteWidth = 16;                     /* float4 / uint2+pad */
    hr = dx11.device->CreateBuffer(&bd, nullptr, &ptr.rect_cbuf);
    if (FAILED(hr)) {
        dx11_log("pointer rect cbuf failed: %#x", (unsigned)hr);
        return false;
    }
    hr = dx11.device->CreateBuffer(&bd, nullptr, &ptr.dim_cbuf);
    if (FAILED(hr)) {
        dx11_log("pointer dim cbuf failed: %#x", (unsigned)hr);
        return false;
    }

    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = dx11.device->CreateBlendState(&bld, &ptr.blend);
    if (FAILED(hr)) {
        dx11_log("pointer blend state failed: %#x", (unsigned)hr);
        return false;
    }

    ptr.up = true;
    return true;
}

static void ptr_release(void)
{
    if (ptr.vs) ptr.vs->Release();
    if (ptr.ps) ptr.ps->Release();
    if (ptr.image_srv) ptr.image_srv->Release();
    if (ptr.image) ptr.image->Release();
    if (ptr.rect_cbuf) ptr.rect_cbuf->Release();
    if (ptr.dim_cbuf) ptr.dim_cbuf->Release();
    if (ptr.blend) ptr.blend->Release();
    memset(&ptr, 0, sizeof(ptr));
}
/* ------------------------------------------------------------------ */
/* The per-mode pipeline                                               */

static void fb_release_pipeline(void)
{
    for (unsigned i = 0; i < 2; i++) {
        if (fb.raw_srv[i]) fb.raw_srv[i]->Release();
        if (fb.raw[i]) fb.raw[i]->Release();
        fb.raw_srv[i] = nullptr;
        fb.raw[i] = nullptr;
    }
    if (fb.pal_srv) fb.pal_srv->Release();
    if (fb.palette) fb.palette->Release();
    if (fb.dec_srv) fb.dec_srv->Release();
    if (fb.dec_rtv) fb.dec_rtv->Release();
    if (fb.decoded) fb.decoded->Release();
    if (fb.ps) fb.ps->Release();
    fb.raw_cur = 0;
    fb.have_good = false;
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
    const char *sdef[] = { "SCALING", nullptr, "SCANLINES", nullptr };
    char scal[8], scan[8];
    size_t ok = 0;

    snprintf(scal, sizeof(scal), "%d", video_opts.scaling);
    snprintf(scan, sizeof(scan), "%d", video_opts.scanlines ? 1 : 0);
    sdef[1] = scal;
    sdef[3] = scan;

    fb.vs = compile_vs();
    fb.scale_ps = compile_ps("ps_4_0", sdef, 2);
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

    /* raw guest bytes: one flat buffer indexed by byte address, the
     * Metal read's twin.  A Texture2D topped out at 16384 texels wide
     * (a 3840-wide 32 bpp pitch sits just inside, anything wider
     * overflows) and forced a row-pitched upload; the buffer takes
     * any pitch and uploads as one memcpy. */
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = ((v->pitch * v->rows) + 3u) & ~3u;  /* words */
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.FirstElement = 0;
        sd.BufferEx.NumElements = bd.ByteWidth / 4;
        sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        for (unsigned i = 0; i < 2; i++) {
            hr = dx11.device->CreateBuffer(&bd, nullptr, &fb.raw[i]);
            if (FAILED(hr)) {
                dx11_log("raw buffer %u %ux%u failed: %#x", i, v->pitch,
                         v->rows, (unsigned)hr);
                return false;
            }
            hr = dx11.device->CreateShaderResourceView(fb.raw[i], &sd,
                                                       &fb.raw_srv[i]);
            if (FAILED(hr)) {
                dx11_log("raw SRV %u failed: %#x", i, (unsigned)hr);
                return false;
            }
        }
        fb.raw_cur = 0;
        fb.have_good = false;
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
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dd.SampleDesc.Count = 1;
    dd.MipLevels = 0;              /* full chain: the scale pass's
                                     * trilinear sampler then area-filters
                                     * any downscale to the window instead
                                     * of point-sampling 4 of every ~4x4
                                     * source pixels */
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    dd.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
                                   /* GenerateMips is a silent no-op
                                    * without this, and MipLevels 0 has
                                    * already allocated levels 1..N: the
                                    * trilinear sample in the scale pass
                                    * then reads an empty level at any
                                    * downscale and the frame is black */
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

    /* constant buffer: dim/misc per config; the post-transform half
     * (client size and the scaling options) is refreshed every frame in
     * dx11_render_frame, because the client size moves with resizes */
    Dx11Params cb = {
        { v->xres, v->yres, v->pitch, v->bpp },
        { v->xoffset, v->yoffset, v->pixo, 0 },
        { 0, 0, (uint32_t)video_opts.scaling, video_opts.scanlines ? 1u : 0u },
        { 0, 0, 0, 0 },
        { 0, 0, 0, 0 },
    };
    backdrop_params(&cb);
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
        /* The same filtering, wrapping: a backdrop tile repeats from the
         * top left, and its seams should filter like everything else. */
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        hr = dx11.device->CreateSamplerState(&sd, &fb.wrap);
        if (FAILED(hr)) {
            dx11_log("wrap sampler failed: %#x", (unsigned)hr);
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
    dx11_log("pipeline built: gen %u, %ux%u, pitch %u, bpp %u, pan %u,%u, "
             "base 0x%llx",
             v->generation, v->xres, v->yres, v->pitch, v->bpp,
             v->xoffset, v->yoffset, (unsigned long long)v->gbase);
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

static uint32_t fb_uploads;          /* frames copied */
static uint32_t fb_skipped;          /* frames the copy was skipped */
static uint32_t fb_dropped;          /* copies dropped, guest drew during */
static bool fb_seen_damage;          /* the blitter has ever reported */
static uint64_t fb_copy_us;          /* time actually spent copying */

/*
 * How long the copy may be deferred while waiting for the guest to stop
 * drawing.  Bounded for two reasons, both the Metal twin's: continuous
 * drawing -- a drag, a scroll -- never settles and still has to animate,
 * and text and lines are plotted straight to memory without passing
 * through the blitter, so nothing raises the flag for them.
 */
#define FB_SETTLE_MAX 4              /* frames */

/*
 * A switch for measuring.  With DX11_NO_DAMAGE set the screen is copied
 * every presented frame the way it was before, so the two behaviours can
 * be compared inside one binary instead of across two builds.
 */
static bool fb_damage_gate(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("DX11_NO_DAMAGE");
        on = !(e && *e && *e != '0');
    }
    return on != 0;
}

/*
 * Copy the guest's screen, but only when it is worth copying.
 *
 * The blitter sets a word on the vCPU thread every time a blit lands,
 * so one atomic read answers both of the questions that matter: has the
 * guest drawn anything since we last looked, and is it drawing right
 * now.  Copy when it has drawn and then stopped; skip entirely when it
 * has not drawn at all, which at rest is most frames; and when it
 * painted while we were reading, drop that copy rather than show a
 * splice of two guest states.  This replaces fingerprinting the
 * framebuffer to guess the same thing: the device already knows.
 */
static void fb_upload(const Dx11FbView *v)
{
    static bool pending;             /* drawn since we last showed a frame */
    static unsigned held;            /* frames since we last showed one */
    unsigned next = fb.raw_cur ^ 1u;
    bool drew = riscos_blitter_take_damage() != 0;
    bool settled;
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT hr;

    if (drew) {
        fb_seen_damage = true;
        pending = true;
    }

    /*
     * Deliberately not the Metal twin here.  Only the blitter raises
     * that flag, so a guest running without GVFill raises it never, and
     * gating on it regardless would drop the whole display to the
     * settle cadence for a guest that is drawing perfectly normally.
     * Until the blitter has been seen at least once, copy every frame
     * as before and cost nothing.
     */
    settled = pending && !drew;
    if (fb_damage_gate() && fb_seen_damage && fb.have_good && !settled
        && ++held < FB_SETTLE_MAX) {
        fb_skipped++;
        return;                      /* the previous whole frame stands */
    }
    held = 0;
    pending = false;

    hr = dx11.context->Map(fb.raw[next], 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
    if (FAILED(hr)) {
        if (frame_count % 300 == 0) {
            dx11_log("raw Map failed: %#x", (unsigned)hr);
        }
        return;
    }
    {
        LARGE_INTEGER a, b, f;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        memcpy(map.pData, v->fb, (size_t)v->pitch * v->rows);
        QueryPerformanceCounter(&b);
        fb_copy_us += (uint64_t)((b.QuadPart - a.QuadPart) * 1000000
                                 / f.QuadPart);
    }
    dx11.context->Unmap(fb.raw[next], 0);

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

    /* Painted while we were reading?  Then the bytes we took splice two
     * guest states; keep the last whole frame and take it again. */
    if (riscos_blitter_take_damage()) {
        fb_seen_damage = true;
        pending = true;
        if (fb_damage_gate() && fb.have_good && ++held < FB_SETTLE_MAX) {
            fb_dropped++;
            return;
        }
        held = 0;
    }

    fb.raw_cur = next;
    fb.have_good = true;
    fb_uploads++;
}

/*
 * Display space to guest pixels: RISC OS puts its screen on the display
 * the way the GPU would -- one scale for both axes, the image centred,
 * whatever is left over as a margin.  The pointer's destination rect
 * arrives in display pixels (the space the ROM's own scale arithmetic
 * produced it in), so it crosses the same transform to land on the
 * framebuffer's pixels.
 */
static void dx11_disp_transform(double disp_w, double disp_h,
                                double fb_w, double fb_h,
                                double *scale, double *mx, double *my)
{
    double k = disp_w / fb_w < disp_h / fb_h ? disp_w / fb_w : disp_h / fb_h;

    if (!(k > 0)) {
        k = 1.0;
    }
    *scale = k;
    *mx = (disp_w - fb_w * k) / 2.0;
    *my = (disp_h - fb_h * k) / 2.0;
}

/*
 * The host cursor is hidden whenever the guest is drawing the pointer
 * under it -- which is any time it is over the guest's screen in the
 * foreground window, not just while grabbed.  The tablet keeps the two
 * arrows in the same place, so showing both is showing the same
 * pointer twice.  ShowCursor is counted, so it is called only on a
 * change.
 */
static struct {
    bool hidden;
} cursor;

static void dx11_cursor_sync(bool sprite_visible)
{
    bool inside = false;

    if (sprite_visible && dx11.hwnd) {
        POINT p;
        if (GetCursorPos(&p) && WindowFromPoint(p) == dx11.hwnd) {
            RECT c;
            GetClientRect(dx11.hwnd, &c);
            POINT lc = p;
            ScreenToClient(dx11.hwnd, &lc);
            inside = lc.x >= 0 && lc.y >= 0 && lc.x < c.right && lc.y < c.bottom
                     && GetForegroundWindow() == dx11.hwnd;
        }
    }
    bool hide = sprite_visible && inside;
    if (hide == cursor.hidden) {
        return;
    }
    /*
     * ShowCursor keeps a counter, and the cursor is drawn while it is
     * >= 0: FALSE decrements (hides), TRUE increments (shows).  So the
     * call to hide is ShowCursor(FALSE), not ShowCursor(TRUE) -- the
     * other way round showed a second arrow inside the window and took
     * the host's cursor away everywhere else.  Drive the counter all
     * the way to the wanted side, because anything else in the process
     * may have moved it.
     */
    if (hide) {
        while (ShowCursor(FALSE) >= 0) {
            /* down to -1 */
        }
    } else {
        while (ShowCursor(TRUE) < 0) {
            /* up to 0 */
        }
    }
    cursor.hidden = hide;
}

/* The guest's pointer sprite, composited on top of the scaled frame.
 * The peer commits whole transactions at UpdateSubmit, so a generation
 * move is always a complete new sprite and position; a read that raced
 * one is reported stale and the previous frame's is kept. */
static void dx11_draw_pointer(const Dx11FbView *v)
{
    Dx11CursorView cv;

    if (!ptr.up && !ptr_build()) {
        dx11_cursor_sync(false);
        return;
    }
    if (dx11_glue_cursor_view(&cv) && !cv.stale
        && cv.generation != ptr.generation) {
        /* The peer packs the sprite tightly at its own width: argb is
         * img_w * img_h words, which is exactly how the Metal twin
         * indexes it (t.y * dim.x + t.x).  So the upload must carry the
         * source's own row pitch, into a destination box of the
         * sprite's own size.  Handing the whole 64x64 texture a fixed
         * DX11_CURSOR_TEXELS pitch instead takes each row from the
         * wrong offset -- the sprite comes out sheared -- and reads
         * 16 KB out of a buffer that is usually 4. */
        UINT sw = (UINT)cv.img_w, sh = (UINT)cv.img_h;
        UINT bw = sw > DX11_CURSOR_TEXELS ? DX11_CURSOR_TEXELS : sw;
        UINT bh = sh > DX11_CURSOR_TEXELS ? DX11_CURSOR_TEXELS : sh;
        if (sw > 0 && sh > 0) {
            D3D11_BOX box = { 0, 0, 0, bw, bh, 1 };
            dx11.context->UpdateSubresource(ptr.image, 0, &box,
                                            cv.argb, sw * 4, 0);
        }
        /* Watch the peer's numbers: the sprite is drawn stretched to the
         * rect it names, from a 64x64 texture it also sizes, so a bad
         * img_w/img_h shows neighbouring guest memory and a bad rect
         * smears it across the screen.  Log anything out of range, and
         * the first few either way for reference. */
        {
            static unsigned seen, bad;
            bool odd = cv.img_w <= 0 || cv.img_h <= 0
                       || cv.img_w > DX11_CURSOR_TEXELS
                       || cv.img_h > DX11_CURSOR_TEXELS
                       || cv.w <= 0 || cv.h <= 0
                       || cv.w > 8 * DX11_CURSOR_TEXELS
                       || cv.h > 8 * DX11_CURSOR_TEXELS
                       || cv.disp_w <= 0 || cv.disp_h <= 0;
            if (odd || seen < 8) {
                if (!odd || bad < 40) {
                    dx11_log("ptr%s gen %u vis %d img %dx%d rect %d,%d %dx%d "
                             "disp %dx%d", odd ? " ODD" : "",
                             cv.generation, cv.visible, cv.img_w, cv.img_h,
                             cv.x, cv.y, cv.w, cv.h, cv.disp_w, cv.disp_h);
                }
                if (odd) {
                    bad++;
                }
                seen++;
            }
        }
        ptr.generation = cv.generation;
        ptr.visible = cv.visible != 0;
        ptr.x = cv.x;
        ptr.y = cv.y;
        ptr.w = cv.w;
        ptr.h = cv.h;
        /* dim drives the shader's Load, so it must name what actually
         * reached the texture, never more than it holds */
        ptr.img_w = (int32_t)bw;
        ptr.img_h = (int32_t)bh;
        ptr.disp_w = cv.disp_w;
        ptr.disp_h = cv.disp_h;
    }
    dx11_cursor_sync(ptr.visible);

    if (!ptr.visible || ptr.w <= 0 || ptr.h <= 0
        || ptr.img_w <= 0 || ptr.img_h <= 0) {
        return;
    }

    /* dest rect: display pixels -> guest pixels -> NDC over the same
     * fullscreen mapping the scale pass drew, so the sprite follows
     * the frame whatever the window's aspect is */
    double dw = ptr.disp_w > 0 ? ptr.disp_w : (double)v->xres;
    double dh = ptr.disp_h > 0 ? ptr.disp_h : (double)v->yres;
    double k, mx, my, sx, sy, sw, sh;

    dx11_disp_transform(dw, dh, v->xres, v->yres, &k, &mx, &my);
    sx = ((double)ptr.x - mx) / k;
    sy = ((double)ptr.y - my) / k;
    sw = (double)ptr.w / k;
    sh = (double)ptr.h / k;

    struct { float rect[4]; } rc = {
        { (float)(2.0 * sx / (double)v->xres - 1.0),
          (float)(1.0 - 2.0 * sy / (double)v->yres),
          0.0f, 0.0f },
    };
    rc.rect[2] = (float)(rc.rect[0] + 2.0 * sw / (double)v->xres);
    rc.rect[3] = (float)(rc.rect[1] - 2.0 * sh / (double)v->yres);
    struct { uint32_t dim[2]; uint32_t pad[2]; } dm = {
        { (uint32_t)ptr.img_w, (uint32_t)ptr.img_h }, { 0, 0 },
    };

    dx11.context->UpdateSubresource(ptr.rect_cbuf, 0, nullptr, &rc, 0, 0);
    dx11.context->UpdateSubresource(ptr.dim_cbuf, 0, nullptr, &dm, 0, 0);
    dx11.context->VSSetShader(ptr.vs, nullptr, 0);
    dx11.context->VSSetConstantBuffers(1, 1, &ptr.rect_cbuf);
    dx11.context->PSSetShader(ptr.ps, nullptr, 0);
    dx11.context->PSSetConstantBuffers(1, 1, &ptr.dim_cbuf);
    dx11.context->PSSetShaderResources(0, 1, &ptr.image_srv);
    dx11.context->OMSetBlendState(ptr.blend, nullptr, 0xffffffff);
    dx11.context->Draw(3, 0);
    dx11.context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ID3D11ShaderResourceView *none[2] = { nullptr, nullptr };
    dx11.context->PSSetShaderResources(0, 2, none);
    ID3D11Buffer *nobuf = nullptr;
    dx11.context->VSSetConstantBuffers(1, 1, &nobuf);
    dx11.context->PSSetConstantBuffers(1, 1, &nobuf);
}

static bool dx11_render_frame(void)
{
    Dx11FbView v;

    if (++frame_count % 300 == 0) {
        dx11_log("frame %u: pipeline %s, fb gen %u, %ux%u bpp %u, "
                 "copied %u, skipped %u, dropped %u, copy %llu ms, "
                 "blitter %s, mouse moves %u, buttons %u",
                 frame_count, fb.up ? "up" : "down",
                 fb.generation, fb.xres, fb.yres, fb.bpp,
                 fb_uploads, fb_skipped, fb_dropped,
                 (unsigned long long)(fb_copy_us / 1000),
                 fb_seen_damage ? "seen" : "absent",
                 dx11.mouse_moves, dx11.mouse_buttons);
    }
    /* The status line: window title carries the guest's mode and the
     * presented frame rate, once a second.  (The sprint's instruction
     * rate needs TCG counters plumbed to the UI; still open.) */
    if (frame_count % 60 == 0) {
        static uint32_t last_second_frame;
        static uint64_t last_tick;
        uint64_t now = GetTickCount64();
        if (last_tick && now > last_tick) {
            unsigned fps = (unsigned)((frame_count - last_second_frame)
                                      * 1000 / (now - last_tick));
            wchar_t title[128];
            _snwprintf(title, ARRAYSIZE(title),
                       L"RISC OS 5 — %ux%u, %u bpp · %u fps — Raspberry Pi 4 (QEMU)",
                       fb.xres, fb.yres, fb.bpp, fps);
            SetWindowTextW(dx11.hwnd, title);
        }
        last_tick = now;
        last_second_frame = frame_count;
    }

    if (!dx11_glue_fb_view(&v)) {
        if (frame_count % 300 == 0) {
            dx11_log("frame %u: no fb view yet", frame_count);
        }
        return false;
    }
    mouse.gxres = v.xres;            /* the mouse maps onto this screen */
    mouse.gyres = v.yres;

    /* The window opens before the machine exists, at a readable default.
     * Once the guest's mode has settled (boot passes through smaller
     * sizes first), the window is resized once to that mode at the
     * system's DPI scale -- the desktop's default mode at boot, which
     * with -global bcm2835-property.mode=WxH is the one that was asked
     * for.  After this one resize the window is the user's. */
    {
        static uint32_t stable_gens;
        static bool sized;
        static uint32_t last_xres, last_yres;
        if (!sized) {
            if (v.xres == last_xres && v.yres == last_yres) {
                stable_gens++;
            } else {
                stable_gens = 0;
                last_xres = v.xres;
                last_yres = v.yres;
            }
            if (stable_gens == 180) {          /* ~3 s unchanged at 60 fps */
                UINT dpi = 96;
                typedef UINT (WINAPI *GDA)(void);
                HMODULE u = GetModuleHandleW(L"user32.dll");
                GDA gda = u ? (GDA)GetProcAddress(u, "GetDpiForSystem") : nullptr;
                if (gda) {
                    dpi = gda();
                }
                int scale = dpi / 96 > 0 ? (int)(dpi / 96) : 1;
                RECT r = { 0, 0, (LONG)v.xres * scale, (LONG)v.yres * scale };
                AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
                SetWindowPos(dx11.hwnd, nullptr, 0, 0,
                             r.right - r.left, r.bottom - r.top,
                             SWP_NOMOVE | SWP_NOZORDER);
                sized = true;
            }
        }
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

    /*
     * Take the guest's screen when it is worth taking: fb_upload asks
     * the blitter what the guest has drawn rather than guessing.
     *
     * Pacing this to the guest's vsync was tried first and made things
     * worse, because the Wimp does not paint on vsync -- it paints when
     * a task next polls -- so there is no quiet phase to aim at, and
     * holding each frame for two host frames left every tear up twice
     * as long.  The blitter's damage word answers the question that
     * actually matters, exactly and for free.
     */
    fb_upload(&v);

    /* decode pass: raw bytes -> linear RGB */
    ID3D11ShaderResourceView *srvs[2] = { fb.raw_srv[fb.raw_cur],
                                          fb.pal_srv };
    dx11.context->PSSetShaderResources(0, 2, srvs);
    dx11.context->PSSetShader(fb.ps, nullptr, 0);
    dx11.context->VSSetShader(fb.vs, nullptr, 0);
    dx11.context->PSSetConstantBuffers(0, 1, &fb.cbuf);
    dx11.context->OMSetRenderTargets(1, &fb.dec_rtv, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)v.xres, (float)v.yres, 0, 1 };
    dx11.context->RSSetViewports(1, &vp);
    dx11.context->Draw(3, 0);

    /* the mip chain over the fresh decode: each level is the 2x2
     * average of the one above, so whatever the window size, the
     * trilinear sample in the scale pass averages every source pixel
     * an output pixel covers (a plain bilinear at half scale touches
     * 4 texels of the ~4.1 covered and text dissolves into shimmer).
     * The texture must first be unbound as a render target or the
     * generation reads it while it is still the draw target, which
     * the driver answers with stripes */
    dx11.context->OMSetRenderTargets(0, nullptr, nullptr);
    dx11.context->GenerateMips(fb.dec_srv);

    /* scale pass: decoded surface over the whole client area */
    ID3D11ShaderResourceView *dec = fb.dec_srv;
    dx11.context->OMSetRenderTargets(1, &dx11.rtv, nullptr);
    /* t0 the decoded guest screen, t1 the backdrop image if there is one;
     * the shader only reads t1 when the mode says to, but an unbound slot
     * would still be a warning under the debug layer. */
    ID3D11ShaderResourceView *ssrv[2] = { dec, backdrop.image_srv };
    dx11.context->PSSetShaderResources(0, 2, ssrv);
    dx11.context->PSSetShader(fb.scale_ps, nullptr, 0);
    ID3D11SamplerState *samp[2] = { fb.linear, fb.wrap };
    dx11.context->PSSetSamplers(0, 2, samp);
    RECT client;
    GetClientRect(dx11.hwnd, &client);
    if (fb.cbuf) {
        /* the post-transform half of the constants: the client size the
         * scaler maps onto, refreshed here because resizes move it */
        Dx11Params cb = {
            { v.xres, v.yres, v.pitch, v.bpp },
            { v.xoffset, v.yoffset, v.pixo, 0 },
            { (uint32_t)(client.right - client.left),
              (uint32_t)(client.bottom - client.top),
              (uint32_t)video_opts.scaling, video_opts.scanlines ? 1u : 0u },
            { 0, 0, 0, 0 },
            { 0, 0, 0, 0 },
        };
        backdrop_params(&cb);
        dx11.context->UpdateSubresource(fb.cbuf, 0, nullptr, &cb, 0, 0);
    }
    D3D11_VIEWPORT vp2 = { 0, 0,
                           (float)(client.right - client.left),
                           (float)(client.bottom - client.top), 0, 1 };
    dx11.context->RSSetViewports(1, &vp2);
    dx11.context->Draw(3, 0);

    dx11_draw_pointer(&v);

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

/* ------------------------------------------------------------------ */
/* The composited frame                                                 */

/*
 * What is actually on the screen: the guest's desktop scaled into the
 * client area, the backdrop showing through underneath it and the
 * pointer over the top.  That picture only exists in the swap chain's
 * back buffer, and the flip model leaves it undefined after Present, so
 * the copy has to be taken inside the frame -- after the last draw and
 * before the flip.
 *
 * Two callers want one.  PrintScreen writes it straight out as a PNG.
 * QMP's screendump wants the pixels, and asks from the QEMU thread; both
 * set `want` and wait for the frame loop to answer.  Before this the two
 * captured the decoded guest buffer, which is what the guest drew rather
 * than what the display shows -- no backdrop, no pointer, and always the
 * mode's own size rather than the window's.
 */
static struct {
    CRITICAL_SECTION lock;      /* over the buffer; held while a reader has it */
    CRITICAL_SECTION turn;      /* one reader at a time */
    HANDLE ready;               /* set when a copy has landed */
    ID3D11Texture2D *staging;
    uint8_t *pixels;            /* BGRA, w * h * 4 */
    uint32_t w, h;
    size_t cap;
    bool want;                  /* a copy has been asked for */
    bool png;                   /* ... and PrintScreen wants a file of it */
    bool up;
} comp;

static void comp_init(void)
{
    if (comp.up) {
        return;
    }
    InitializeCriticalSection(&comp.lock);
    InitializeCriticalSection(&comp.turn);
    comp.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);  /* manual reset */
    comp.up = true;
}

/* The PNG the window writes for PrintScreen, from the mapped back buffer
 * (B,G,R,A in memory, so the channels come out reversed). */
static void comp_write_png(const uint8_t *src, uint32_t pitch,
                           uint32_t w, uint32_t h)
{
    static int shot;
    char name[64];
    FILE *f;

    snprintf(name, sizeof(name), "dx11-screenshot-%03d.png", ++shot);
    f = fopen(name, "wb");
    if (!f) {
        return;
    }
    uint8_t ihdr[13] = {
        (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
        8, 2, 0, 0, 0,                 /* 8-bit truecolour RGB */
    };
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    png_write_chunk(f, "IHDR", ihdr, 13);

    /* filter byte 0 (none) + RGB rows, deflated into one IDAT */
    uint32_t row = w * 3 + 1;
    uint8_t *raw_rows = (uint8_t *)malloc((size_t)row * h);
    if (raw_rows) {
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *s = src + (size_t)y * pitch;
            uint8_t *dst = raw_rows + (size_t)y * row;

            dst[0] = 0;
            for (uint32_t x = 0; x < w; x++) {
                dst[1 + x * 3 + 0] = s[x * 4 + 2];
                dst[1 + x * 3 + 1] = s[x * 4 + 1];
                dst[1 + x * 3 + 2] = s[x * 4 + 0];
            }
        }
        uLongf zlen = compressBound((uLong)row * h);
        uint8_t *zbuf = (uint8_t *)malloc(zlen);
        if (zbuf && compress2(zbuf, &zlen, raw_rows, (uLong)row * h, 6) == Z_OK) {
            png_write_chunk(f, "IDAT", zbuf, (uint32_t)zlen);
        }
        free(zbuf);
        free(raw_rows);
    }
    png_write_chunk(f, "IEND", nullptr, 0);
    fclose(f);
    dx11_log("screenshot: %s (%ux%u, composited)", name, w, h);
}

/*
 * Take the copy, if anyone asked for one.  On the UI thread, between the
 * frame's last draw and Present.
 */
static void comp_capture(void)
{
    ID3D11Texture2D *back = nullptr;
    D3D11_TEXTURE2D_DESC bd;
    D3D11_MAPPED_SUBRESOURCE map;
    bool png;

    if (!comp.up || !comp.want || !dx11.swap) {
        return;
    }
    if (FAILED(dx11.swap->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    (void **)&back))) {
        return;
    }
    back->GetDesc(&bd);

    /* One staging texture, rebuilt only when the window changes size. */
    if (comp.staging) {
        D3D11_TEXTURE2D_DESC sd;

        comp.staging->GetDesc(&sd);
        if (sd.Width != bd.Width || sd.Height != bd.Height
            || sd.Format != bd.Format) {
            comp.staging->Release();
            comp.staging = nullptr;
        }
    }
    if (!comp.staging) {
        D3D11_TEXTURE2D_DESC sd = bd;

        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.MiscFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dx11.device->CreateTexture2D(&sd, nullptr, &comp.staging))) {
            back->Release();
            return;
        }
    }
    dx11.context->CopyResource(comp.staging, back);
    back->Release();

    if (FAILED(dx11.context->Map(comp.staging, 0, D3D11_MAP_READ, 0, &map))) {
        return;
    }
    EnterCriticalSection(&comp.lock);
    png = comp.png;
    comp.png = false;
    comp.want = false;
    if (comp.cap < (size_t)bd.Width * bd.Height * 4) {
        free(comp.pixels);
        comp.cap = (size_t)bd.Width * bd.Height * 4;
        comp.pixels = (uint8_t *)malloc(comp.cap);
    }
    if (comp.pixels) {
        for (uint32_t y = 0; y < bd.Height; y++) {
            memcpy(comp.pixels + (size_t)y * bd.Width * 4,
                   (const uint8_t *)map.pData + (size_t)y * map.RowPitch,
                   (size_t)bd.Width * 4);
        }
        comp.w = bd.Width;
        comp.h = bd.Height;
    }
    LeaveCriticalSection(&comp.lock);
    SetEvent(comp.ready);

    if (png) {
        comp_write_png((const uint8_t *)map.pData, map.RowPitch,
                       bd.Width, bd.Height);
    }
    dx11.context->Unmap(comp.staging, 0);
}

/* PrintScreen: ask the frame loop for the next composited frame. */
void dx11_screenshot(void)
{
    if (!comp.up) {
        return;
    }
    EnterCriticalSection(&comp.lock);
    comp.png = true;
    comp.want = true;
    LeaveCriticalSection(&comp.lock);
}

/*
 * screendump's half of it: wait for a fresh composited frame and hand the
 * pixels over.  The lock is held until dx11_glue_composite_done, so the
 * frame loop cannot overwrite them while they are being read.
 */
extern "C" int dx11_glue_composite_get(uint32_t *w, uint32_t *h,
                                       const void **pixels, int wait_ms)
{
    if (!comp.up) {
        return 0;
    }
    EnterCriticalSection(&comp.turn);
    EnterCriticalSection(&comp.lock);
    ResetEvent(comp.ready);
    comp.want = true;
    LeaveCriticalSection(&comp.lock);

    WaitForSingleObject(comp.ready, wait_ms < 0 ? 0 : (DWORD)wait_ms);

    EnterCriticalSection(&comp.lock);
    if (!comp.pixels || !comp.w || !comp.h) {
        LeaveCriticalSection(&comp.lock);
        LeaveCriticalSection(&comp.turn);
        return 0;                   /* no frame yet: the caller falls back */
    }
    *w = comp.w;
    *h = comp.h;
    *pixels = comp.pixels;
    return 1;
}

extern "C" void dx11_glue_composite_done(void)
{
    if (!comp.up) {
        return;
    }
    LeaveCriticalSection(&comp.lock);
    LeaveCriticalSection(&comp.turn);
}

/* ------------------------------------------------------------------ */
/* The boundary, called from ui/dx11.c                                 */

extern "C" int dx11_backend_init(void)
{
    /* Per-monitor DPI awareness, so the window is not bitmapscaled on
     * displays with scaling: the emulator scales the guest itself. */
    {
        typedef BOOL (WINAPI *SetCtx)(HANDLE);
        HMODULE u = GetModuleHandleW(L"user32.dll");
        SetCtx set_ctx = u ? (SetCtx)GetProcAddress(
            u, "SetProcessDpiAwarenessContext") : nullptr;
        if (set_ctx) {
            set_ctx((HANDLE)-4);      /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
        } else {
            HMODULE sc = LoadLibraryW(L"shcore.dll");
            if (sc) {
                typedef HRESULT (WINAPI *SetDpi)(int);
                SetDpi set_dpi = (SetDpi)GetProcAddress(
                    sc, "SetProcessDpiAwareness");
                if (set_dpi) {
                    set_dpi(2);       /* PROCESS_PER_MONITOR_DPI_AWARE */
                }
                FreeLibrary(sc);
            }
        }
    }

    comp_init();
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
        /* Minimized: DWM flips nothing, so present nothing. */
        if (IsIconic(dx11.hwnd)) {
            Sleep(50);
            continue;
        }
        /* Wait for a free frame-latency slot with the message queue
         * armed, so input keeps flowing while the display is stalled
         * (occlusion, DWM hiccup) instead of ghosting the window.
         * Present is only called once a slot is free, which is what
         * keeps it from blocking: presenting after a mere timeout is
         * how the window wedged inside Present when DWM stopped
         * compositing it.  A blt-model fallback chain paces on a
         * plain sleep instead (its Present copies, it cannot wedge). */
        if (dx11.swap_wait) {
            DWORD w9 = MsgWaitForMultipleObjectsEx(
                1, &dx11.swap_wait, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (w9 != WAIT_OBJECT_0) {
                continue;           /* message or timeout: no free slot */
            }
        } else {
            Sleep(1);
        }
        if (dx11_acquire_target()) {
            if (!dx11_render_frame()) {
                dx11.context->ClearRenderTargetView(dx11.rtv, clear);
            }
            /* The screen as it will appear, while the back buffer still
             * holds it: the flip leaves its contents undefined. */
            comp_capture();
            HRESULT hr = dx11.swap->Present(1 /* vsync */, 0);
            if (FAILED(hr)) {
                dx11_log("present failed hr=%08lx", (unsigned long)hr);
                Sleep(10);
            }
        } else {
            Sleep(1);
        }
    }

    dx11_backend_request_shutdown();
    dx11_glue_fb_done();

    dx11_cursor_sync(false);         /* show the host cursor again */
    ptr_release();
    if (fb.cbuf) fb.cbuf->Release();
    if (fb.linear) fb.linear->Release();
    if (fb.wrap) fb.wrap->Release();
    if (fb.vs) fb.vs->Release();
    if (fb.scale_ps) fb.scale_ps->Release();
    for (size_t i = 0; i < sizeof(fb_ps_all) / sizeof(fb_ps_all[0]); i++) {
        if (fb_ps_all[i]) fb_ps_all[i]->Release();
    }
    fb_release_pipeline();
    if (dx11.rtv) dx11.rtv->Release();
    if (dx11.swap_wait) CloseHandle(dx11.swap_wait);
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
