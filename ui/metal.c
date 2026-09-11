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
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "system/reset.h"
#include "block/snapshot.h"
#include "standard-headers/linux/input-event-codes.h"
#include <zlib.h>

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
    { "state", "MQemStat",
      "Machine counters: run state, uptime, frame gen and count, input and event counters, display mode.",
      NULL, 'f' },
    { "video", "MQemVOpt",
      "Set video options; omitted keys are unchanged. Reply is as applied.",
      "scaling int (0 linear, 1 sharp, 2 nearest), scanlines bool, vsync int Hz, waiting int s",
      'f' },
    { "pause", "MQemPaus",
      "Stop the virtual CPUs; the reply envelope reports the paused state.",
      NULL, 'b' },
    { "resume", "MQemResu",
      "Continue the virtual CPUs after a pause.",
      NULL, 'b' },
    { "reset", "MQemRset",
      "Reset the machine. Destructive: needs dangerous true.",
      "dangerous bool", 'b' },
    { "poweroff", "MQemPowr",
      "Power off the way the window close does, disc written back. Destructive: needs dangerous true.",
      "dangerous bool", 'b' },
    { "savevm", "MQemSnSv",
      "Save an internal snapshot on the machine disc. Overwriting an existing name needs dangerous true.",
      "name str, overwrite bool, dangerous bool, waiting int s", 'b' },
    { "loadvm", "MQemSnLd",
      "Load an internal snapshot and resume: the Machine menu path, generalised.",
      "name str, waiting int s", 'b' },
    { "listvm", "MQemSnLs",
      "List the internal snapshots on the machine disc.",
      NULL, 'b' },
    { "screendump", "MQemDump",
      "Write the guest framebuffer as PNG into the app support directory: the truth, no decode.",
      "name str, waiting int s", 'b' },
    { "screenshot", "MQemShot",
      "Write the decoded Metal surface as PNG into the app support directory.",
      "name str", 'f' },
    { "key", "MQemKeyD",
      "One key event: a macOS virtual keycode, down or up.",
      "keycode int, down bool", 'f' },
    { "type", "MQemType",
      "Type ASCII text through the US keymap, shifted punctuation included.",
      "text str", 'f' },
    { "mouse", "MQemMous",
      "Move the pointer to guest pixel coordinates on the current screen.",
      "x int, y int", 'f' },
    { "click", "MQemClkM",
      "Press a mouse button (0 left, 1 middle, 2 right); without down, the full click.",
      "button int, down bool", 'f' },
    { "wheel", "MQemWhl ",
      "Scroll notches; positive is away from the user.",
      "notches int", 'f' },
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
 * 16.7 ms; this BQL-taking read is not the cost.  The framebuffer view
 * is gathered after the lock, the way the render loop gathers it (the
 * mapping is owned by this thread); frame_gen and frames are the two
 * numbers that answer did anything change. */
static char *script_machine_block(void)
{
    MetalFbView v;
    const char *state;
    uint64_t uptime;
    int have_fb;

    bql_lock();
    state = RunState_str(runstate_get());
    uptime = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bql_unlock();

    have_fb = metal_glue_fb_view(&v);
    if (have_fb) {
        return g_strdup_printf(
            "\"machine\":{\"state\":\"%s\",\"uptime_ns\":%" PRIu64 ","
            "\"frame_gen\":%u,\"frames\":%u,\"mode\":\"%ux%ux%u\"}",
            state, uptime, v.generation, metal_ui_frame_count(),
            v.xres, v.yres, v.bpp);
    }
    return g_strdup_printf(
        "\"machine\":{\"state\":\"%s\",\"uptime_ns\":%" PRIu64 ","
        "\"frame_gen\":0,\"frames\":%u,\"mode\":null}",
        state, uptime, metal_ui_frame_count());
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

/* ------------------------------------------------------------------ */
/* E2: the control commands                                            */

/* Every event the surface answers, for the state command. */
static uint32_t script_ae_events;

/* Input injections, counted where they enter QEMU so the count covers
 * both the window's events and the surface's. */
static struct {
    uint32_t keys;
    uint32_t mouse_moves;
    uint32_t mouse_buttons;
} cnt;

/* Argument readers over the direct parameter's JSON object.  A missing
 * key is simply the default; type mismatches are the qdict helpers'
 * defaults too, which keeps the surface forgiving in the way the wire
 * lessons said agents need. */
static bool arg_bool(const QDict *q, const char *key, bool dflt)
{
    return q ? qdict_get_try_bool(q, key, dflt) : dflt;
}

static int64_t arg_int(const QDict *q, const char *key, int64_t dflt)
{
    return q ? qdict_get_try_int(q, key, dflt) : dflt;
}

static const char *arg_str(const QDict *q, const char *key)
{
    return q ? qdict_get_try_str(q, key) : NULL;
}

/*
 * Writes are confined (SCRIPTING.md section 7): screenshots and
 * screendumps land under ~/Library/Application Support/RISCOSQEMU only.
 * A caller-supplied name is a leaf, never a path: letters, digits,
 * dot, underscore and hyphen, and it may not start with a dot.  NULL
 * or empty picks the next numbered name.  Returns a script error code
 * with *errmsg set, or SCRIPT_OK with the full path in buf.
 */
static int script_out_path(const char *name, const char *prefix,
                           char *buf, size_t buflen, const char **errmsg)
{
    const char *home = g_get_home_dir();
    char dir[PATH_MAX];

    if (!home) {
        *errmsg = "no home directory for the app support directory";
        return SCRIPT_E_NOT_CAPABLE;
    }
    snprintf(dir, sizeof(dir), "%s/Library/Application Support/RISCOSQEMU",
             home);
    if (g_mkdir_with_parents(dir, 0700) != 0 && errno != EEXIST) {
        *errmsg = "cannot create the app support directory";
        return SCRIPT_E_NOT_CAPABLE;
    }

    if (name && *name) {
        const char *c;

        for (c = name; *c; c++) {
            if (!g_ascii_isalnum(*c) && *c != '.' && *c != '_'
                && *c != '-') {
                *errmsg = "the name must be a plain file name, not a path";
                return SCRIPT_E_DENIED;
            }
        }
        if (name[0] == '.') {
            *errmsg = "the name may not start with a dot";
            return SCRIPT_E_DENIED;
        }
        snprintf(buf, buflen, "%s/%s.png", dir, name);
        return SCRIPT_OK;
    }

    for (unsigned n = 1; n < 100000; n++) {
        snprintf(buf, buflen, "%s/%s-%05u.png", dir, prefix, n);
        if (access(buf, F_OK) != 0) {
            return SCRIPT_OK;
        }
    }
    *errmsg = "no free screenshot number";
    return SCRIPT_E_NOT_CAPABLE;
}

/* A minimal RGB8 PNG writer: filter byte 0 per scanline, one IDAT.
 * The front end's Python counterparts proved this layout; zlib is
 * already a QEMU dependency. */
static bool png_chunk(FILE *f, const char type[4], const void *data,
                      size_t len)
{
    uint32_t be = cpu_to_be32(len);
    uint32_t crc = crc32(crc32(0, Z_NULL, 0), (const Bytef *)type, 4);
    bool ok;

    if (data && len) {
        crc = crc32(crc, data, len);
    }
    ok = fwrite(&be, 4, 1, f) == 1;
    ok &= fwrite(type, 4, 1, f) == 1;
    if (data && len) {
        ok &= fwrite(data, len, 1, f) == 1;
    }
    be = cpu_to_be32(crc);
    ok &= fwrite(&be, 4, 1, f) == 1;
    return ok;
}

static bool script_png_write(const char *path, uint32_t w, uint32_t h,
                             const uint8_t *rgb)
{
    size_t rowlen = (size_t)w * 3;
    size_t rawlen = (size_t)h * (rowlen + 1);
    uint8_t *raw = g_malloc(rawlen);
    uLongf clen = compressBound(rawlen);
    uint8_t *comp = g_malloc(clen);
    uint8_t ihdr[13];
    FILE *f;
    bool ok;

    for (uint32_t y = 0; y < h; y++) {
        raw[y * (rowlen + 1)] = 0;         /* filter: none */
        memcpy(&raw[y * (rowlen + 1) + 1], &rgb[(size_t)y * rowlen], rowlen);
    }
    if (compress2(comp, &clen, raw, rawlen, 6) != Z_OK) {
        g_free(comp);
        g_free(raw);
        return false;
    }
    stl_be_p(&ihdr[0], w);
    stl_be_p(&ihdr[4], h);
    ihdr[8] = 8;                            /* bit depth */
    ihdr[9] = 2;                            /* colour type: truecolor RGB */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;

    f = fopen(path, "wb");
    if (!f) {
        g_free(comp);
        g_free(raw);
        return false;
    }
    ok = fwrite("\x89PNG\r\n\x1a\n", 8, 1, f) == 1
         && png_chunk(f, "IHDR", ihdr, sizeof(ihdr))
         && png_chunk(f, "IDAT", comp, clen)
         && png_chunk(f, "IEND", NULL, 0);
    ok &= fclose(f) == 0;
    g_free(comp);
    g_free(raw);
    return ok;
}

/*
 * The screendump: the guest framebuffer, read from the device the same
 * way the decode shader reads it (offsets only when the viewport
 * exceeds the physical screen; palette words 0x00BBGGRR at the VideoCore
 * base for the palettised modes).  Runs inside the bottom half, BQL
 * held; the reply is the path and the config generation it captured.
 */
static BCM2835FBState *script_fb;

static int script_screendump(const char *name, char **data, char **errmsg)
{
    BCM2835FBConfig cfg;
    uint32_t gen, pitch, bypp, rowlen, xo, yo;
    uint8_t *raw = NULL, *pal = NULL, *rgb;
    char path[PATH_MAX];
    g_autofree char *epath = NULL;
    int rc;

    if (!script_fb) {
        Object *obj = object_resolve_path_type("", TYPE_BCM2835_FB, NULL);

        if (!obj) {
            *errmsg = g_strdup("no framebuffer device");
            return SCRIPT_E_NOT_CAPABLE;
        }
        script_fb = BCM2835_FB(obj);
    }
    gen = bcm2835_fb_get_config(script_fb, &cfg);
    if (!cfg.xres || !cfg.yres) {
        *errmsg = g_strdup("no framebuffer mode yet");
        return SCRIPT_E_NOT_CAPABLE;
    }
    bypp = (cfg.bpp + 7) / 8;
    pitch = bcm2835_fb_get_pitch(&cfg);
    if (cfg.xres_virtual > cfg.xres || cfg.yres_virtual > cfg.yres) {
        xo = cfg.xoffset;
        yo = cfg.yoffset;
    } else {
        xo = yo = 0;
    }

    raw = g_malloc_n(cfg.yres, rowlen = cfg.xres * bypp);
    for (uint32_t y = 0; y < cfg.yres; y++) {
        address_space_read(&script_fb->dma_as,
                           cfg.base + ((uint64_t)(yo + y) * pitch
                                       + (uint64_t)xo * bypp),
                           MEMTXATTRS_UNSPECIFIED,
                           raw + (uint64_t)y * rowlen, rowlen);
    }
    if (cfg.bpp <= 8) {
        pal = g_malloc(256 * 4);
        address_space_read(&script_fb->dma_as, script_fb->vcram_base,
                           MEMTXATTRS_UNSPECIFIED, pal, 256 * 4);
    }

    rgb = g_malloc_n((size_t)cfg.xres * cfg.yres, 3);
    for (uint32_t y = 0; y < cfg.yres; y++) {
        const uint8_t *row = raw + (uint64_t)y * rowlen;

        for (uint32_t x = 0; x < cfg.xres; x++) {
            uint8_t *o = &rgb[((uint64_t)y * cfg.xres + x) * 3];
            uint32_t r = 0, g = 0, b = 0;

            if (cfg.bpp == 32) {
                r = row[x * 4 + 0];
                g = row[x * 4 + 1];
                b = row[x * 4 + 2];
                if (cfg.pixo == 0) {
                    uint32_t t = r; r = b; b = t;    /* BGR order */
                }
            } else if (cfg.bpp == 24) {
                r = row[x * 3 + 0];
                g = row[x * 3 + 1];
                b = row[x * 3 + 2];
            } else if (cfg.bpp == 16) {
                uint32_t w16 = row[x * 2] | ((uint32_t)row[x * 2 + 1] << 8);

                r = ((w16 >> 11) & 31) * 255 / 31;
                g = ((w16 >> 5) & 63) * 255 / 63;
                b = (w16 & 31) * 255 / 31;
            } else {
                uint32_t byte = row[x * cfg.bpp / 8];
                uint32_t idx = (byte >> ((x * cfg.bpp) & 7))
                               & ((1u << cfg.bpp) - 1);

                r = pal[idx * 4 + 0];
                g = pal[idx * 4 + 1];
                b = pal[idx * 4 + 2];
            }
            o[0] = r;
            o[1] = g;
            o[2] = b;
        }
    }
    g_free(raw);
    g_free(pal);

    rc = script_out_path(name, "screendump", path, sizeof(path),
                         (const char **)errmsg);
    if (rc != SCRIPT_OK) {
        g_free(rgb);
        return rc;               /* *errmsg is a literal here */
    }
    if (!script_png_write(path, cfg.xres, cfg.yres, rgb)) {
        g_free(rgb);
        *errmsg = g_strdup("the PNG could not be written");
        return SCRIPT_E_NOT_CAPABLE;
    }
    g_free(rgb);
    epath = json_escape(path);
    *data = g_strdup_printf("{\"path\":\"%s\",\"frame_gen\":%u}",
                            epath, gen);
    return SCRIPT_OK;
}

/* ------------------------------------------------------------------ */
/* Typing: ASCII to linux key codes, the same entry point the osx      */
/* keymap feeds.  US layout spelling for the shifted punctuation,      */
/* documented in the command's help.                                   */

static void script_send_linux(unsigned lnx, bool down)
{
    cnt.keys++;                 /* typed text counts as key events too */
    bql_lock();
    qemu_input_event_send_key_linux(NULL, lnx, down);
    bql_unlock();
}

static int script_key_for_char(char c, unsigned *code, bool *shift)
{
    *shift = false;
    if (c >= 'a' && c <= 'z') {
        *code = KEY_A + (c - 'a');
        return 0;
    }
    if (c >= 'A' && c <= 'Z') {
        *code = KEY_A + (c - 'A');
        *shift = true;
        return 0;
    }
    if (c >= '1' && c <= '9') {
        *code = KEY_1 + (c - '1');
        return 0;
    }
    switch (c) {
    case '0': *code = KEY_0; return 0;
    case ' ': *code = KEY_SPACE; return 0;
    case '\n': case '\r': *code = KEY_ENTER; return 0;
    case '\t': *code = KEY_TAB; return 0;
    case '!': *code = KEY_1; *shift = true; return 0;
    case '@': *code = KEY_2; *shift = true; return 0;
    case '#': *code = KEY_3; *shift = true; return 0;
    case '$': *code = KEY_4; *shift = true; return 0;
    case '%': *code = KEY_5; *shift = true; return 0;
    case '^': *code = KEY_6; *shift = true; return 0;
    case '&': *code = KEY_7; *shift = true; return 0;
    case '*': *code = KEY_8; *shift = true; return 0;
    case '(': *code = KEY_9; *shift = true; return 0;
    case ')': *code = KEY_0; *shift = true; return 0;
    case '-': *code = KEY_MINUS; return 0;
    case '_': *code = KEY_MINUS; *shift = true; return 0;
    case '=': *code = KEY_EQUAL; return 0;
    case '+': *code = KEY_EQUAL; *shift = true; return 0;
    case '[': *code = KEY_LEFTBRACE; return 0;
    case '{': *code = KEY_LEFTBRACE; *shift = true; return 0;
    case ']': *code = KEY_RIGHTBRACE; return 0;
    case '}': *code = KEY_RIGHTBRACE; *shift = true; return 0;
    case '\\': *code = KEY_BACKSLASH; return 0;
    case '|': *code = KEY_BACKSLASH; *shift = true; return 0;
    case ';': *code = KEY_SEMICOLON; return 0;
    case ':': *code = KEY_SEMICOLON; *shift = true; return 0;
    case '\'': *code = KEY_APOSTROPHE; return 0;
    case '"': *code = KEY_APOSTROPHE; *shift = true; return 0;
    case '`': *code = KEY_GRAVE; return 0;
    case '~': *code = KEY_GRAVE; *shift = true; return 0;
    case ',': *code = KEY_COMMA; return 0;
    case '<': *code = KEY_COMMA; *shift = true; return 0;
    case '.': *code = KEY_DOT; return 0;
    case '>': *code = KEY_DOT; *shift = true; return 0;
    case '/': *code = KEY_SLASH; return 0;
    case '?': *code = KEY_SLASH; *shift = true; return 0;
    default:
        return -1;
    }
}

static char *cmd_type(const QDict *q, int *err, const char **errmsg)
{
    const char *text = arg_str(q, "text");
    unsigned typed = 0;

    if (!text || !*text) {
        *err = SCRIPT_E_INVALID;
        *errmsg = "text is required";
        return NULL;
    }
    if (strlen(text) > 256) {
        *err = SCRIPT_E_INVALID;
        *errmsg = "text is capped at 256 characters";
        return NULL;
    }
    for (const char *c = text; *c; c++) {
        unsigned code;
        bool shift;

        if (script_key_for_char(*c, &code, &shift)) {
            *err = SCRIPT_E_INVALID;
            *errmsg = "only ASCII printable text is supported in v1";
            return NULL;
        }
        if (shift) {
            script_send_linux(KEY_LEFTSHIFT, true);
        }
        script_send_linux(code, true);
        script_send_linux(code, false);
        if (shift) {
            script_send_linux(KEY_LEFTSHIFT, false);
        }
        typed++;
    }
    return g_strdup_printf("{\"typed\":%u}", typed);
}

/* ------------------------------------------------------------------ */
/* The fast commands' replies                                          */

static char *cmd_state(void)
{
    MetalFbView v;
    const char *state;
    uint64_t uptime;
    char mode[32];
    int have_fb;

    bql_lock();
    state = RunState_str(runstate_get());
    uptime = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bql_unlock();
    have_fb = metal_glue_fb_view(&v);
    if (have_fb) {
        snprintf(mode, sizeof(mode), "\"%ux%ux%u\"", v.xres, v.yres, v.bpp);
    } else {
        strcpy(mode, "null");
    }
    return g_strdup_printf(
        "{\"state\":\"%s\",\"uptime_ns\":%" PRIu64 ",\"frame_gen\":%u,"
        "\"frames\":%u,\"mode\":%s,\"keys\":%u,\"mouse_moves\":%u,"
        "\"mouse_buttons\":%u,\"ae_events\":%u}",
        state, uptime, have_fb ? v.generation : 0, metal_ui_frame_count(),
        mode, cnt.keys, cnt.mouse_moves, cnt.mouse_buttons,
        script_ae_events);
}

static char *cmd_video(const QDict *q, int *err, const char **errmsg)
{
    int scaling, cur_scaling, vsync;
    bool scanlines, cur_scan;

    metal_ui_video_opts(&cur_scaling, &cur_scan);
    scaling = q ? qdict_get_try_int(q, "scaling", cur_scaling) : cur_scaling;
    scanlines = q ? qdict_get_try_bool(q, "scanlines", cur_scan) : cur_scan;
    if (scaling < 0 || scaling > 2) {
        *err = SCRIPT_E_INVALID;
        *errmsg = "scaling is 0 linear, 1 sharp or 2 nearest";
        return NULL;
    }
    metal_glue_video_opts(scaling, scanlines ? 1 : 0);

    vsync = q ? qdict_get_try_int(q, "vsync", -1) : -1;
    if (vsync != -1) {
        bool ok;

        if (vsync < 0 || vsync > 1000) {
            *err = SCRIPT_E_INVALID;
            *errmsg = "vsync is 0 to 1000 Hz";
            return NULL;
        }
        bql_lock();
        ok = metal_glue_set_vsync_hz(vsync);
        bql_unlock();
        if (!ok) {
            *err = SCRIPT_E_NOT_CAPABLE;
            *errmsg = "this machine has no vertical-sync generator";
            return NULL;
        }
        return g_strdup_printf("{\"scaling\":%d,\"scanlines\":%s,"
                               "\"vsync_hz\":%d}", scaling,
                               scanlines ? "true" : "false", vsync);
    }
    return g_strdup_printf("{\"scaling\":%d,\"scanlines\":%s,"
                           "\"vsync_hz\":null}", scaling,
                           scanlines ? "true" : "false");
}

/* ------------------------------------------------------------------ */
/* The bottom-half class: schedule on the main loop, wait on the       */
/* semaphore (SCRIPTING.md section 4's threading rules).  The waiting  */
/* UI thread holds no lock; the bottom half never calls into it.       */

static struct {
    QEMUBH *bh;
    QemuSemaphore sem;
    bool armed;                 /* scheduled or running */
    /* set by the UI thread before scheduling */
    const ScriptCmd *cmd;
    QDict *args;                /* ref held until the next reset */
    /* filled by the bottom half */
    char *data;                 /* heap; owned here until collected */
    char *errmsg;               /* heap; owned here until collected */
    int err;
} sbh;

static void script_bh_fn(void *opaque)
{
    const char *name = sbh.cmd->name;
    const QDict *args = sbh.args;
    char *data = NULL, *msg = NULL;
    Error *verr = NULL;
    int err = SCRIPT_OK;

    /* Main loop, BQL held: the hmp/qmp sequences, verbatim. */
    if (!strcmp(name, "pause")) {
        vm_stop(RUN_STATE_PAUSED);
        data = g_strdup("{\"state\":\"paused\"}");
    } else if (!strcmp(name, "resume")) {
        if (runstate_check(RUN_STATE_SHUTDOWN)) {
            err = SCRIPT_E_WRONG_STATE;
            msg = g_strdup("the machine is powering off");
        } else {
            vm_start();
            data = g_strdup_printf("{\"state\":\"%s\"}",
                                   RunState_str(runstate_get()));
        }
    } else if (!strcmp(name, "reset")) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_HOST_QMP_SYSTEM_RESET);
        data = g_strdup("{\"state\":\"resetting\"}");
    } else if (!strcmp(name, "poweroff")) {
        metal_backend_request_shutdown();
        data = g_strdup("{\"powering_off\":true}");
    } else if (!strcmp(name, "savevm")) {
        const char *sn = arg_str(args, "name");

        if (!sn || !*sn) {
            err = SCRIPT_E_INVALID;
            msg = g_strdup("a snapshot name is required");
        } else if (!save_snapshot(sn, arg_bool(args, "overwrite", false),
                                  NULL, false, NULL, &verr)) {
            err = SCRIPT_E_INVALID;
            msg = g_strdup(error_get_pretty(verr));
            error_free(verr);
        } else {
            data = g_strdup_printf("{\"saved\":true,\"name\":\"%s\"}", sn);
        }
    } else if (!strcmp(name, "loadvm")) {
        const char *sn = arg_str(args, "name");

        if (!sn || !*sn) {
            err = SCRIPT_E_INVALID;
            msg = g_strdup("a snapshot name is required");
        } else {
            RunState saved = runstate_get();

            vm_stop(RUN_STATE_RESTORE_VM);
            if (load_snapshot(sn, NULL, false, NULL, &verr)) {
                load_snapshot_resume(saved);
                data = g_strdup_printf("{\"loaded\":true,\"name\":\"%s\"}",
                                       sn);
            } else {
                const char *pretty = error_get_pretty(verr);

                err = strstr(pretty, "snapshot") || strstr(pretty, "exist")
                          ? SCRIPT_E_NOT_FOUND : SCRIPT_E_INVALID;
                msg = g_strdup(pretty);
                error_free(verr);
            }
        }
    } else if (!strcmp(name, "listvm")) {
        BlockDriverState *bs = bdrv_all_find_vmstate_bs(NULL, false, NULL,
                                                        &verr);

        if (!bs) {
            if (verr) {
                error_free(verr);
            }
            err = SCRIPT_E_NOT_CAPABLE;
            msg = g_strdup("no block device carries machine state");
        } else {
            QEMUSnapshotInfo *tab = NULL;
            int n = bdrv_snapshot_list(bs, &tab);

            if (n < 0) {
                err = SCRIPT_E_INVALID;
                msg = g_strdup("cannot list the snapshots on the disc");
            } else {
                GString *s = g_string_new("[");

                for (int i = 0; i < n; i++) {
                    g_string_append_printf(s,
                        "%s{\"name\":\"%s\",\"id\":\"%s\",\"date_sec\":%u,"
                        "\"vm_state_size\":%" PRIu64 "}",
                        i ? "," : "", tab[i].name, tab[i].id_str,
                        tab[i].date_sec, tab[i].vm_state_size);
                }
                g_string_append(s, "]");
                data = g_string_free(s, FALSE);
                g_free(tab);
            }
        }
    } else if (!strcmp(name, "screendump")) {
        err = script_screendump(arg_str(args, "name"), &data, &msg);
    } else {
        g_assert_not_reached();
    }

    sbh.data = data;
    sbh.errmsg = msg;
    sbh.err = err;
    sbh.armed = false;
    qemu_sem_post(&sbh.sem);
}

/* Returns the reply data, or NULL with the error outputs set.
 * A timeout leaves the bottom half running and the next command busy. */
static char *script_run_bh(const ScriptCmd *c, QDict *qdict, int *err,
                           char **errmsg_dyn, const char **errmsg,
                           int64_t waiting_s)
{
    char *data;

    if (sbh.armed) {
        *err = SCRIPT_E_BUSY;
        *errmsg = "another command is still in flight";
        return NULL;
    }
    if (!sbh.bh) {
        qemu_sem_init(&sbh.sem, 0);
        sbh.bh = qemu_bh_new(script_bh_fn, NULL);
    }
    /* drain a completion that arrived after an earlier timeout; only
     * now is the semaphore certain to be initialised (the first ever
     * call used to reach this drain first and abort on the zeroed
     * mutex inside, which one crashed pause taught us) */
    while (qemu_sem_timedwait(&sbh.sem, 0) == 0) {
    }
    if (sbh.args) {
        qobject_unref(sbh.args);
        sbh.args = NULL;
    }
    g_free(sbh.data);
    sbh.data = NULL;
    g_free(sbh.errmsg);
    sbh.errmsg = NULL;
    sbh.err = SCRIPT_OK;

    sbh.cmd = c;
    sbh.args = qdict;
    if (qdict) {
        qobject_ref(qdict);
    }
    sbh.armed = true;
    qemu_bh_schedule(sbh.bh);

    if (qemu_sem_timedwait(&sbh.sem, (int)MIN(waiting_s * 1000, INT_MAX))
        != 0) {
        *err = SCRIPT_E_TIMEOUT;
        *errmsg = "waiting expired; the operation continues";
        return NULL;
    }
    if (sbh.err != SCRIPT_OK) {
        *err = sbh.err;
        *errmsg_dyn = sbh.errmsg;
        sbh.errmsg = NULL;
        return NULL;
    }
    data = sbh.data;
    sbh.data = NULL;
    return data;
}

bool metal_glue_script(uint32_t event_class, uint32_t event_id,
                       const char *json, char **reply)
{
    int64_t t0 = g_get_monotonic_time();
    QDict *qdict = NULL;
    const ScriptCmd *cmd = NULL;
    char *data = NULL;
    char *errmsg_dyn = NULL;
    int err = SCRIPT_OK;
    const char *errmsg = NULL;
    int64_t waiting;
    unsigned i;

    *reply = NULL;
    script_ae_events++;

    /*
     * The standard quit event routes to the clean power off, the same
     * path the window close takes (never -[NSApp terminate:], for the
     * reason quitAction records).  It is not in the table: AppleScript
     * addresses it through Apple's own terminology.
     */
    if (event_class == 0x61657674u && event_id == 0x71756974u) {
        metal_backend_request_shutdown();
        data = g_strdup("{\"quitting\":true}");
        goto out;
    }

    /*
     * Handlers arrive on the UI thread only and fast commands answer
     * inline, so execution is single-flight by construction.  The
     * bottom-half class waits on its semaphore for up to waiting
     * seconds (default 10, clamped to 1 to 60); a timeout completes
     * the operation and reports rather than cancel it, and the next
     * command is busy until it lands.
     */
    if (json) {
        qdict = qobject_to(QDict, qobject_from_json(json, NULL));
        if (!qdict) {
            err = SCRIPT_E_INVALID;
            errmsg = "the direct parameter must be a JSON object";
            goto out;
        }
    }
    waiting = arg_int(qdict, "waiting", 10);
    waiting = MIN(MAX(waiting, 1), 60);

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

    if (!strcmp(cmd->name, "ping")) {
        data = g_strdup_printf(
            "{\"app\":\"RISCOSQEMU\",\"qemu\":\"%s\",\"pid\":%d,"
            "\"surface\":1}",
            QEMU_VERSION, (int)getpid());
    } else if (!strcmp(cmd->name, "describe")) {
        data = script_describe();
    } else if (!strcmp(cmd->name, "state")) {
        data = cmd_state();
    } else if (!strcmp(cmd->name, "video")) {
        data = cmd_video(qdict, &err, &errmsg);
    } else if (!strcmp(cmd->name, "screenshot")) {
        char path[PATH_MAX];
        MetalFbView v;
        int have_fb = metal_glue_fb_view(&v);
        int rc = script_out_path(arg_str(qdict, "name"), "screenshot",
                                 path, sizeof(path), &errmsg);

        if (rc != SCRIPT_OK) {
            err = rc;
        } else if (!have_fb) {
            err = SCRIPT_E_NOT_CAPABLE;
            errmsg = "no framebuffer to capture yet";
        } else if (!metal_ui_screenshot(path)) {
            err = SCRIPT_E_NOT_CAPABLE;
            errmsg = "the PNG could not be written";
        } else {
            g_autofree char *epath = json_escape(path);

            data = g_strdup_printf("{\"path\":\"%s\",\"frame_gen\":%u}",
                                   epath, v.generation);
        }
    } else if (!strcmp(cmd->name, "key")) {
        int64_t keycode = arg_int(qdict, "keycode", -1);

        if (keycode < 0 || keycode > 127) {
            err = SCRIPT_E_INVALID;
            errmsg = "keycode is a macOS virtual key code, 0 to 127";
        } else {
            metal_glue_key(arg_bool(qdict, "down", true), keycode);
            data = g_strdup("{\"sent\":true}");
        }
    } else if (!strcmp(cmd->name, "type")) {
        data = cmd_type(qdict, &err, &errmsg);
    } else if (!strcmp(cmd->name, "mouse")) {
        MetalFbView v;
        int64_t x = arg_int(qdict, "x", INT64_MIN);
        int64_t y = arg_int(qdict, "y", INT64_MIN);

        if (x == INT64_MIN || y == INT64_MIN) {
            err = SCRIPT_E_INVALID;
            errmsg = "x and y are required, guest pixels";
        } else if (!metal_glue_fb_view(&v)) {
            err = SCRIPT_E_NOT_CAPABLE;
            errmsg = "no screen to move on yet";
        } else {
            metal_glue_mouse_abs(x, y, v.xres, v.yres);
            data = g_strdup_printf("{\"moved\":true,\"x\":%d,\"y\":%d}",
                                   (int)x, (int)y);
        }
    } else if (!strcmp(cmd->name, "click")) {
        int64_t button = arg_int(qdict, "button", 0);
        bool full = qdict ? !qdict_haskey(qdict, "down") : true;

        if (button < 0 || button > 2) {
            err = SCRIPT_E_INVALID;
            errmsg = "button is 0 left, 1 middle or 2 right";
        } else if (full) {
            metal_glue_mouse_btn(button, true);
            metal_glue_mouse_btn(button, false);
            data = g_strdup_printf("{\"clicked\":true,\"button\":%d}",
                                   (int)button);
        } else {
            metal_glue_mouse_btn(button, arg_bool(qdict, "down", true));
            data = g_strdup_printf("{\"clicked\":true,\"button\":%d}",
                                   (int)button);
        }
    } else if (!strcmp(cmd->name, "wheel")) {
        int64_t notches = arg_int(qdict, "notches", 1);

        if (notches == 0) {
            err = SCRIPT_E_INVALID;
            errmsg = "notches is nonzero; positive is away from the user";
        } else {
            metal_glue_mouse_wheel(notches);
            data = g_strdup_printf("{\"scrolled\":%d}", (int)notches);
        }
    } else if (!strcmp(cmd->name, "reset") || !strcmp(cmd->name, "poweroff")) {
        if (!arg_bool(qdict, "dangerous", false)) {
            err = SCRIPT_E_DENIED;
            errmsg = "set dangerous true first; the gate is per command";
        }
    } else if (!strcmp(cmd->name, "savevm")) {
        if (arg_bool(qdict, "overwrite", false)
            && !arg_bool(qdict, "dangerous", false)) {
            err = SCRIPT_E_DENIED;
            errmsg = "overwrite needs dangerous true; the gate is per command";
        }
    } else {
        /* the rest of the table is the bottom-half class */
        data = script_run_bh(cmd, qdict, &err, &errmsg_dyn, &errmsg,
                             waiting);
    }

    if (err == SCRIPT_OK && !data && !errmsg) {
        /* a gated command that passed its gate falls through to here */
        data = script_run_bh(cmd, qdict, &err, &errmsg_dyn, &errmsg,
                             waiting);
    }

out:
    *reply = script_envelope(err == SCRIPT_OK, data, err,
                             errmsg_dyn ?: errmsg,
                             g_get_monotonic_time() - t0);
    g_free(errmsg_dyn);
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
    cnt.keys++;

    bql_lock();
    qemu_input_event_send_key_linux(NULL, lnx, down);
    bql_unlock();
}

void metal_glue_mouse_abs(int gx, int gy, int xres, int yres)
{
    if (xres < 2 || yres < 2) {
        return;
    }
    cnt.mouse_moves++;
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
    cnt.mouse_buttons++;
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
    cnt.mouse_buttons++;
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
