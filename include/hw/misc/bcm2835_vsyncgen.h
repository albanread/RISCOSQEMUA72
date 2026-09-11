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

/*
 * One observation of the frame phase, for a display front end that wants
 * to read the guest's framebuffer between the guest's own screen-update
 * flushes rather than on the host's clock.
 *
 * *seq counts vsyncs, so it advances once per guest frame.  *settled is
 * true while the generator is in the first half of a frame: the guest
 * flushed its pending updates at the last half-frame pulse, half a
 * period ago, and will not flush again until the next one, so the
 * framebuffer is as quiet as it ever gets.  Sampling outside that
 * window can catch a flush in progress, which shows up as a window
 * drawn half-moved.
 *
 * Returns false when no generator is running (hz 0), and the caller
 * should then pace itself.  The reads are relaxed and no BQL is
 * required: a front end only needs the counter to advance.
 */
bool bcm2835_vsyncgen_frame_phase(BCM2835VsyncGenState *s,
                                  uint64_t *seq, bool *settled);

#endif
