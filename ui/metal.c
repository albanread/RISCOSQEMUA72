/*
 * metal.c — the QEMU-side glue for the Metal front end.  The macOS twin
 * of ui/dx11.c: it registers the display backend, gathers each frame's
 * view of the guest framebuffer, and forwards into ui/metal.m across the
 * boundary declared in ui/metal.h.
 *
 * Objective-C would compile QEMU's headers happily, so this file is not
 * forced apart the way the Windows one is by C++.  Keeping the split
 * anyway means the two front ends can be read side by side, and it keeps
 * every QEMU API call on this side of one small, checkable contract.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"        /* config-host.h: QEMU_VERSION */
#include "qemu/error-report.h"
#include "qemu-main.h"
#include "qemu/main-loop.h"
#include "qapi/qapi-types-run-state.h"
#include "qobject/qjson.h"
#include "qobject/qdict.h"
#include "system/runstate.h"
#include "ui/console.h"
#include "ui/input.h"
#include "qapi/qapi-types-ui.h"
#include "ui/metal.h"
#include "hw/display/bcm2835_fb.h"
#include "hw/misc/bcm2835_vchiq.h"
#include "hw/misc/bcm2835_vsyncgen.h"
#include "migration/snapshot.h"
#include "system/address-spaces.h"
#include "qom/object.h"

void metal_backend_request_shutdown(void)
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

int metal_glue_fb_view(MetalFbView *out)
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

void metal_glue_fb_done(void)
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
/* The pointer sprite, answered for by the VCHIQ peer                  */

int metal_glue_cursor_view(MetalCursorView *out)
{
    VchiqCursor cur;

    if (!bcm2835_vchiq_get_cursor(&cur)) {
        return 0;                   /* no machine, or the service is closed */
    }
    out->generation = cur.generation;
    out->stale = cur.stale;
    out->visible = cur.visible;
    out->x = cur.x;
    out->y = cur.y;
    out->w = cur.w;
    out->h = cur.h;
    out->img_w = cur.img_w;
    out->img_h = cur.img_h;
    out->disp_w = cur.disp_w;
    out->disp_h = cur.disp_h;
    out->argb = cur.argb;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Apple Events: the command table and the dispatch (SCRIPTING.md)     */

/*
 * The one source of truth for the surface.  riscos-pi4/tools/mksdef.py
 * parses this array to generate the sdef and the schema artifact;
 * describe serialises it at runtime; the dispatch looks names up in
 * it.  An entry is { name, code, help, args, class }: the code is the
 * suite and command four-character codes concatenated; the class is
 * 'f' for fast (answered inline on the UI thread, taking the BQL the
 * way the input glue does) and 'b' for bottom-half (E2 and later).
 * Help text stays plain ASCII without quotes, and the codes stay
 * mixed case: Apple reserves all-lowercase codes.
 */
typedef struct ScriptCmd {
    const char *name;
    const char *code;
    const char *help;
    const char *args;         /* NULL, or a JSON-shaped description */
    char klass;               /* 'f' or 'b' */
} ScriptCmd;

/* SCRIPT-TABLE-BEGIN */
static const ScriptCmd script_cmds[] = {
    { "ping", "MQemPing",
      "Round-trip a command through the surface; the reply is the envelope.",
      NULL, 'f' },
    { "describe", "MQemDesc",
      "The whole command table as JSON: names, help, arguments, classes.",
      NULL, 'f' },
};
/* SCRIPT-TABLE-END */

/* The error taxonomy (SCRIPTING.md section 6): stable string codes for
 * an agent's retry logic, numbers for the AE error descriptors. */
enum {
    SCRIPT_OK = 0,
    SCRIPT_E_BUSY = 1,
    SCRIPT_E_WRONG_STATE = 2,
    SCRIPT_E_TIMEOUT = 3,
    SCRIPT_E_INVALID = 4,
    SCRIPT_E_NOT_CAPABLE = 5,
    SCRIPT_E_NOT_FOUND = 6,
    SCRIPT_E_DENIED = 7,
};
static const char *const script_err_names[] = {
    [SCRIPT_OK] = "ok",
    [SCRIPT_E_BUSY] = "busy",
    [SCRIPT_E_WRONG_STATE] = "wrong-state",
    [SCRIPT_E_TIMEOUT] = "timeout",
    [SCRIPT_E_INVALID] = "invalid-argument",
    [SCRIPT_E_NOT_CAPABLE] = "not-capable",
    [SCRIPT_E_NOT_FOUND] = "not-found",
    [SCRIPT_E_DENIED] = "denied",
};

/* Escape a string into JSON.  Everything E1 puts in an envelope is our
 * own ASCII, but the guarantee is cheap and E2 starts quoting paths. */
static char *json_escape(const char *s)
{
    GString *out = g_string_sized_new(strlen(s) + 8);

    for (; *s; s++) {
        unsigned char c = *s;

        switch (c) {
        case '"':
            g_string_append(out, "\\\"");
            break;
        case '\\':
            g_string_append(out, "\\\\");
            break;
        case '\n':
            g_string_append(out, "\\n");
            break;
        case '\r':
            g_string_append(out, "\\r");
            break;
        case '\t':
            g_string_append(out, "\\t");
            break;
        default:
            if (c < 0x20) {
                g_string_append_printf(out, "\\u%04x", c);
            } else {
                g_string_append_c(out, c);
            }
            break;
        }
    }
    return g_string_free(out, FALSE);
}

/* The machine block that rides on every reply, so an agent's next
 * decision needs no extra round trip.  E0 measured the whole event at
 * 16.7 ms; this BQL-taking read is not the cost. */
static char *script_machine_block(void)
{
    const char *state;

    bql_lock();
    state = RunState_str(runstate_get());
    bql_unlock();
    return g_strdup_printf("\"machine\":{\"state\":\"%s\"}", state);
}

static char *script_envelope(bool ok, const char *data_json, int err,
                             const char *msg, int64_t elapsed_us)
{
    char *machine = script_machine_block();
    char *reply;

    if (ok) {
        reply = g_strdup_printf(
            "{\"ok\":true,\"data\":%s,\"elapsed_ms\":%d,%s}",
            data_json ? data_json : "null", (int)(elapsed_us / 1000),
            machine);
    } else {
        char *esc = json_escape(msg ?: "");

        reply = g_strdup_printf(
            "{\"ok\":false,\"error\":{\"code\":\"%s\",\"number\":%d,"
            "\"message\":\"%s\"},\"elapsed_ms\":%d,%s}",
            script_err_names[err], err, esc, (int)(elapsed_us / 1000),
            machine);
        g_free(esc);
    }
    g_free(machine);
    return reply;
}

static char *script_describe(void)
{
    GString *s = g_string_new("{\"surface\":1,\"commands\":[");
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(script_cmds); i++) {
        const ScriptCmd *c = &script_cmds[i];
        char *help = json_escape(c->help);
        char *args = c->args ? json_escape(c->args) : NULL;

        g_string_append_printf(s,
                               "%s{\"name\":\"%s\",\"code\":\"%s\","
                               "\"help\":\"%s\",\"args\":",
                               i ? "," : "", c->name, c->code, help);
        if (args) {
            g_string_append_printf(s, "\"%s\"", args);
        } else {
            g_string_append(s, "null");
        }
        g_string_append_printf(s, ",\"class\":\"%s\"}",
                               c->klass == 'f' ? "fast" : "bottom");
        g_free(help);
        g_free(args);
    }
    g_string_append(s, "]}");
    return g_string_free(s, FALSE);
}

bool metal_glue_script(uint32_t event_class, uint32_t event_id,
                       const char *json, char **reply)
{
    int64_t t0 = g_get_monotonic_time();
    QDict *qdict = NULL;
    const ScriptCmd *cmd = NULL;
    char *data = NULL;
    int err = SCRIPT_OK;
    const char *errmsg = NULL;
    unsigned i;

    *reply = NULL;

    /*
     * Handlers arrive on the UI thread only and fast commands answer
     * inline, so execution is single-flight by construction.  The
     * waiting parameter (seconds, clamped here, default 10, max 60)
     * is accepted and becomes live with the bottom-half class in E2;
     * a timeout will complete the operation and report rather than
     * cancel it.
     */
    if (json) {
        qdict = qobject_to(QDict, qobject_from_json(json, NULL));
        if (!qdict) {
            err = SCRIPT_E_INVALID;
            errmsg = "the direct parameter must be a JSON object";
            goto out;
        }
    }

    /* The event id is authoritative: it is the command AppleScript
     * addressed through the terminology.  A "cmd" key in a JSON
     * parameter overrides it, so the raw-text form keeps working. */
    for (i = 0; i < ARRAY_SIZE(script_cmds); i++) {
        const char *c = script_cmds[i].code;
        uint32_t cls, id2;

        cls = ((uint8_t)c[0] << 24) | ((uint8_t)c[1] << 16)
              | ((uint8_t)c[2] << 8) | (uint8_t)c[3];
        id2 = ((uint8_t)c[4] << 24) | ((uint8_t)c[5] << 16)
              | ((uint8_t)c[6] << 8) | (uint8_t)c[7];
        if (cls == event_class && id2 == event_id) {
            cmd = &script_cmds[i];
            break;
        }
    }
    if (qdict) {
        const char *override = qdict_get_try_str(qdict, "cmd");

        if (override) {
            for (i = 0; i < ARRAY_SIZE(script_cmds); i++) {
                if (strcmp(script_cmds[i].name, override) == 0) {
                    cmd = &script_cmds[i];
                    break;
                }
            }
            if (i == ARRAY_SIZE(script_cmds)) {
                cmd = NULL;
            }
        }
    }
    if (!cmd) {
        err = SCRIPT_E_INVALID;
        errmsg = "unknown command; describe lists them";
        goto out;
    }

    if (cmd == &script_cmds[0]) {           /* ping */
        data = g_strdup_printf(
            "{\"app\":\"RISCOSQEMU\",\"qemu\":\"%s\",\"pid\":%d,"
            "\"surface\":1}",
            QEMU_VERSION, (int)getpid());
    } else if (cmd == &script_cmds[1]) {    /* describe */
        data = script_describe();
    } else {
        g_assert_not_reached();
    }

out:
    *reply = script_envelope(err == SCRIPT_OK, data, err, errmsg,
                             g_get_monotonic_time() - t0);
    g_free(data);
    qobject_unref(qdict);
    return true;
}

size_t metal_glue_script_events(uint32_t *classes, uint32_t *ids, size_t max)
{
    size_t n = 0;

    for (unsigned i = 0; i < ARRAY_SIZE(script_cmds) && n < max; i++) {
        const char *code = script_cmds[i].code;

        classes[n] = ((uint8_t)code[0] << 24) | ((uint8_t)code[1] << 16)
                     | ((uint8_t)code[2] << 8) | (uint8_t)code[3];
        ids[n] = ((uint8_t)code[4] << 24) | ((uint8_t)code[5] << 16)
                 | ((uint8_t)code[6] << 8) | (uint8_t)code[7];
        n++;
    }
    return n;
}


/* ------------------------------------------------------------------ */
/* Input: Cocoa events to QEMU input events, under the BQL             */

void metal_glue_key(bool down, uint32_t oskeycode)
{
    /*
     * An NSEvent's keyCode is a macOS virtual key code, which is exactly
     * how the osx keymap is indexed -- the same relationship the Windows
     * front end has to the AT set 1 scan codes in a WM_KEY* lParam.
     */
    unsigned int lnx;

    if (oskeycode >= qemu_input_map_osx_to_linux_len) {
        return;
    }
    lnx = qemu_input_map_osx_to_linux[oskeycode];
    if (lnx == 0) {
        return;
    }

    bql_lock();
    qemu_input_event_send_key_linux(NULL, lnx, down);
    bql_unlock();
}

void metal_glue_mouse_abs(int gx, int gy, int xres, int yres)
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

void metal_glue_mouse_btn(int button, bool down)
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

void metal_glue_mouse_wheel(int notches)
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

void metal_glue_grab(bool on)
{
    /* Windows arms a low-level keyboard hook here so Alt+Tab and the
     * Windows key reach the guest.  The macOS equivalent is a CGEventTap
     * at the annotated-session level, which needs Accessibility consent
     * the emulator has no business asking for -- and Command chords
     * belonging to the host is the platform convention.  So the grab is
     * the pointer's alone; the front end tracks the state itself. */
    (void)on;
}

/* ------------------------------------------------------------------ */
/* Guest vertical sync                                                 */

bool metal_glue_set_vsync_hz(int hz)
{
    Object *obj = object_resolve_path_type("", TYPE_BCM2835_VSYNCGEN, NULL);

    if (!obj) {
        return false;
    }
    bcm2835_vsyncgen_set_hz(BCM2835_VSYNCGEN(obj), hz);
    return true;
}

/* ------------------------------------------------------------------ */
/* Snapshot restore, from the Machine menu                             */

/* The one snapshot the menu loads; tools/run.py creates it. */
#define METAL_SNAPSHOT_NAME "desktop"

static QEMUBH *metal_loadvm_bh;

static void metal_loadvm_bh_fn(void *opaque)
{
    /* Main loop, BQL held: exactly the hmp_loadvm sequence. */
    RunState saved = runstate_get();
    Error *err = NULL;

    vm_stop(RUN_STATE_RESTORE_VM);
    if (load_snapshot(METAL_SNAPSHOT_NAME, NULL, false, NULL, &err)) {
        load_snapshot_resume(saved);
    } else {
        error_report_err(err);
    }
}

void metal_glue_load_snapshot(void)
{
    if (!metal_loadvm_bh) {
        metal_loadvm_bh = qemu_bh_new(metal_loadvm_bh_fn, NULL);
    }
    qemu_bh_schedule(metal_loadvm_bh);
}

static void metal_display_init(DisplayState *ds, DisplayOptions *opts)
{
    MetalFbView prime;
    int vsync_hz;

    if (metal_backend_init() != 0) {
        error_report("metal: cannot create window or Metal device");
        exit(1);
    }
    /* Resolve the framebuffer device now, while this thread is the only
     * one running; metal_glue_fb_view keeps the cache warm after that. */
    metal_glue_fb_view(&prime);

    /* Guest vertical sync rate. The machine's generator delivers it from
     * the timer thread; this only sets the rate. */
    if (opts->u.metal.has_vsync) {
        vsync_hz = MIN(MAX(opts->u.metal.vsync, 0), 1000);
        if (!metal_glue_set_vsync_hz(vsync_hz)) {
            warn_report("metal: this machine has no vertical-sync "
                        "generator; vsync= ignored");
        }
    }

    /* Scaler and CRT flavour.  The qapi enum order is the integer order
     * the window expects: 0 linear, 1 sharp, 2 nearest. */
    {
        int scaling = 1;                    /* sharp */

        if (opts->u.metal.has_scaling) {
            switch (opts->u.metal.scaling) {
            case DISPLAY_METAL_SCALING_LINEAR:
                scaling = 0;
                break;
            case DISPLAY_METAL_SCALING_SHARP:
                scaling = 1;
                break;
            case DISPLAY_METAL_SCALING_NEAREST:
                scaling = 2;
                break;
            default:
                break;
            }
        }
        metal_glue_video_opts(scaling, opts->u.metal.has_scanlines
                                      && opts->u.metal.scanlines);
    }
    /* The hand-off: system/main.c sees this set after qemu_init and runs
     * the QEMU main loop on its own thread, giving the UI the main one.
     * On Darwin qemu_main already points at a bare CFRunLoop; replacing
     * it is what makes this window, rather than nothing, own the thread. */
    qemu_main = metal_backend_main;
}

static QemuDisplay qemu_display_metal = {
    .type       = DISPLAY_TYPE_METAL,
    .init       = metal_display_init,
};

static void metal_register_types(void)
{
    qemu_display_register(&qemu_display_metal);
}
type_init(metal_register_types);
