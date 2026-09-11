/*
 * metal_script.m — the Apple Events surface, Cocoa side.
 *
 * Sprint E0 of riscos-pi4/SCRIPTING.md: prove that Apple Events reach a
 * handler through the front end's hand-run event pump (open question
 * Q1), and put a ping on the wire whose round trip can be measured
 * against the QMP socket (Q2).  The handler contract is
 * NSAppleEventManager's: it fires on the main thread inside the pump's
 * [NSApp sendEvent:], and the reply is set on the reply event's direct
 * object.
 *
 * The commands themselves live on the C side (metal_glue_script) with
 * everything else that talks to QEMU; this file only marshals.  The
 * dictionary this surface is described by is
 * riscos-pi4/app/RISCOSQEMU.sdef, generated from the C command table
 * by riscos-pi4/tools/mksdef.py.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#import <Cocoa/Cocoa.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include "ui/metal.h"

/* The suite and command codes come from the C table -- one handler per
 * event id, because NSAppleEventManager dispatches exact (class, id)
 * pairs and a handler for one id does not serve another in the same
 * suite.  The standard quit event is registered beside them and routed
 * by the C side to the clean power off.  Mixed case on purpose: Apple
 * reserves all-lowercase codes. */
static uint32_t g_classes[32];
static uint32_t g_ids[32];
static size_t g_ncodes;

static id g_handler;

@interface MetalScriptHandler : NSObject
- (void)handleEvent:(NSAppleEventDescriptor *)event
     withReplyEvent:(NSAppleEventDescriptor *)reply;
@end

@implementation MetalScriptHandler

- (void)handleEvent:(NSAppleEventDescriptor *)event
     withReplyEvent:(NSAppleEventDescriptor *)reply
{
    NSString *cmd = nil;
    NSAppleEventDescriptor *direct, *result;
    char *out = NULL;

    /* The direct parameter, when present, is a JSON object of
     * arguments; the event id itself names the command. */
    direct = [event descriptorForKeyword:keyDirectObject];
    if (direct) {
        cmd = [direct stringValue];
    }
    metal_log("apple event: command %s (event class 0x%08x id 0x%08x)",
              cmd ? [cmd UTF8String] : "(by event id)",
              (unsigned)event.eventClass, (unsigned)event.eventID);

    metal_glue_script(event.eventClass, event.eventID,
                      cmd ? [cmd UTF8String] : NULL, &out);
    result = [NSAppleEventDescriptor
        descriptorWithString:[NSString stringWithUTF8String:
            out ? out
               : "{\"ok\":false,\"error\":{\"code\":\"not-capable\","
                  "\"number\":5,\"message\":\"no reply\"}}"]];
    g_free(out);
    [reply setDescriptor:result forKeyword:keyDirectObject];
    metal_log("apple event: replied %zu bytes", strlen(out ?: ""));
}

@end

/* Idempotent: called at backend init and again once the pump has run
 * finishLaunching, in case AppKit's own scripting initialisation (the
 * NSAppleScriptEnabled machinery) installs its handler set after ours
 * and pre-empts it. */
void metal_script_register(void)
{
    NSAppleEventManager *em = [NSAppleEventManager sharedAppleEventManager];
    size_t n, i;

    if (getenv("RISCOSQEMU_SCRIPTING_OFF")) {
        return;
    }
    if (!g_handler) {
        g_handler = [[MetalScriptHandler alloc] init];
    }
    if (!g_ncodes) {
        g_ncodes = metal_glue_script_events(g_classes, g_ids, 32);
    }
    n = g_ncodes;
    for (i = 0; i < n; i++) {
        [em setEventHandler:g_handler
                  andSelector:@selector(handleEvent:withReplyEvent:)
              forEventClass:g_classes[i] andEventID:g_ids[i]];
    }
    /* aevt/quit: the standard event AppleScript itself sends for
     * quit.  Without this, the hand-run NSApplication would take the
     * default terminate path and bypass the disc write-back; the C
     * side routes it beside poweroff. */
    [em setEventHandler:g_handler
              andSelector:@selector(handleEvent:withReplyEvent:)
            forEventClass:'aevt' andEventID:'quit'];
    metal_log("apple events: %zu handler%s registered", n + 1,
              n ? "s" : "");
}
