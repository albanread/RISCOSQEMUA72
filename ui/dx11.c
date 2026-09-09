/*
 * dx11.c — the QEMU-side glue for the D3D11 front end.  QEMU's headers
 * are not C++-parseable, so this file is C: it registers the display
 * backend and forwards into ui/dx11.cpp across the extern "C" boundary
 * declared in ui/dx11.h.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu-main.h"
#include "system/runstate.h"
#include "ui/console.h"
#include "ui/dx11.h"

void dx11_backend_request_shutdown(void)
{
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
}

static void dx11_display_init(DisplayState *ds, DisplayOptions *opts)
{
    if (dx11_backend_init() != 0) {
        error_report("dx11: cannot create window or D3D11 device");
        exit(1);
    }
    /* The hand-off: system/main.c sees this set after qemu_init and runs
     * the QEMU main loop on its own thread, giving the UI the main one. */
    qemu_main = dx11_backend_main;
}

static QemuDisplay qemu_display_dx11 = {
    .type       = DISPLAY_TYPE_DX11,
    .init       = dx11_display_init,
};

static void dx11_register_types(void)
{
    qemu_display_register(&qemu_display_dx11);
}
type_init(dx11_register_types);
