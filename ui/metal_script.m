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
 * riscos-pi4/app/RISCOSQEMU.sdef, hand-written until E1 generates it
 * from the C command table.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#import <Cocoa/Cocoa.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include "ui/metal.h"

/* The suite and command codes, allocated in the sdef.  Mixed case on
 * purpose: Apple reserves all-lowercase four-character codes. */
#define AE_SUITE 'MQem'
#define AE_PING  'Ping'

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

    /* The direct parameter is the JSON command object; omitted means
     * ping, which is every command E0 has. */
    direct = [event descriptorForKeyword:keyDirectObject];
    if (direct) {
        cmd = [direct stringValue];
    }
    metal_log("apple event: command %s",
              cmd ? [cmd UTF8String] : "(ping)");

    metal_glue_script(cmd ? [cmd UTF8String] : "{\"cmd\":\"ping\"}", &out);
    result = [NSAppleEventDescriptor
        descriptorWithString:[NSString stringWithUTF8String:
            out ? out
               : "{\"ok\":false,\"error\":{\"code\":\"not-capable\","
                  "\"number\":5,\"message\":\"no reply\"}}"]];
    g_free(out);
    [reply setDescriptor:result forKeyword:keyDirectObject];
}

@end

void metal_script_init(void)
{
    /* The dev kill switch; the settings file and the display suboption
     * arrive with the rest of the surface (SCRIPTING.md section 7). */
    if (getenv("RISCOSQEMU_SCRIPTING_OFF")) {
        metal_log("apple events: off (RISCOSQEMU_SCRIPTING_OFF)");
        return;
    }

    g_handler = [[MetalScriptHandler alloc] init];
    [[NSAppleEventManager sharedAppleEventManager]
        setEventHandler:g_handler
            andSelector:@selector(handleEvent:withReplyEvent:)
        forEventClass:AE_SUITE andEventID:AE_PING];
    metal_log("apple events: 'MQem' handler registered");
}
