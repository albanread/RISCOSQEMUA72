/*
 * dx11.h — the boundary between the C++ D3D11 front end (ui/dx11.cpp)
 * and QEMU's C world (ui/dx11.c).  Everything here is extern "C".
 */
#ifndef QEMU_UI_DX11_H
#define QEMU_UI_DX11_H

#ifdef __cplusplus
extern "C" {
#endif

/* Called from QEMU display init, on the main thread inside qemu_init:
 * creates the window and the D3D11 device/swap chain.  Nonzero on
 * failure (reported by the caller). */
int dx11_backend_init(void);

/* The UI main loop: Win32 message pump + vsync clear/present.  Returns
 * after the window closes.  Installed as QEMU's qemu_main, so QEMU's
 * main loop runs on its own thread while this owns the main thread. */
int dx11_backend_main(void);

/* Cleanly request the machine to power off (window closed). */
void dx11_backend_request_shutdown(void);

/*
 * A frame's view of the guest framebuffer, gathered by dx11_glue_fb_view()
 * on the UI thread without holding the BQL.  The pointers are host
 * addresses into guest RAM, mapped for as long as the view is current.
 */
typedef struct Dx11FbView {
    uint32_t xres, yres;            /* visible size, pixels */
    uint32_t xoffset, yoffset;      /* pan within the buffer */
    uint32_t pitch;                 /* bytes per buffer row */
    uint32_t bpp;                   /* guest pixel format, bits */
    uint32_t pixo;                  /* 1 = RGB order, 0 = BGR */
    uint32_t rows;                  /* mapped buffer rows (>= yres+yoffset) */
    uint32_t generation;            /* config generation of this view */
    uint64_t gbase;                 /* guest address of the buffer */
    const void *fb;                 /* raw framebuffer bytes, pitch * rows */
    const void *palette;            /* 256 * 4 bytes, 0x00BBGGRR; always set */
} Dx11FbView;

/*
 * Fill `out` with the current framebuffer view, mapping guest RAM for it.
 * Returns 1 when the view is usable, 0 while there is no framebuffer
 * device yet (before machine init), and the mapping is kept across calls
 * until the config generation moves.  Thread: UI only.
 */
int dx11_glue_fb_view(Dx11FbView *out);

/* Drop any cached mapping (called once from the UI loop at exit). */
void dx11_glue_fb_done(void);

/*
 * Input, called from the UI thread's window procedure.  Each call takes
 * the BQL just long enough to run the QEMU input API, the way cocoa's
 * with_bql does; nothing else of QEMU is touched.
 */
/* lParam of WM_KEYDOWN/UP/SYSKEYDOWN/UP: scan code bits 16..23, extended
 * bit 24; repeats are already dropped by the caller. */
void dx11_glue_key(bool down, uint32_t lparam);
/* Guest pointer position in guest pixels on a xres x yres screen: sent
 * absolutely, the way the guest's tablet driver maps onto the screen. */
void dx11_glue_mouse_abs(int gx, int gy, int xres, int yres);
void dx11_glue_mouse_btn(int button, bool down); /* 0 left, 1 middle, 2 right */
void dx11_glue_mouse_wheel(int notches);         /* positive = away from user */
/* Keyboard grab: while on, the low-level hook swallows Alt+Tab and the
 * Windows key instead of letting the host see them. */
void dx11_glue_grab(bool on);
/* Install the low-level keyboard hook for this window; call once from the
 * thread that pumps messages, before any grab. */
void dx11_glue_kbd_hook_window(void *hwnd);

/* Guest vertical sync rate, pulses per second, 0 for none. The machine's
 * vsync generator delivers them from the timer thread; false if the
 * machine has no generator. */
bool dx11_glue_set_vsync_hz(int hz);

/* Scaler and CRT options for the window: scaling is 0 linear, 1 sharp
 * bilinear, 2 nearest; set once at display init, before any shader is
 * compiled. */
void dx11_glue_video_opts(int scaling, int scanlines);

/* Reload the named-by-convention snapshot ("desktop") from the window's
 * system menu; the load itself runs as a bottom half on the main loop. */
void dx11_glue_load_snapshot(void);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_UI_DX11_H */
