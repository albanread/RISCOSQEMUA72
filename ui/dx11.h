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

#ifdef __cplusplus
}
#endif

#endif /* QEMU_UI_DX11_H */
