/*
 * metal.m — the native macOS front end for the emulated Pi 4.
 *
 * The macOS twin of ui/dx11.cpp, and deliberately the same program: each
 * frame the UI thread pulls the fb view over ui/metal.h's boundary
 * (config snapshot plus host pointers into guest RAM, no locks), uploads
 * the raw bytes to a shared buffer, decodes them to linear RGB with a
 * per-format fragment shader, and stretches the decoded surface over the
 * whole content view — the window is the monitor, so every mode fills it,
 * as the Pi's GPU fills a real display.  8 bpp palette and 32 bpp are the
 * two formats RISC OS's BCMVideo ever asks for; the rest of the decoder
 * set is the same source with different constants.
 *
 * Where Windows compiles HLSL at start-up with d3dcompiler, this compiles
 * MSL at start-up with newLibraryWithSource:, and specialises the decode
 * and scale passes with Metal function constants rather than preprocessor
 * macros — the same "one source, several shaders" shape, in the form the
 * platform offers.
 *
 * This file includes no QEMU headers and talks to the emulation only
 * through ui/metal.h.  No ARC: QEMU does not build Objective-C with it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <ImageIO/ImageIO.h>
#import <CoreServices/CoreServices.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ui/metal.h"

/* ------------------------------------------------------------------ */
/* State, owned by the UI thread only                                  */

#define METAL_TITLE     @"RISC OS 5 — Raspberry Pi 4 (QEMU)"
#define METAL_RING      3               /* frames of upload buffer in flight */

@class MetalView;

static struct {
    NSWindow *window;
    MetalView *view;
    CAMetalLayer *layer;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLLibrary> library;
    id<MTLSamplerState> linear;
    dispatch_semaphore_t inflight;
    bool ready;
    bool lost;
    /* input forwarded, for the debug log */
    unsigned keys;
    unsigned mouse_moves;
    unsigned mouse_buttons;
} m;

static uint32_t frame_count;

/* -display metal options, set once at init: how the guest screen is
 * magnified to the content view (sharp-bilinear by default) and whether
 * an optional CRT scanline mask rides on top at 2x+ magnification. */
enum MetalScaling { METAL_SCALING_LINEAR = 0, METAL_SCALING_SHARP,
                    METAL_SCALING_NEAREST };
static struct {
    int scaling;                        /* MetalScaling */
    bool scanlines;
} video_opts = { METAL_SCALING_SHARP, false };

void metal_glue_video_opts(int scaling, int scanlines)
{
    if (scaling >= METAL_SCALING_LINEAR && scaling <= METAL_SCALING_NEAREST) {
        video_opts.scaling = scaling;
    }
    video_opts.scanlines = scanlines != 0;
}

/* The per-mode pipeline: everything that depends on the fb config. */
static struct {
    bool up;
    uint32_t generation;
    uint32_t bpp;
    uint32_t xres, yres, pitch, rows;

    id<MTLBuffer> raw[METAL_RING];      /* guest bytes, pitch * rows */
    unsigned ring;
    id<MTLBuffer> palette;              /* 256 * RGBA8 */
    uint8_t pal_cache[256 * 4];
    bool pal_valid;

    id<MTLTexture> decoded;             /* linear RGB, xres x yres */
    id<MTLRenderPipelineState> decode;  /* specialised for this bpp */
    id<MTLRenderPipelineState> scale;   /* specialised for the options */
} fb;

static bool fb_failed;
static uint32_t fb_failed_generation;

/* The pointer sprite: a pipeline and an image that do not depend on
 * the guest's mode, built once beside the scale pass.  The VCHIQ peer
 * answers the dispmanx requests the ROM sends for its hardware
 * pointer, so the sprite is composited here on top of the scaled
 * frame -- never written to guest RAM, sharp at any window scale, and
 * without the scanline mask: on a real Acorn machine the sprite lay
 * over the CRT, it was not part of its raster. */
#define METAL_CURSOR_TEXELS 64     /* words per side, as the peer defines */
static struct {
    id<MTLRenderPipelineState> pipe;
    id<MTLBuffer> image;            /* METAL_CURSOR_TEXELS^2 ARGB words */
    bool visible;
    uint32_t generation;
    int32_t x, y, w, h;             /* dest rect, display pixels */
    int32_t img_w, img_h;           /* the sprite's own resolution */
    int32_t disp_w, disp_h;         /* the display the rect is measured in */
} ptr;

/* The shader constants, mirroring the Params struct in the MSL below. */
typedef struct {
    uint32_t dim[4];    /* xres, yres, pitch(bytes), bpp */
    uint32_t misc[4];   /* xoffset(px), yoffset(rows), pixo, unused */
    uint32_t post[4];   /* view w, view h, scaling, scanlines */
} MetalParams;

/* The front end's log: a file next to the process, because a windowed
 * build has no console to complain to.  METAL_DEBUG=1 also turns on
 * Metal's own validation, which the environment reads at device
 * creation.  Not static: ui/metal_script.m logs here too. */

void metal_log(const char *fmt, ...)
{
    static FILE *f;
    va_list ap;

    if (!f) {
        f = fopen("metal-debug.txt", "w");
        if (!f) {
            return;
        }
        setvbuf(f, NULL, _IOLBF, 0);
    }
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
}

static void metal_screenshot(void);

/* METAL_DEBUG=1 traces the events the pump sees but the guest does not,
 * which is the only way to tell "the app never got the keystroke" from
 * "the view never got it". */
static bool metal_debug(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("METAL_DEBUG");
        on = e && *e && *e != '0';
    }
    return on != 0;
}

/* ------------------------------------------------------------------ */
/* Grab: the guest owns the pointer until Ctrl+Alt+G                   */

static void metal_mouse_centre(void);
static void metal_mouse_send_abs(int gx, int gy);

static struct {
    bool on;
} kbd;

/* The host cursor is hidden whenever the guest is drawing the pointer
 * under it -- which is any time it is over the guest's screen in a key
 * window, not just while grabbed.  The tablet keeps the two arrows in
 * the same place, so showing both is showing the same pointer twice.
 * [NSCursor hide] is counted, so it is called only on a change. */
static struct {
    bool hidden;
    bool inside;
} cursor;

static void metal_cursor_sync(void)
{
    bool want = kbd.on || (cursor.inside && [m.window isKeyWindow]);

    if (want == cursor.hidden) {
        return;
    }
    cursor.hidden = want;
    if (want) {
        [NSCursor hide];
    } else {
        [NSCursor unhide];
    }
}

static struct {
    bool swallow_up;                /* the middle-up of a grab click */
    int held_left, held_right;      /* button actually sent, per stream */
    /* the guest pointer position we last sent, in guest pixels: the
     * anchor for relative motion while grabbed (absolute while not) */
    int gx, gy;
    bool have_guest;
    int gxres, gyres;               /* guest screen size, from the fb view */
} mouse;

/* A point in the view's coordinates, put on the screen in Quartz's:
 * Cocoa measures up from the bottom of the main screen, CoreGraphics
 * down from its top, and the warp is a CoreGraphics call. */
static CGPoint metal_view_point_to_cg(NSPoint p)
{
    NSPoint win = [m.view convertPoint:p toView:nil];
    NSRect r = [m.window convertRectToScreen:NSMakeRect(win.x, win.y, 0, 0)];
    CGFloat top = NSMaxY([[[NSScreen screens] objectAtIndex:0] frame]);

    return CGPointMake(r.origin.x, top - r.origin.y);
}

static void metal_mouse_centre(void)
{
    NSRect b = [m.view bounds];
    CGPoint c = metal_view_point_to_cg(NSMakePoint(NSMidX(b), NSMidY(b)));

    CGWarpMouseCursorPosition(c);
}

static void metal_set_grab(bool on)
{
    if (on == kbd.on) {
        return;
    }
    kbd.on = on;
    metal_glue_grab(on);
    if (on) {
        if (!mouse.have_guest && mouse.gxres > 1) {
            mouse.gx = mouse.gxres / 2;
            mouse.gy = mouse.gyres / 2;
            mouse.have_guest = true;
        }
        metal_mouse_centre();
        /* Untie the cursor from the mouse: the deltas keep coming, the
         * arrow stays where it was warped, and motion is unbounded. */
        CGAssociateMouseAndMouseCursorPosition(false);
    } else {
        /* Put the host cursor where the guest arrow is, so the two
         * coincide again the moment the pointer is handed back. */
        if (mouse.have_guest && mouse.gxres > 1) {
            NSRect b = [m.view bounds];
            CGFloat x = (CGFloat)mouse.gx * NSWidth(b) / (mouse.gxres - 1);
            CGFloat y = NSHeight(b)
                        - (CGFloat)mouse.gy * NSHeight(b) / (mouse.gyres - 1);
            CGWarpMouseCursorPosition(
                metal_view_point_to_cg(NSMakePoint(x, y)));
        }
        CGAssociateMouseAndMouseCursorPosition(true);
    }
    metal_cursor_sync();
    metal_log("grab %s", on ? "on" : "off");
}

/* ------------------------------------------------------------------ */
/* Mouse.  The guest device is an absolute tablet, so the position is   */
/* always sent as a coordinate: ungrabbed that is the host cursor's     */
/* place in the view scaled to the guest screen (the two arrows never   */
/* diverge); grabbed the cursor is dissociated and the deltas           */
/* accumulate into a virtual position sent absolutely -- it cannot      */
/* drift.                                                              */

static void metal_mouse_send_abs(int gx, int gy)
{
    if (mouse.gxres > 1 && mouse.gyres > 1) {
        if (gx < 0) {
            gx = 0;
        }
        if (gy < 0) {
            gy = 0;
        }
        if (gx > mouse.gxres - 1) {
            gx = mouse.gxres - 1;
        }
        if (gy > mouse.gyres - 1) {
            gy = mouse.gyres - 1;
        }
        metal_glue_mouse_abs(gx, gy, mouse.gxres, mouse.gyres);
        mouse.gx = gx;
        mouse.gy = gy;
        mouse.have_guest = true;
        m.mouse_moves++;
    }
}

static void metal_mouse_move(NSEvent *e)
{
    NSRect b;
    NSPoint p;

    if (mouse.gxres < 2 || mouse.gyres < 2) {
        return;                     /* no guest screen to map onto yet */
    }
    b = [m.view bounds];
    if (NSWidth(b) < 1 || NSHeight(b) < 1) {
        return;
    }
    if (!kbd.on) {
        /* Ungrabbed: host cursor and guest arrow coincide.  Cocoa
         * measures up from the bottom of the view; the guest counts
         * rows down from the top. */
        p = [m.view convertPoint:[e locationInWindow] fromView:nil];
        metal_mouse_send_abs(
            (int)(p.x * (mouse.gxres - 1) / (NSWidth(b) - 1)),
            (int)((NSHeight(b) - p.y) * (mouse.gyres - 1) / (NSHeight(b) - 1)));
        return;
    }
    /* Grabbed: the dissociated cursor still reports deltas.  Scale them
     * by guest-per-view pixels, so a window larger than the guest screen
     * does not run the pointer into the edges at double speed. */
    {
        CGFloat dx = [e deltaX], dy = [e deltaY];

        if (dx != 0 || dy != 0) {
            int gdx = (int)(dx * mouse.gxres / NSWidth(b));
            int gdy = (int)(dy * mouse.gyres / NSHeight(b));

            if (gdx == 0 && dx != 0) {
                gdx = dx > 0 ? 1 : -1;      /* never lose a slow move */
            }
            if (gdy == 0 && dy != 0) {
                gdy = dy > 0 ? 1 : -1;
            }
            metal_mouse_send_abs(mouse.gx + gdx, mouse.gy + gdy);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Keyboard.  NSEvent keyCodes are macOS virtual key codes, which the   */
/* osx keymap is indexed by; the modifiers arrive as a flags word and   */
/* are turned back into presses and releases here.                      */

/* The device-dependent modifier bits, which is the only way to tell a
 * left Shift from a right one.  <IOKit/hidsystem/IOLLEvent.h> has them;
 * naming them here keeps the include list to frameworks we already use. */
#define METAL_LCTRL     0x00000001
#define METAL_LSHIFT    0x00000002
#define METAL_RSHIFT    0x00000004
#define METAL_LCMD      0x00000008
#define METAL_RCMD      0x00000010
#define METAL_LALT      0x00000020
#define METAL_RALT      0x00000040
#define METAL_RCTRL     0x00002000

static const struct { uint32_t keycode; uint32_t mask; } metal_mods[] = {
    { 59, METAL_LCTRL  },  { 62, METAL_RCTRL  },
    { 56, METAL_LSHIFT },  { 60, METAL_RSHIFT },
    { 58, METAL_LALT   },  { 61, METAL_RALT   },
    { 55, METAL_LCMD   },  { 54, METAL_RCMD   },
};
#define METAL_SHIFT_ANY (METAL_LSHIFT | METAL_RSHIFT)
#define METAL_CTRL_ANY  (METAL_LCTRL  | METAL_RCTRL)
#define METAL_CMD_ANY   (METAL_LCMD   | METAL_RCMD)
#define METAL_ALT_ANY   (METAL_LALT   | METAL_RALT)

static uint32_t mod_state;              /* the masks we believe are down */
/*
 * Masks spent on a mouse button and therefore hidden from the guest. A
 * modifier used to pick a button must not also arrive as a key: RISC OS
 * reads Shift-Adjust and Adjust as different gestures, so leaking the
 * Shift makes the click land as something else entirely -- which is
 * exactly how Shift-click came to do nothing while Ctrl-click worked.
 */
static uint32_t mod_masked;

static void metal_flags_changed(NSEvent *e)
{
    NSUInteger flags = [e modifierFlags];
    size_t i;

    for (i = 0; i < sizeof(metal_mods) / sizeof(metal_mods[0]); i++) {
        bool now = (flags & metal_mods[i].mask) != 0;
        bool was = (mod_state & metal_mods[i].mask) != 0;

        if (mod_masked & metal_mods[i].mask) {
            continue;               /* it is a mouse button just now */
        }
        if (now != was) {
            metal_glue_key(now, metal_mods[i].keycode);
            if (now) {
                mod_state |= metal_mods[i].mask;
            } else {
                mod_state &= ~metal_mods[i].mask;
            }
        }
    }
    /* Caps Lock latches on the host, so the guest never sees a release
     * unless we make one: press and release on each change of state. */
    if ([e keyCode] == 57) {
        metal_glue_key(true, 57);
        metal_glue_key(false, 57);
    }
}

/* Take a modifier away from the guest for the duration of a click */
static void metal_mods_hold(uint32_t masks)
{
    size_t i;

    for (i = 0; i < sizeof(metal_mods) / sizeof(metal_mods[0]); i++) {
        uint32_t m = metal_mods[i].mask;

        if ((masks & m) && (mod_state & m)) {
            metal_glue_key(false, metal_mods[i].keycode);
            mod_state &= ~m;
            mod_masked |= m;
        }
    }
}

/* And give it back, if it is still physically down when the button is up */
static void metal_mods_restore(NSEvent *e)
{
    NSUInteger flags = [e modifierFlags];
    size_t i;

    if (!mod_masked) {
        return;
    }
    for (i = 0; i < sizeof(metal_mods) / sizeof(metal_mods[0]); i++) {
        uint32_t m = metal_mods[i].mask;

        if ((mod_masked & m) && (flags & m)) {
            metal_glue_key(true, metal_mods[i].keycode);
            mod_state |= m;
        }
    }
    mod_masked = 0;
}

/* Every modifier the guest thinks is down, released.  Called when the
 * window loses focus, so a Command-Tab away does not leave RISC OS
 * holding a phantom Alt down for ever. */
static void metal_release_modifiers(void)
{
    size_t i;

    for (i = 0; i < sizeof(metal_mods) / sizeof(metal_mods[0]); i++) {
        if (mod_state & metal_mods[i].mask) {
            metal_glue_key(false, metal_mods[i].keycode);
        }
    }
    mod_state = 0;
}

/* ------------------------------------------------------------------ */
/* The shaders.  One source, compiled once; the decode pass is          */
/* specialised on the guest's bits per pixel and the scale pass on the  */
/* -display options, with function constants.                           */

static NSString * const SHADER_SRC = @""
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"\n"
"constant uint BPP       [[function_constant(0)]];\n"
"constant uint SCALING   [[function_constant(1)]];\n"
"constant bool SCANLINES [[function_constant(2)]];\n"
"\n"
"struct Params {\n"
"    uint4 dim;     /* xres, yres, pitch(bytes), bpp */\n"
"    uint4 misc;    /* xoffset(px), yoffset(rows), pixo, unused */\n"
"    uint4 post;    /* view w, view h, scaling, scanlines */\n"
"};\n"
"\n"
"struct VSOut {\n"
"    float4 pos [[position]];\n"
"    float2 uv;\n"
"};\n"
"\n"
"vertex VSOut vs_main(uint id [[vertex_id]])\n"
"{\n"
"    /* one big triangle covering the target; uv happens to be the same\n"
"     * (0..2) coordinates the position is built from, so the interpolated\n"
"     * uv runs 0..1 across the visible area with y=0 at the top */\n"
"    VSOut o;\n"
"    float2 t = float2((id << 1) & 2, id & 2);\n"
"    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"    o.uv = t;\n"
"    return o;\n"
"}\n"
"\n"
"fragment float4 ps_decode(VSOut v [[stage_in]],\n"
"                          constant Params &P [[buffer(0)]],\n"
"                          device const uchar *raw [[buffer(1)]],\n"
"                          device const uchar4 *pal [[buffer(2)]])\n"
"{\n"
"    /* visible pixel -> buffer coordinates, pan included */\n"
"    uint bx = uint(v.pos.x) + P.misc.x;\n"
"    uint by = uint(v.pos.y) + P.misc.y;\n"
"    uint row = by * P.dim.z;\n"
"    uint r = 0, g = 0, b = 0;\n"
"\n"
"    if (BPP == 32) {\n"
"        r = raw[row + bx * 4 + 0];\n"
"        g = raw[row + bx * 4 + 1];\n"
"        b = raw[row + bx * 4 + 2];\n"
"        if (P.misc.z == 0) { uint t = r; r = b; b = t; }   /* BGR order */\n"
"    } else if (BPP == 24) {\n"
"        r = raw[row + bx * 3 + 0];\n"
"        g = raw[row + bx * 3 + 1];\n"
"        b = raw[row + bx * 3 + 2];\n"
"    } else if (BPP == 16) {\n"
"        uint w = uint(raw[row + bx * 2])\n"
"               | (uint(raw[row + bx * 2 + 1]) << 8);       /* RGB565 */\n"
"        r = ((w >> 11) & 31) * 255 / 31;\n"
"        g = ((w >>  5) & 63) * 255 / 63;\n"
"        b = ((w      ) & 31) * 255 / 31;\n"
"    } else {\n"
"        /* palettised: 8 bpp, or sub-byte 1/2/4 with LSB-first packing */\n"
"        uint bpp = P.dim.w;\n"
"        uint byte = raw[row + bx * bpp / 8];\n"
"        uint idx = (byte >> ((bx * bpp) & 7)) & ((1u << bpp) - 1);\n"
"        uchar4 c = pal[idx];\n"
"        /* 0x00BBGGRR words, so as bytes: red, green, blue, unused */\n"
"        return float4(float(c.x), float(c.y), float(c.z), 255.0) / 255.0;\n"
"    }\n"
"    return float4(float(r), float(g), float(b), 255.0) / 255.0;\n"
"}\n"
"\n"
"fragment float4 ps_scale(VSOut v [[stage_in]],\n"
"                         constant Params &P [[buffer(0)]],\n"
"                         texture2d<float> src [[texture(0)]],\n"
"                         sampler lin [[sampler(0)]])\n"
"{\n"
"    float2 outPx = float2(P.post.x, P.post.y);      /* view size */\n"
"    float2 srcPx = float2(P.dim.x, P.dim.y);        /* decoded size */\n"
"    float2 st = v.pos.xy / outPx * srcPx;           /* source-pixel coords */\n"
"    if (SCALING == 1) {\n"
"        /* Sharp bilinear: within each source texel the bilinear\n"
"         * transition is narrowed to a 1/ratio-wide band at the texel\n"
"         * edge, so at 1:1 the image passes through untouched and at 2x+\n"
"         * it is crisp with just enough filtering to avoid staircases. */\n"
"        float2 ratio = max(outPx / srcPx, float2(1.0));\n"
"        float2 halfw = 0.5 - 0.5 / ratio;           /* half the band */\n"
"        float2 i = floor(st);\n"
"        float2 f = st - i - 0.5;                    /* -0.5..0.5 in-texel */\n"
"        st = i + 0.5 + clamp(f, -halfw, halfw);\n"
"    } else if (SCALING == 2) {\n"
"        st = floor(st) + 0.5;                       /* nearest */\n"
"    }\n"
"    float4 c = src.sample(lin, st / srcPx);\n"
"    if (SCANLINES) {\n"
"        /* CRT flavour, only when magnified enough for a line to be two */\n"
"        if (P.post.w != 0 && outPx.y >= srcPx.y * 1.99\n"
"            && (uint(v.pos.y) & 1) != 0) {\n"
"            c.rgb *= 0.8;\n"
"        }\n"
"    }\n"
"    return float4(c.rgb, 1.0);\n"
"}\n"
"\n"
"vertex VSOut vs_quad(uint id [[vertex_id]], constant float4 &R [[buffer(0)]])\n"
"{\n"
"    /* one triangle covering the NDC rectangle R = {x0, y0, x1, y1}:\n"
"     * the vertices overshoot to 2x the rectangle's span, the same\n"
"     * trick as vs_main, so the unit square of t lies strictly inside\n"
"     * and no pixel of the rectangle lands on the hypotenuse (a\n"
"     * corner-exact triangle loses its lower right half to the\n"
"     * top-left fill rule).  uv runs 0..1 across the rectangle; the\n"
"     * overhang samples past 1 and the fragment clamps it. */\n"
"    VSOut o;\n"
"    float2 t = float2((id << 1) & 2, id & 2);\n"
"    o.pos = float4(R.xy + t * (R.zw - R.xy), 0, 1);\n"
"    o.uv = t;\n"
"    return o;\n"
"}\n"
"\n"
"fragment float4 ps_pointer(VSOut v [[stage_in]],\n"
"                           constant uint2 &dim [[buffer(0)]],\n"
"                           device const uchar4 *img [[buffer(1)]])\n"
"{\n"
"    /* The triangle overshoots the rectangle on purpose; its overhang\n"
"     * is not clipped by anything, so drop it here.  The image words\n"
"     * are little-endian 0xAARRGGBB, so the bytes are B, G, R, A --\n"
"     * what HWP_Update's REV-plus-alpha leaves in guest memory. */\n"
"    if (v.uv.x > 1.0f || v.uv.y > 1.0f) {\n"
"        discard_fragment();\n"
"    }\n"
"    uint2 t = min(uint2(v.uv * float2(dim)), dim - 1);\n"
"    uchar4 c = img[t.y * dim.x + t.x];\n"
"    return float4(float(c.z), float(c.y), float(c.x), float(c.w)) / 255.0f;\n"
"}\n";

static bool metal_compile_library(void)
{
    NSError *err = nil;
    MTLCompileOptions *opts = [[MTLCompileOptions alloc] init];

    m.library = [m.device newLibraryWithSource:SHADER_SRC
                                       options:opts
                                         error:&err];
    [opts release];
    if (!m.library) {
        metal_log("shader compile failed: %s",
                  [[err localizedDescription] UTF8String]);
        return false;
    }
    return true;
}

/* A render pipeline from vs_main and one fragment function, specialised
 * by the function constants the caller sets. */
static id<MTLRenderPipelineState> metal_pipeline(NSString *fragment,
                                                 MTLFunctionConstantValues *cv,
                                                 MTLPixelFormat format)
{
    MTLRenderPipelineDescriptor *pd;
    id<MTLRenderPipelineState> ps;
    id<MTLFunction> vs, fs;
    NSError *err = nil;

    vs = [m.library newFunctionWithName:@"vs_main"];
    fs = [m.library newFunctionWithName:fragment constantValues:cv error:&err];
    if (!vs || !fs) {
        metal_log("function %s failed: %s", [fragment UTF8String],
                  [[err localizedDescription] UTF8String]);
        [vs release];
        [fs release];
        return nil;
    }
    pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = vs;
    pd.fragmentFunction = fs;
    pd.colorAttachments[0].pixelFormat = format;
    ps = [m.device newRenderPipelineStateWithDescriptor:pd error:&err];
    [pd release];
    [vs release];
    [fs release];
    if (!ps) {
        metal_log("pipeline %s failed: %s", [fragment UTF8String],
                  [[err localizedDescription] UTF8String]);
    }
    return ps;
}

/* ------------------------------------------------------------------ */
/* The per-mode pipeline                                               */

static void fb_release_pipeline(void)
{
    unsigned i;

    for (i = 0; i < METAL_RING; i++) {
        [fb.raw[i] release];
        fb.raw[i] = nil;
    }
    [fb.decoded release];
    fb.decoded = nil;
    [fb.decode release];
    fb.decode = nil;
    fb.up = false;
}

static bool fb_build_pipeline(const MetalFbView *v)
{
    MTLTextureDescriptor *td;
    MTLFunctionConstantValues *cv;
    size_t bytes = (size_t)v->pitch * v->rows;
    uint32_t shader_bpp;
    unsigned i;

    fb_release_pipeline();

    if (!bytes || !v->xres || !v->yres) {
        return false;
    }

    /* The raw guest bytes, a ring deep enough that the frame the GPU is
     * still reading is never the one the CPU is writing.  Shared storage
     * means the "upload" is a memcpy into memory the GPU already sees --
     * on Apple silicon there is nothing further to transfer. */
    for (i = 0; i < METAL_RING; i++) {
        fb.raw[i] = [m.device newBufferWithLength:bytes
                                          options:MTLResourceStorageModeShared];
        if (!fb.raw[i]) {
            metal_log("raw buffer %zu bytes failed", bytes);
            fb_release_pipeline();
            return false;
        }
    }
    fb.ring = 0;

    if (!fb.palette) {
        fb.palette = [m.device newBufferWithLength:256 * 4
                                           options:MTLResourceStorageModeShared];
        if (!fb.palette) {
            metal_log("palette buffer failed");
            fb_release_pipeline();
            return false;
        }
    }
    fb.pal_valid = false;               /* force a refill for the new mode */

    /* The decoded surface: linear RGB at the guest's own resolution, the
     * scale pass's only input and what a screenshot reads back. */
    td = [MTLTextureDescriptor
          texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                       width:v->xres
                                      height:v->yres
                                   mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    fb.decoded = [m.device newTextureWithDescriptor:td];
    if (!fb.decoded) {
        metal_log("decoded texture %ux%u failed", v->xres, v->yres);
        fb_release_pipeline();
        return false;
    }

    /* One decoder per format.  Anything not 32, 24 or 16 bpp is
     * palettised, and the shader takes the exact width from the
     * constants -- 8 bpp is the one RISC OS actually asks for. */
    shader_bpp = (v->bpp == 32 || v->bpp == 24 || v->bpp == 16) ? v->bpp : 8;
    cv = [[MTLFunctionConstantValues alloc] init];
    [cv setConstantValue:&shader_bpp type:MTLDataTypeUInt atIndex:0];
    fb.decode = metal_pipeline(@"ps_decode", cv, MTLPixelFormatRGBA8Unorm);
    [cv release];
    if (!fb.decode) {
        fb_release_pipeline();
        return false;
    }

    fb.generation = v->generation;
    fb.bpp = v->bpp;
    fb.xres = v->xres;
    fb.yres = v->yres;
    fb.pitch = v->pitch;
    fb.rows = v->rows;
    fb.up = true;
    metal_log("pipeline up: gen %u, %ux%u, %u bpp, pitch %u, %u rows",
              v->generation, v->xres, v->yres, v->bpp, v->pitch, v->rows);
    return true;
}

/* The scale pass does not depend on the guest's mode, only on the
 * options, so it is built once. */
static bool fb_build_scale(void)
{
    MTLFunctionConstantValues *cv = [[MTLFunctionConstantValues alloc] init];
    uint32_t scaling = (uint32_t)video_opts.scaling;
    bool scanlines = video_opts.scanlines;

    [cv setConstantValue:&scaling type:MTLDataTypeUInt atIndex:1];
    [cv setConstantValue:&scanlines type:MTLDataTypeBool atIndex:2];
    fb.scale = metal_pipeline(@"ps_scale", cv, m.layer.pixelFormat);
    [cv release];
    return fb.scale != nil;
}

/* The pointer pipeline: straight alpha, so the ROM's anti-fringe fill
 * (transparent pixels carrying the neighbouring colour at alpha 0)
 * behaves exactly as it does against the firmware's compositor. */
static bool ptr_build(void)
{
    MTLRenderPipelineDescriptor *pd;
    id<MTLFunction> vs, fs;
    NSError *err = nil;

    vs = [m.library newFunctionWithName:@"vs_quad"];
    fs = [m.library newFunctionWithName:@"ps_pointer"];
    if (!vs || !fs) {
        metal_log("pointer shader functions missing");
        [vs release];
        [fs release];
        return false;
    }
    pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = vs;
    pd.fragmentFunction = fs;
    pd.colorAttachments[0].pixelFormat = m.layer.pixelFormat;
    pd.colorAttachments[0].blendingEnabled = YES;
    pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    pd.colorAttachments[0].destinationRGBBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pd.colorAttachments[0].destinationAlphaBlendFactor =
        MTLBlendFactorOneMinusSourceAlpha;
    ptr.pipe = [m.device newRenderPipelineStateWithDescriptor:pd error:&err];
    [pd release];
    [vs release];
    [fs release];
    if (!ptr.pipe) {
        metal_log("pointer pipeline failed: %s",
                  err ? [[err localizedDescription] UTF8String] : "?");
        return false;
    }
    ptr.image = [m.device
        newBufferWithLength:METAL_CURSOR_TEXELS * METAL_CURSOR_TEXELS * 4
                    options:MTLResourceStorageModeShared];
    if (!ptr.image) {
        metal_log("pointer image buffer failed");
        return false;
    }
    return true;
}

static void fb_upload(const MetalFbView *v)
{
    fb.ring = (fb.ring + 1) % METAL_RING;
    memcpy([fb.raw[fb.ring] contents], v->fb, (size_t)v->pitch * v->rows);

    if (!fb.pal_valid
        || memcmp(fb.pal_cache, v->palette, sizeof(fb.pal_cache)) != 0) {
        memcpy(fb.pal_cache, v->palette, sizeof(fb.pal_cache));
        memcpy([fb.palette contents], fb.pal_cache, sizeof(fb.pal_cache));
        fb.pal_valid = true;
    }
}

/* ------------------------------------------------------------------ */
/* A frame                                                             */

static void metal_update_title(void)
{
    static uint32_t last_second_frame;
    static double last_tick;
    double now = CFAbsoluteTimeGetCurrent();

    if (last_tick && now > last_tick) {
        unsigned fps = (unsigned)((frame_count - last_second_frame)
                                  / (now - last_tick));
        [m.window setTitle:
            [NSString stringWithFormat:
                @"RISC OS 5 — %ux%u, %u bpp · %u fps — Raspberry Pi 4 (QEMU)",
                fb.xres, fb.yres, fb.bpp, fps]];
    }
    last_tick = now;
    last_second_frame = frame_count;
}

static bool metal_render_frame(void)
{
    MetalFbView v;
    id<CAMetalDrawable> drawable;
    id<MTLCommandBuffer> cb;
    MTLRenderPassDescriptor *rp;
    id<MTLRenderCommandEncoder> enc;
    MetalParams params;
    CGSize target;
    bool have_fb;

    if (++frame_count % 300 == 0) {
        metal_log("frame %u: pipeline %s, fb gen %u, %ux%u bpp %u, "
                  "keys %u, mouse moves %u, button events %u",
                  frame_count, fb.up ? "up" : "down",
                  fb.generation, fb.xres, fb.yres, fb.bpp,
                  m.keys, m.mouse_moves, m.mouse_buttons);
    }
    /* The status line: the window title carries the guest's mode and the
     * presented frame rate, once a second. */
    if (frame_count % 60 == 0) {
        metal_update_title();
    }

    /* METAL_SHOT_EVERY=<frames> writes the decoded surface out on that
     * period.  The decode is what is hard to be sure of -- a wrong
     * shader still presents a frame -- and a window is not always
     * something one can photograph: over a screen share, in CI, or on a
     * machine one is only ever logged in to.  Off unless asked for. */
    {
        static int shot_every = -1;

        if (shot_every < 0) {
            const char *e = getenv("METAL_SHOT_EVERY");
            shot_every = e ? atoi(e) : 0;
        }
        if (shot_every > 0 && frame_count % (uint32_t)shot_every == 0) {
            metal_screenshot();
        }
    }

    have_fb = metal_glue_fb_view(&v) != 0;
    if (have_fb) {
        mouse.gxres = v.xres;           /* the mouse maps onto this screen */
        mouse.gyres = v.yres;
        if (!fb.up || v.generation != fb.generation) {
            if (fb_failed && v.generation == fb_failed_generation) {
                have_fb = false;        /* same mode, same failure */
            } else if (!fb_build_pipeline(&v)) {
                fb_failed = true;
                fb_failed_generation = v.generation;
                have_fb = false;
            } else {
                fb_failed = false;
            }
        }
    } else if (frame_count % 300 == 0) {
        metal_log("frame %u: no fb view yet", frame_count);
    }

    /* Wait for a ring slot before touching one: the frame three back has
     * to have left the GPU. */
    dispatch_semaphore_wait(m.inflight, DISPATCH_TIME_FOREVER);

    drawable = [m.layer nextDrawable];
    if (!drawable) {
        dispatch_semaphore_signal(m.inflight);
        return false;                   /* occluded, or no free surface */
    }
    cb = [m.queue commandBuffer];

    if (have_fb) {
        fb_upload(&v);

        target = m.layer.drawableSize;
        params.dim[0] = v.xres;
        params.dim[1] = v.yres;
        params.dim[2] = v.pitch;
        params.dim[3] = v.bpp;
        params.misc[0] = v.xoffset;
        params.misc[1] = v.yoffset;
        params.misc[2] = v.pixo;
        params.misc[3] = 0;
        params.post[0] = (uint32_t)target.width;
        params.post[1] = (uint32_t)target.height;
        params.post[2] = (uint32_t)video_opts.scaling;
        params.post[3] = video_opts.scanlines ? 1 : 0;

        /* decode pass: raw bytes -> linear RGB */
        rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = fb.decoded;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:fb.decode];
        [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
        [enc setFragmentBuffer:fb.raw[fb.ring] offset:0 atIndex:1];
        [enc setFragmentBuffer:fb.palette offset:0 atIndex:2];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0 vertexCount:3];
        [enc endEncoding];
    }

    /* scale pass: the decoded surface over the whole content view */
    rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = drawable.texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.05, 0.05, 0.08, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    enc = [cb renderCommandEncoderWithDescriptor:rp];
    if (have_fb && fb.scale) {
        [enc setRenderPipelineState:fb.scale];
        [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
        [enc setFragmentTexture:fb.decoded atIndex:0];
        [enc setFragmentSamplerState:m.linear atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0 vertexCount:3];
    }

    /* The pointer sprite, on top of the scaled frame.  The peer
     * commits whole transactions at UpdateSubmit, so a generation move
     * is always a complete new sprite and position; a read that raced
     * one is reported stale and the previous frame's is kept. */
    {
        MetalCursorView cv;

        if (ptr.pipe && metal_glue_cursor_view(&cv) && !cv.stale
            && cv.generation != ptr.generation) {
            memcpy([ptr.image contents], cv.argb,
                   METAL_CURSOR_TEXELS * METAL_CURSOR_TEXELS * 4);
            ptr.generation = cv.generation;
            ptr.visible = cv.visible;
            ptr.x = cv.x;
            ptr.y = cv.y;
            ptr.w = cv.w;
            ptr.h = cv.h;
            ptr.img_w = cv.img_w;
            ptr.img_h = cv.img_h;
            ptr.disp_w = cv.disp_w;
            ptr.disp_h = cv.disp_h;
        }
        if (have_fb && ptr.pipe && ptr.visible && ptr.w > 0 && ptr.h > 0
            && ptr.img_w > 0 && ptr.img_h > 0) {
            float rect[4];
            uint32_t dim[2];
            double dw = ptr.disp_w > 0 ? ptr.disp_w : (int32_t)v.xres;
            double dh = ptr.disp_h > 0 ? ptr.disp_h : (int32_t)v.yres;

            /* The dest rect is in display pixels -- the space the ROM's
             * own scale arithmetic produced it in -- and the view is
             * that display; the image keeps its own resolution and is
             * stretched to the rect, whatever mode is underneath. */
            rect[0] = (float)(2.0 * (double)ptr.x / dw - 1.0);
            rect[1] = (float)(1.0 - 2.0 * (double)ptr.y / dh);
            rect[2] = (float)(rect[0] + 2.0 * (double)ptr.w / dw);
            rect[3] = (float)(rect[1] - 2.0 * (double)ptr.h / dh);
            dim[0] = (uint32_t)ptr.img_w;
            dim[1] = (uint32_t)ptr.img_h;
            [enc setRenderPipelineState:ptr.pipe];
            [enc setVertexBytes:rect length:sizeof(rect) atIndex:0];
            [enc setFragmentBytes:dim length:sizeof(dim) atIndex:0];
            [enc setFragmentBuffer:ptr.image offset:0 atIndex:1];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:0 vertexCount:3];
        }
    }
    [enc endEncoding];

    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        dispatch_semaphore_signal(m.inflight);
    }];
    [cb presentDrawable:drawable];
    [cb commit];
    return have_fb;
}

/* ------------------------------------------------------------------ */
/* Screenshot: the decoded surface to a PNG file                       */

static void metal_screenshot(void)
{
    static unsigned n;
    id<MTLBuffer> staging;
    id<MTLCommandBuffer> cb;
    id<MTLBlitCommandEncoder> blit;
    CGColorSpaceRef cs;
    CGContextRef ctx;
    CGImageRef img;
    CGImageDestinationRef dst;
    NSString *path;
    NSURL *url;
    size_t stride;

    if (!fb.up || !fb.decoded) {
        return;
    }
    stride = (size_t)fb.xres * 4;
    staging = [m.device newBufferWithLength:stride * fb.yres
                                    options:MTLResourceStorageModeShared];
    if (!staging) {
        return;
    }
    cb = [m.queue commandBuffer];
    blit = [cb blitCommandEncoder];
    [blit copyFromTexture:fb.decoded
              sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(fb.xres, fb.yres, 1)
                 toBuffer:staging
        destinationOffset:0
   destinationBytesPerRow:stride
 destinationBytesPerImage:stride * fb.yres];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];

    /* The pointer is composited over the frame, not in it, and a
     * screenshot shows what a human sees: blend the sprite in with the
     * same straight alpha the pipeline uses.  The dest rect is in
     * display pixels, so it is mapped into the decoded surface's own
     * guest pixels first; the image is indexed by its own resolution. */
    {
        MetalCursorView cv;

        if (metal_glue_cursor_view(&cv) && !cv.stale && cv.visible
            && cv.w > 0 && cv.h > 0 && cv.img_w > 0 && cv.img_h > 0
            && cv.disp_w > 0 && cv.disp_h > 0) {
            uint8_t *px = [staging contents];
            const uint8_t *sp = cv.argb;
            double kx = (double)fb.xres / (double)cv.disp_w;
            double ky = (double)fb.yres / (double)cv.disp_h;
            /* sprite texels per guest pixel, for the stretch */
            double tx = (double)cv.img_w / ((double)cv.w * kx);
            double ty = (double)cv.img_h / ((double)cv.h * ky);
            int32_t gx0 = (int32_t)(cv.x * kx);
            int32_t gy0 = (int32_t)(cv.y * ky);
            int32_t gx1 = (int32_t)((cv.x + cv.w) * kx);
            int32_t gy1 = (int32_t)((cv.y + cv.h) * ky);
            int32_t x0 = gx0 < 0 ? 0 : gx0;
            int32_t y0 = gy0 < 0 ? 0 : gy0;
            int32_t sx, sy;

            if (gx1 > (int32_t)fb.xres) {
                gx1 = (int32_t)fb.xres;
            }
            if (gy1 > (int32_t)fb.yres) {
                gy1 = (int32_t)fb.yres;
            }
            for (sy = y0; sy < gy1; sy++) {
                for (sx = x0; sx < gx1; sx++) {
                    /* nearest sprite texel for this guest pixel; the
                     * sprite bytes are B,G,R,A, the surface R,G,B,A */
                    int32_t ix = (int32_t)((sx - gx0) * tx);
                    int32_t iy = (int32_t)((sy - gy0) * ty);

                    if (ix >= cv.img_w) {
                        ix = cv.img_w - 1;
                    }
                    if (iy >= cv.img_h) {
                        iy = cv.img_h - 1;
                    }
                    const uint8_t *s = sp + (size_t)(iy * cv.img_w + ix) * 4;
                    uint8_t *d = px + (size_t)(sy * fb.xres + sx) * 4;
                    int a = s[3];

                    d[0] = (uint8_t)(d[0] + (s[2] - d[0]) * a / 255);
                    d[1] = (uint8_t)(d[1] + (s[1] - d[1]) * a / 255);
                    d[2] = (uint8_t)(d[2] + (s[0] - d[2]) * a / 255);
                }
            }
        }
    }

    cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    ctx = CGBitmapContextCreate([staging contents], fb.xres, fb.yres, 8,
                                stride, cs,
                                kCGImageAlphaNoneSkipLast
                                | kCGBitmapByteOrder32Big);
    img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;

    path = [NSString stringWithFormat:@"metal-screenshot-%u.png", ++n];
    url = [NSURL fileURLWithPath:path];
    dst = img ? CGImageDestinationCreateWithURL((CFURLRef)url,
                                                CFSTR("public.png"), 1, NULL)
              : NULL;
    if (dst) {
        CGImageDestinationAddImage(dst, img, NULL);
        CGImageDestinationFinalize(dst);
        CFRelease(dst);
        metal_log("screenshot: %s (%ux%u)", [path UTF8String],
                  fb.xres, fb.yres);
    } else {
        metal_log("screenshot failed");
    }
    if (img) {
        CGImageRelease(img);
    }
    if (ctx) {
        CGContextRelease(ctx);
    }
    CGColorSpaceRelease(cs);
    [staging release];
}

/* ------------------------------------------------------------------ */
/* The window                                                          */

@interface MetalView : NSView
@end

@implementation MetalView

- (CALayer *)makeBackingLayer
{
    CAMetalLayer *l = [CAMetalLayer layer];

    l.device = m.device;
    l.pixelFormat = MTLPixelFormatBGRA8Unorm;
    l.framebufferOnly = YES;
    l.maximumDrawableCount = METAL_RING;
    l.displaySyncEnabled = YES;      /* the present is the pacing */
    l.allowsNextDrawableTimeout = YES;
    return l;
}

- (BOOL)wantsUpdateLayer          { return YES; }
- (BOOL)acceptsFirstResponder     { return YES; }
- (BOOL)isOpaque                  { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }

/* The drawable is in backing pixels; the view is in points.  Keeping
 * them in step is what makes a Retina display show the guest at its own
 * resolution rather than a doubled blur. */
- (void)updateDrawableSize
{
    NSRect b = [self convertRectToBacking:[self bounds]];

    if (NSWidth(b) > 0 && NSHeight(b) > 0) {
        m.layer.contentsScale = [[self window] backingScaleFactor];
        m.layer.drawableSize = CGSizeMake(NSWidth(b), NSHeight(b));
    }
}

- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self updateDrawableSize];
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    [self updateDrawableSize];
}

/* One tracking area over the whole view, so entering and leaving the
 * guest's screen is an event rather than something polled per frame. */
- (void)updateTrackingAreas
{
    NSTrackingArea *area;

    for (NSTrackingArea *old in [self trackingAreas]) {
        [self removeTrackingArea:old];
    }
    area = [[NSTrackingArea alloc]
            initWithRect:[self bounds]
                 options:NSTrackingMouseEnteredAndExited
                         | NSTrackingActiveInKeyWindow
                         | NSTrackingInVisibleRect
                   owner:self
                userInfo:nil];
    [self addTrackingArea:area];
    [area release];
    [super updateTrackingAreas];
}

- (void)mouseEntered:(NSEvent *)e
{
    (void)e;
    cursor.inside = true;
    metal_cursor_sync();
}

- (void)mouseExited:(NSEvent *)e
{
    (void)e;
    cursor.inside = false;
    metal_cursor_sync();
}

/* Keyboard.  Ctrl+Alt+G is the grab toggle and never reaches the guest;
 * so is F13, where a PC keyboard's PrintScreen lands, which takes the
 * screenshot.  Auto-repeat is dropped: the guest does its own. */
- (void)keyDown:(NSEvent *)e
{
    NSUInteger f = [e modifierFlags];

    if (metal_debug()) {
        metal_log("view: keyDown code %u", (unsigned)[e keyCode]);
    }

    if ([e keyCode] == 5 /* G */
        && (f & NSEventModifierFlagControl)
        && (f & NSEventModifierFlagOption)) {
        metal_set_grab(!kbd.on);
        return;
    }
    if ([e keyCode] == 105 /* F13 */) {
        metal_screenshot();
        return;
    }
    if ([e isARepeat]) {
        return;
    }
    m.keys++;
    metal_glue_key(true, [e keyCode]);
}

- (void)keyUp:(NSEvent *)e
{
    if ([e keyCode] == 105) {
        return;
    }
    metal_glue_key(false, [e keyCode]);
}

- (void)flagsChanged:(NSEvent *)e
{
    m.keys++;
    metal_flags_changed(e);
}

/* Mouse.  Middle click captures the pointer (Ctrl+Alt+G or focus loss
 * releases).  Left clicks go straight through to the guest, now that the
 * tablet keeps the two arrows together, so entering the window costs
 * nothing. */
- (void)mouseMoved:(NSEvent *)e       { metal_mouse_move(e); }
- (void)mouseDragged:(NSEvent *)e     { metal_mouse_move(e); }
- (void)rightMouseDragged:(NSEvent *)e { metal_mouse_move(e); }
- (void)otherMouseDragged:(NSEvent *)e { metal_mouse_move(e); }

/*
 * RISC OS wants three buttons -- Select, Menu and Adjust -- and a Mac
 * has one. Control-click is Menu; Command, Option or Shift is Adjust.
 *
 * Three things make it fiddlier than it looks. macOS turns a
 * Control-click into a right click before we ever see it, so Menu has to
 * be recognised on that stream too. The modifier may be let go before
 * the button is, so the button sent is latched at press time and that is
 * what gets released. And the modifier must be taken away from the guest
 * while it is being a button: RISC OS reads Shift-Adjust and Adjust as
 * different gestures, so leaking the Shift makes the click land as
 * something else -- which is how Shift-click came to do nothing at all
 * while Control-click worked.
 *
 * Command is the one to reach for. RISC OS has no Command key, so
 * nothing is lost to it, whereas Shift-Select and Control-Select are
 * gestures the Filer really uses.
 */
static int metal_button_for(NSEvent *e, int plain)
{
    NSUInteger f = [e modifierFlags];

    if (f & NSEventModifierFlagControl) {
        metal_mods_hold(METAL_CTRL_ANY);
        return 1;                       /* Menu */
    }
    if (f & NSEventModifierFlagCommand) {
        metal_mods_hold(METAL_CMD_ANY);
        return 2;                       /* Adjust */
    }
    if (f & NSEventModifierFlagOption) {
        metal_mods_hold(METAL_ALT_ANY);
        return 2;                       /* Adjust */
    }
    if (f & NSEventModifierFlagShift) {
        metal_mods_hold(METAL_SHIFT_ANY);
        return 2;                       /* Adjust */
    }
    return plain;
}

- (void)mouseDown:(NSEvent *)e
{
    mouse.held_left = metal_button_for(e, 0);
    if (metal_debug()) {
        metal_log("view: mouseDown -> button %d (flags %#lx)",
                  mouse.held_left, (unsigned long)[e modifierFlags]);
    }
    metal_glue_mouse_btn(mouse.held_left, true);
    m.mouse_buttons++;
}

- (void)mouseUp:(NSEvent *)e
{
    metal_glue_mouse_btn(mouse.held_left, false);
    metal_mods_restore(e);
}

- (void)rightMouseDown:(NSEvent *)e
{
    mouse.held_right = metal_button_for(e, 2);
    if (metal_debug()) {
        metal_log("view: rightMouseDown -> button %d (flags %#lx)",
                  mouse.held_right, (unsigned long)[e modifierFlags]);
    }
    metal_glue_mouse_btn(mouse.held_right, true);
    m.mouse_buttons++;
}

- (void)rightMouseUp:(NSEvent *)e
{
    metal_glue_mouse_btn(mouse.held_right, false);
    metal_mods_restore(e);
}

/*
 * A real middle button is Menu, not a grab toggle: the Windows front end
 * spends it on capturing the pointer, but the tablet already keeps the
 * two arrows together here, so there is nothing to capture and Menu is
 * the better use of it. Ctrl+Alt+G still grabs.
 */
- (void)otherMouseDown:(NSEvent *)e
{
    if ([e buttonNumber] != 2) {
        return;
    }
    metal_glue_mouse_btn(1, true);
    m.mouse_buttons++;
}

- (void)otherMouseUp:(NSEvent *)e
{
    if ([e buttonNumber] != 2) {
        return;
    }
    metal_glue_mouse_btn(1, false);
}

- (void)scrollWheel:(NSEvent *)e
{
    /* A wheel notch is one line; a trackpad reports fractions of one, so
     * accumulate and send whole notches. */
    static CGFloat acc;
    int notches;

    acc += [e hasPreciseScrollingDeltas] ? [e scrollingDeltaY] / 10.0
                                         : [e scrollingDeltaY];
    notches = (int)acc;
    if (notches) {
        acc -= notches;
        metal_glue_mouse_wheel(notches);
    }
}

@end

/* ------------------------------------------------------------------ */
/* Application delegate: the menu's targets, and the window's fate      */

@interface MetalDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation MetalDelegate

- (void)windowWillClose:(NSNotification *)n
{
    (void)n;
    m.lost = true;
}

- (void)windowDidResignKey:(NSNotification *)n
{
    (void)n;
    metal_set_grab(false);          /* never hold the host pointer hostage */
    metal_release_modifiers();
    cursor.inside = false;          /* and never a cursor, either */
    metal_cursor_sync();
}

- (void)windowDidBecomeKey:(NSNotification *)n
{
    (void)n;
    /* The pointer may already be over the guest's screen: no enter event
     * is coming for a window that was activated under a still cursor. */
    cursor.inside = NSMouseInRect(
        [m.view convertPoint:[m.window mouseLocationOutsideOfEventStream]
                    fromView:nil],
        [m.view bounds], [m.view isFlipped]);
    metal_cursor_sync();
}

- (void)windowDidResize:(NSNotification *)n
{
    (void)n;
    [m.view updateDrawableSize];
}

- (void)quitAction:(id)sender
{
    /* Not [NSApp terminate:]: the machine is asked to power off and the
     * UI loop unwinds, which is what gets the disc written back. */
    (void)sender;
    m.lost = true;
}

- (void)grabAction:(id)sender     { (void)sender; metal_set_grab(!kbd.on); }
- (void)shotAction:(id)sender     { (void)sender; metal_screenshot(); }
- (void)snapshotAction:(id)sender { (void)sender; metal_glue_load_snapshot(); }

- (void)fullScreenAction:(id)sender
{
    (void)sender;
    [m.window toggleFullScreen:nil];
}

@end

static MetalDelegate *delegate;

static void metal_add_item(NSMenu *menu, NSString *title, SEL action,
                           NSString *key, NSEventModifierFlags mods)
{
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title
                                                  action:action
                                           keyEquivalent:key];

    [item setKeyEquivalentModifierMask:mods];
    [item setTarget:delegate];
    [menu addItem:item];
    [item release];
}

static void metal_build_menu(void)
{
    NSMenu *bar = [[NSMenu alloc] init];
    NSMenu *app = [[NSMenu alloc] init];
    NSMenu *machine = [[NSMenu alloc] initWithTitle:@"Machine"];
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    NSMenuItem *machineItem = [[NSMenuItem alloc] init];

    metal_add_item(app, @"Quit RISC OS", @selector(quitAction:), @"q",
                   NSEventModifierFlagCommand);
    [appItem setSubmenu:app];
    [bar addItem:appItem];

    metal_add_item(machine, @"Grab Pointer", @selector(grabAction:), @"g",
                   NSEventModifierFlagControl | NSEventModifierFlagOption);
    metal_add_item(machine, @"Save Screenshot", @selector(shotAction:), @"s",
                   NSEventModifierFlagCommand);
    metal_add_item(machine, @"Load Snapshot", @selector(snapshotAction:), @"",
                   0);
    [machine addItem:[NSMenuItem separatorItem]];
    metal_add_item(machine, @"Toggle Full Screen",
                   @selector(fullScreenAction:), @"f",
                   NSEventModifierFlagCommand | NSEventModifierFlagControl);
    [machineItem setSubmenu:machine];
    [bar addItem:machineItem];

    [NSApp setMainMenu:bar];
    [bar release];
    [app release];
    [machine release];
    [appItem release];
    [machineItem release];
}

static bool metal_create_window(void)
{
    NSRect frame = NSMakeRect(0, 0, 800, 600);
    NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                     | NSWindowStyleMaskMiniaturizable
                     | NSWindowStyleMaskResizable;

    m.window = [[NSWindow alloc] initWithContentRect:frame
                                           styleMask:style
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
    if (!m.window) {
        return false;
    }
    [m.window setTitle:METAL_TITLE];
    [m.window setDelegate:delegate];
    [m.window setAcceptsMouseMovedEvents:YES];
    [m.window setReleasedWhenClosed:NO];
    [m.window center];

    m.view = [[MetalView alloc] initWithFrame:frame];
    [m.view setWantsLayer:YES];
    m.layer = (CAMetalLayer *)[m.view layer];
    [m.window setContentView:m.view];
    [m.window makeFirstResponder:m.view];
    [m.view updateDrawableSize];
    return m.layer != nil;
}

/* ------------------------------------------------------------------ */
/* The boundary, called from ui/metal.c                                */

int metal_backend_init(void)
{
    MTLSamplerDescriptor *sd;

    m.device = MTLCreateSystemDefaultDevice();
    if (!m.device) {
        metal_log("no Metal device");
        return -1;
    }
    metal_log("device: %s", [[m.device name] UTF8String]);

    m.queue = [m.device newCommandQueue];
    if (!m.queue) {
        metal_log("no command queue");
        return -1;
    }

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    delegate = [[MetalDelegate alloc] init];
    [NSApp setDelegate:delegate];
    metal_build_menu();
    metal_script_init();

    if (!metal_create_window()) {
        metal_log("no window");
        return -1;
    }
    if (!metal_compile_library() || !fb_build_scale() || !ptr_build()) {
        return -1;
    }

    sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    m.linear = [m.device newSamplerStateWithDescriptor:sd];
    [sd release];
    if (!m.linear) {
        metal_log("no sampler");
        return -1;
    }

    m.inflight = dispatch_semaphore_create(METAL_RING);
    m.ready = true;
    return 0;
}

int metal_backend_main(void)
{
    if (!m.ready) {
        return 1;
    }

    /* We run our own event pump rather than -[NSApp run], so the frame
     * and the events interleave the way the Windows front end's do.
     * finishLaunching is what -run would have called first. */
    [NSApp finishLaunching];
    [m.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    metal_log("window: visible %d, miniaturized %d, key %d, occlusion %#lx, "
              "drawable %.0fx%.0f",
              (int)[m.window isVisible], (int)[m.window isMiniaturized],
              (int)[m.window isKeyWindow],
              (unsigned long)[m.window occlusionState],
              m.layer.drawableSize.width, m.layer.drawableSize.height);

    while (!m.lost) {
        @autoreleasepool {
            NSEvent *e;

            while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                           untilDate:[NSDate distantPast]
                                              inMode:NSDefaultRunLoopMode
                                             dequeue:YES]) != nil) {
                if (metal_debug()) {
                    NSEventType t = [e type];

                    if (t == NSEventTypeKeyDown || t == NSEventTypeKeyUp
                        || t == NSEventTypeFlagsChanged) {
                        id fr = [m.window firstResponder];

                        metal_log("pump: key event type %ld code %u, "
                                  "our window %d, app active %d, key win %d, "
                                  "first responder %s",
                                  (long)t, (unsigned)[e keyCode],
                                  (int)([e window] == m.window),
                                  (int)[NSApp isActive],
                                  (int)[m.window isKeyWindow],
                                  fr ? [NSStringFromClass([fr class]) UTF8String]
                                     : "(none)");
                    }
                }
                [NSApp sendEvent:e];
            }
            if (m.lost) {
                break;
            }
            /* Nothing is composited for a minimised window, so present
             * nothing.  Occlusion is deliberately *not* consulted: a
             * window can be fully covered and still want frames (a
             * screenshot, a screen recording, the moment it is
             * uncovered), and allowsNextDrawableTimeout is what keeps an
             * unseen window from wedging the loop -- nextDrawable gives
             * up and returns nil rather than blocking for ever. */
            if ([m.window isMiniaturized] || ![m.window isVisible]) {
                usleep(50 * 1000);
                continue;
            }
            metal_render_frame();
        }
    }

    metal_set_grab(false);
    cursor.inside = false;
    metal_cursor_sync();            /* never exit with the cursor hidden */
    metal_backend_request_shutdown();
    metal_glue_fb_done();

    /* Returning from here means returning from qemu_main's stand-in, which
     * unwinds main() while the main-loop thread is inside qemu_cleanup() --
     * the process dies under it.  The main loop calls exit(); outlive it. */
    for (;;) {
        pause();
    }
    return 0;                           /* not reached */
}
