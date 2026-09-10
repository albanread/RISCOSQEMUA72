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

#ifdef __cplusplus
}
#endif

#endif /* QEMU_UI_DX11_H */
