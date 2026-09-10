/*
 * dx11.c — the QEMU-side glue for the D3D11 front end.  QEMU's headers
 * are not C++-parseable, so this file is C: it registers the display
 * backend, gathers each frame's view of the guest framebuffer, and
 * forwards into ui/dx11.cpp across the extern "C" boundary declared in
 * ui/dx11.h.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu-main.h"
#include "qemu/main-loop.h"
#include "system/runstate.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/win32-kbd-hook.h"
#include "qapi/qapi-types-ui.h"
#include "ui/dx11.h"
#include "hw/display/bcm2835_fb.h"
#include "hw/misc/bcm2835_vsyncgen.h"
#include "system/address-spaces.h"
#include "qom/object.h"

void dx11_backend_request_shutdown(void)
{
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
}

/*
 * The framebuffer view, cached across frames.  Guest RAM is mapped once
 * per config generation and unmapped when the generation moves; the UI
 * thread reads the mapped bytes directly, with no lock, exactly as a
 * monitor would scan a real framebuffer mid-update.
 */
static struct {
    BCM2835FBState *fb;
    uint32_t generation;
    bool mapped;
    void *fb_ptr;
    void *pal_ptr;
    hwaddr fb_len;
} view;

int dx11_glue_fb_view(Dx11FbView *out)
{
    BCM2835FBConfig cfg;
    uint32_t gen, rows;

    if (!view.fb) {
        Object *obj = object_resolve_path_type("", TYPE_BCM2835_FB, NULL);

        if (!obj) {
            return 0;               /* machine not built yet */
        }
        view.fb = BCM2835_FB(obj);
    }

    gen = bcm2835_fb_get_config(view.fb, &cfg);
    if (!cfg.xres || !cfg.yres) {
        return 0;                   /* no mode programmed yet */
    }

    if (view.mapped && gen != view.generation) {
        address_space_unmap(&view.fb->dma_as, view.fb_ptr, view.fb_len,
                            false, 0);
        address_space_unmap(&view.fb->dma_as, view.pal_ptr, 256 * 4,
                            false, 0);
        view.mapped = false;
    }

    if (!view.mapped) {
        /*
         * Map exactly what the guest indexes: the visible rows plus the pan
         * offset, which the fb model applies only when the virtual size
         * exceeds the physical one (the offsets are meaningless otherwise).
         * The palette sits at the VideoCore RAM base, 256 words of
         * 0x00BBGGRR, where SET_PALETTE writes it.
         */
        bool use_offsets = cfg.xres_virtual > cfg.xres ||
                           cfg.yres_virtual > cfg.yres;
        rows = cfg.yres + (use_offsets ? cfg.yoffset : 0);
        hwaddr fb_len = rows * bcm2835_fb_get_pitch(&cfg);
        hwaddr pal_len = 256 * 4;

        view.fb_ptr = address_space_map(&view.fb->dma_as, cfg.base, &fb_len,
                                        false, MEMTXATTRS_UNSPECIFIED);
        view.pal_ptr = address_space_map(&view.fb->dma_as,
                                         view.fb->vcram_base, &pal_len,
                                         false, MEMTXATTRS_UNSPECIFIED);
        if (!view.fb_ptr || !view.pal_ptr) {
            if (view.fb_ptr) {
                address_space_unmap(&view.fb->dma_as, view.fb_ptr, fb_len,
                                    false, 0);
                view.fb_ptr = NULL;
            }
            return 0;
        }

        view.fb_len = fb_len;
        view.generation = gen;
        view.mapped = true;
    }

    rows = view.fb_len / bcm2835_fb_get_pitch(&cfg);

    out->xres = cfg.xres;
    out->yres = cfg.yres;
    /* Offsets only count when the viewport exceeds the physical screen, as
     * the fb model itself decides; mirror that or the image would shift. */
    if (cfg.xres_virtual > cfg.xres || cfg.yres_virtual > cfg.yres) {
        out->xoffset = cfg.xoffset;
        out->yoffset = cfg.yoffset;
    } else {
        out->xoffset = 0;
        out->yoffset = 0;
    }
    out->pitch = bcm2835_fb_get_pitch(&cfg);
    out->bpp = cfg.bpp;
    out->pixo = cfg.pixo;
    out->rows = rows;
    out->generation = view.generation;
    out->gbase = cfg.base;
    out->fb = view.fb_ptr;
    out->palette = view.pal_ptr;
    return 1;
}

void dx11_glue_fb_done(void)
{
    if (view.mapped) {
        address_space_unmap(&view.fb->dma_as, view.fb_ptr, view.fb_len,
                            false, 0);
        address_space_unmap(&view.fb->dma_as, view.pal_ptr, 256 * 4,
                            false, 0);
        view.mapped = false;
    }
    view.fb = NULL;
}

/* ------------------------------------------------------------------ */
/* Input: Win32 window messages to QEMU input events, under the BQL    */

void dx11_glue_key(bool down, uint32_t lparam)
{
    /*
     * Bits 16-23 of a WM_KEY* lParam are the AT set 1 scan code and bit 24
     * the 0xE0 prefix, which is exactly how the atset1 keymap is indexed.
     * (The win32 keymap is keyed by virtual-key codes: index it with a scan
     * code and 'z', 0x2c, becomes VK_SNAPSHOT -- the Print key.)
     */
    uint32_t scancode = (lparam >> 16) & 0xff;
    uint32_t code = (lparam & (1u << 24)) ? (0xe000 | scancode) : scancode;

    if (code >= qemu_input_map_atset1_to_linux_len) {
        return;
    }
    unsigned int lnx = qemu_input_map_atset1_to_linux[code];
    if (lnx == 0) {
        return;
    }

    bql_lock();
    qemu_input_event_send_key_linux(NULL, lnx, down);
    bql_unlock();
}

void dx11_glue_mouse_abs(int gx, int gy, int xres, int yres)
{
    if (xres < 2 || yres < 2) {
        return;
    }
    bql_lock();
    /* Scaled by the input layer onto the tablet's 0..32767 axes, which
     * the guest's absolute-mouse driver maps onto the whole screen. */
    qemu_input_queue_abs(NULL, INPUT_AXIS_X, gx, 0, xres - 1);
    qemu_input_queue_abs(NULL, INPUT_AXIS_Y, gy, 0, yres - 1);
    qemu_input_event_sync();
    bql_unlock();
}

void dx11_glue_mouse_btn(int button, bool down)
{
    static const InputButton map[3] = {
        INPUT_BUTTON_LEFT, INPUT_BUTTON_MIDDLE, INPUT_BUTTON_RIGHT,
    };

    if ((unsigned)button > 2) {
        return;
    }
    bql_lock();
    qemu_input_queue_btn(NULL, map[button], down);
    qemu_input_event_sync();
    bql_unlock();
}

void dx11_glue_mouse_wheel(int notches)
{
    if (!notches) {
        return;
    }
    bql_lock();
    while (notches > 0) {
        qemu_input_queue_btn(NULL, INPUT_BUTTON_WHEEL_UP, true);
        qemu_input_queue_btn(NULL, INPUT_BUTTON_WHEEL_UP, false);
        notches--;
    }
    while (notches < 0) {
        qemu_input_queue_btn(NULL, INPUT_BUTTON_WHEEL_DOWN, true);
        qemu_input_queue_btn(NULL, INPUT_BUTTON_WHEEL_DOWN, false);
        notches++;
    }
    qemu_input_event_sync();
    bql_unlock();
}

void dx11_glue_grab(bool on)
{
    win32_kbd_set_grab(on);
}

void dx11_glue_kbd_hook_window(void *hwnd)
{
    win32_kbd_set_window(hwnd);
}

/* ------------------------------------------------------------------ */
/* Guest vertical sync                                                 */

bool dx11_glue_set_vsync_hz(int hz)
{
    Object *obj = object_resolve_path_type("", TYPE_BCM2835_VSYNCGEN, NULL);

    if (!obj) {
        return false;
    }
    bcm2835_vsyncgen_set_hz(BCM2835_VSYNCGEN(obj), hz);
    return true;
}

static void dx11_display_init(DisplayState *ds, DisplayOptions *opts)
{
    Dx11FbView prime;
    int vsync_hz;

    if (dx11_backend_init() != 0) {
        error_report("dx11: cannot create window or D3D11 device");
        exit(1);
    }
    /* Resolve the framebuffer device now, while this thread is the only
     * one running; dx11_glue_fb_view keeps the cache warm after that. */
    dx11_glue_fb_view(&prime);

    /* Guest vertical sync rate. The machine's generator delivers it from
     * the timer thread; this only sets the rate. */
    if (opts->u.dx11.has_vsync) {
        vsync_hz = MIN(MAX(opts->u.dx11.vsync, 0), 1000);
        if (!dx11_glue_set_vsync_hz(vsync_hz)) {
            warn_report("dx11: this machine has no vertical-sync "
                        "generator; vsync= ignored");
        }
    }

    /* Scaler and CRT flavour.  The qapi enum order is the integer order
     * the window expects: 0 linear, 1 sharp, 2 nearest. */
    {
        int scaling = 1;                    /* sharp */

        if (opts->u.dx11.has_scaling) {
            switch (opts->u.dx11.scaling) {
            case DISPLAY_DX11_SCALING_LINEAR:
                scaling = 0;
                break;
            case DISPLAY_DX11_SCALING_SHARP:
                scaling = 1;
                break;
            case DISPLAY_DX11_SCALING_NEAREST:
                scaling = 2;
                break;
            default:
                break;
            }
        }
        dx11_glue_video_opts(scaling, opts->u.dx11.has_scanlines
                                     && opts->u.dx11.scanlines);
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
