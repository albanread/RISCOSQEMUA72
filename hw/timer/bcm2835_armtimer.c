/*
 * BCM2835 ARM timer, as a latch
 *
 * See include/hw/timer/bcm2835_armtimer.h. Layout from the BCM2835 ARM
 * Peripherals datasheet, section 14. Nothing counts down here: the
 * interrupt comes from the "fire" input, provided the guest has the timer
 * enabled, and is cleared by the IRQ clear register as on hardware.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/timer/bcm2835_armtimer.h"
#include "migration/vmstate.h"

/* Registers, offsets from ARMC + 0x400 */
#define R_LOAD          0x00
#define R_VALUE         0x04
#define R_CONTROL       0x08
#define R_IRQ_CLEAR     0x0c
#define R_RAW_IRQ       0x10
#define R_MASKED_IRQ    0x14
#define R_RELOAD        0x18
#define R_PREDIV        0x1c
#define R_COUNTER       0x20
#define REGION_SIZE     0x40

#define CTRL_IRQ_ENABLE     (1u << 5)
#define CTRL_ENABLE         (1u << 7)
#define CTRL_COUNTER_ENABLE (1u << 9)

/* Reset values, from the datasheet */
#define CTRL_RESET      0x003e0020
#define PREDIV_RESET    0x7d

static void bcm2835_armtimer_update_irq(BCM2835ARMTimerState *s)
{
    qemu_set_irq(s->irq, s->raw_irq && (s->control & CTRL_IRQ_ENABLE));
}

static uint32_t bcm2835_armtimer_counter(BCM2835ARMTimerState *s)
{
    if (!(s->control & CTRL_COUNTER_ENABLE)) {
        return 0;
    }
    return (uint32_t)muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                              s->counter_hz, NANOSECONDS_PER_SECOND);
}

static void bcm2835_armtimer_fire(void *opaque, int n, int level)
{
    BCM2835ARMTimerState *s = opaque;

    if (level && (s->control & CTRL_ENABLE)) {
        s->raw_irq = 1;
        s->fires++;
        bcm2835_armtimer_update_irq(s);
    }
}

static uint64_t bcm2835_armtimer_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    BCM2835ARMTimerState *s = opaque;

    switch (offset) {
    case R_LOAD:
    case R_VALUE:
        return s->load;
    case R_CONTROL:
        return s->control;
    case R_IRQ_CLEAR:
        return 0;
    case R_RAW_IRQ:
        return s->raw_irq;
    case R_MASKED_IRQ:
        return s->raw_irq && (s->control & CTRL_IRQ_ENABLE);
    case R_RELOAD:
        return s->reload;
    case R_PREDIV:
        return s->prediv;
    case R_COUNTER:
        return bcm2835_armtimer_counter(s);
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void bcm2835_armtimer_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    BCM2835ARMTimerState *s = opaque;

    switch (offset) {
    case R_LOAD:
        s->load = s->reload = value;
        break;
    case R_RELOAD:
        s->reload = value;
        break;
    case R_CONTROL:
        s->control = value;
        bcm2835_armtimer_update_irq(s);
        break;
    case R_IRQ_CLEAR:
        s->raw_irq = 0;
        bcm2835_armtimer_update_irq(s);
        break;
    case R_PREDIV:
        s->prediv = value & 0x3ff;
        break;
    case R_VALUE:
    case R_RAW_IRQ:
    case R_MASKED_IRQ:
    case R_COUNTER:
        /* read-only */
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
    }
}

static const MemoryRegionOps bcm2835_armtimer_ops = {
    .read = bcm2835_armtimer_read,
    .write = bcm2835_armtimer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void bcm2835_armtimer_reset(DeviceState *dev)
{
    BCM2835ARMTimerState *s = BCM2835_ARMTIMER(dev);

    s->load = 0;
    s->reload = 0;
    s->control = CTRL_RESET;
    s->prediv = PREDIV_RESET;
    s->raw_irq = 0;
    bcm2835_armtimer_update_irq(s);
}

static void bcm2835_armtimer_init(Object *obj)
{
    BCM2835ARMTimerState *s = BCM2835_ARMTIMER(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2835_armtimer_ops, s,
                          TYPE_BCM2835_ARMTIMER, REGION_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_in_named(DEVICE(obj), bcm2835_armtimer_fire,
                            BCM2835_ARMTIMER_FIRE_IN, 1);
}

static const VMStateDescription vmstate_bcm2835_armtimer = {
    .name = TYPE_BCM2835_ARMTIMER,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(load, BCM2835ARMTimerState),
        VMSTATE_UINT32(reload, BCM2835ARMTimerState),
        VMSTATE_UINT32(control, BCM2835ARMTimerState),
        VMSTATE_UINT32(prediv, BCM2835ARMTimerState),
        VMSTATE_UINT32(raw_irq, BCM2835ARMTimerState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property bcm2835_armtimer_props[] = {
    /* The free-running counter's rate. RISC OS only uses it to measure
     * the interval between vertical syncs, which it then halves. */
    DEFINE_PROP_UINT32("counter-hz", BCM2835ARMTimerState, counter_hz,
                       1000000),
};

static void bcm2835_armtimer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, bcm2835_armtimer_props);
    device_class_set_legacy_reset(dc, bcm2835_armtimer_reset);
    dc->vmsd = &vmstate_bcm2835_armtimer;
}

static const TypeInfo bcm2835_armtimer_info = {
    .name = TYPE_BCM2835_ARMTIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835ARMTimerState),
    .instance_init = bcm2835_armtimer_init,
    .class_init = bcm2835_armtimer_class_init,
};

static void bcm2835_armtimer_register_types(void)
{
    type_register_static(&bcm2835_armtimer_info);
}

type_init(bcm2835_armtimer_register_types)
