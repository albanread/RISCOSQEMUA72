/*
 * BCM2835 vertical-sync generator
 *
 * See include/hw/misc/bcm2835_vsyncgen.h. The schedule is two expiries
 * per frame on the high-resolution timer thread: the vsync at T, the
 * half-frame pulse at T + period/2. Both are pulses; the latches they
 * land on hold them until the guest acknowledges.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/bcm2835_vsyncgen.h"

static int64_t bcm2835_vsyncgen_period(BCM2835VsyncGenState *s)
{
    return NANOSECONDS_PER_SECOND / s->hz;
}

static void bcm2835_vsyncgen_expire(void *opaque)
{
    BCM2835VsyncGenState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t period;

    if (!s->hz) {
        return;
    }
    period = bcm2835_vsyncgen_period(s);
    if (s->half_next) {
        qemu_irq_pulse(s->half_frame);
        s->half_next = false;
        hrtimer_mod_ns(s->timer, s->next_vsync_ns);
        return;
    }
    qemu_irq_pulse(s->vsync);
    s->vsyncs++;
    s->next_vsync_ns += period;
    if (s->next_vsync_ns < now) {
        /* fell behind (machine stopped, host stalled): no catch-up burst */
        s->next_vsync_ns = now + period;
    }
    s->half_next = true;
    hrtimer_mod_ns(s->timer, s->next_vsync_ns - period / 2);
}

static void bcm2835_vsyncgen_restart(BCM2835VsyncGenState *s)
{
    hrtimer_del(s->timer);
    s->half_next = false;
    if (!s->hz) {
        return;
    }
    s->next_vsync_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                       + bcm2835_vsyncgen_period(s);
    hrtimer_mod_ns(s->timer, s->next_vsync_ns);
}

void bcm2835_vsyncgen_set_hz(BCM2835VsyncGenState *s, uint32_t hz)
{
    s->hz = hz;
    bcm2835_vsyncgen_restart(s);
}

bool bcm2835_vsyncgen_frame_phase(BCM2835VsyncGenState *s,
                                  uint64_t *seq, bool *settled)
{
    if (!s || !qatomic_read(&s->hz)) {
        return false;               /* no vsync: the caller paces itself */
    }
    /*
     * half_next is set by the vsync pulse and cleared by the half-frame
     * pulse, so it is true exactly while the generator is in the first
     * half of a frame -- after the guest's last flush and before its
     * next.  Both reads are relaxed: the front end only needs the
     * counter to advance, and a phase read one frame stale merely costs
     * it a frame of latency, never correctness.
     */
    *seq = qatomic_read__nocheck(&s->vsyncs);
    *settled = qatomic_read(&s->half_next);
    return true;
}

static void bcm2835_vsyncgen_realize(DeviceState *dev, Error **errp)
{
    BCM2835VsyncGenState *s = BCM2835_VSYNCGEN(dev);

    s->timer = hrtimer_new(bcm2835_vsyncgen_expire, s);
    bcm2835_vsyncgen_restart(s);
}

static void bcm2835_vsyncgen_init(Object *obj)
{
    BCM2835VsyncGenState *s = BCM2835_VSYNCGEN(obj);

    qdev_init_gpio_out_named(DEVICE(obj), &s->vsync,
                             BCM2835_VSYNCGEN_VSYNC_OUT, 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->half_frame,
                             BCM2835_VSYNCGEN_HALF_FRAME_OUT, 1);
}

static const Property bcm2835_vsyncgen_props[] = {
    DEFINE_PROP_UINT32("hz", BCM2835VsyncGenState, hz, 30),
};

static void bcm2835_vsyncgen_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bcm2835_vsyncgen_realize;
    device_class_set_props(dc, bcm2835_vsyncgen_props);
    /* not user-creatable: the SoC owns it */
    dc->user_creatable = false;
}

static const TypeInfo bcm2835_vsyncgen_info = {
    .name = TYPE_BCM2835_VSYNCGEN,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(BCM2835VsyncGenState),
    .instance_init = bcm2835_vsyncgen_init,
    .class_init = bcm2835_vsyncgen_class_init,
};

static void bcm2835_vsyncgen_register_types(void)
{
    type_register_static(&bcm2835_vsyncgen_info);
}

type_init(bcm2835_vsyncgen_register_types)
