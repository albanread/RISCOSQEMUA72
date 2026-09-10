/*
 * BCM2835 vertical-sync generator
 *
 * What the VideoCore firmware does for the ARM on a real Pi, done by the
 * high-resolution timer thread: a vertical sync at a fixed rate, and
 * halfway through each frame the pulse RISC OS's video driver arms the
 * ARM timer for, so its pending screen updates go out mid-frame. Two GPIO
 * outputs, "vsync" and "half-frame"; the SoC wires them to the SMI latch
 * and the ARM timer latch.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef BCM2835_VSYNCGEN_H
#define BCM2835_VSYNCGEN_H

#include "hw/core/qdev.h"
#include "hw/core/irq.h"
#include "qemu/hrtimer.h"
#include "qom/object.h"

#define TYPE_BCM2835_VSYNCGEN "bcm2835-vsyncgen"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835VsyncGenState, BCM2835_VSYNCGEN)

#define BCM2835_VSYNCGEN_VSYNC_OUT      "vsync"
#define BCM2835_VSYNCGEN_HALF_FRAME_OUT "half-frame"

struct BCM2835VsyncGenState {
    /*< private >*/
    DeviceState parent_obj;
    /*< public >*/
    uint32_t hz;                /* property; 0 = no vsync at all */
    qemu_irq vsync;
    qemu_irq half_frame;
    HRTimer *timer;
    int64_t next_vsync_ns;      /* on QEMU_CLOCK_VIRTUAL */
    bool half_next;             /* the pending expiry is the half-frame */
    uint64_t vsyncs;            /* delivered since start */
};

/* Change the rate while running; 0 stops it. BQL held. */
void bcm2835_vsyncgen_set_hz(BCM2835VsyncGenState *s, uint32_t hz);

#endif
