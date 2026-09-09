/*
 * BCM2835 VideoCore mailbox channel 0 — power management.
 *
 * The mailbox device routes a message whose low nibble is 0 to this channel.
 * The remaining 28 bits are a bitmask of peripherals the ARM wants powered;
 * the VideoCore firmware replies on the same channel with the resulting power
 * state. Since every device we model is always powered, the reply is the
 * request.
 *
 * Guests that use the property interface (tag 0x00028001, set power state)
 * never come here — Linux is one. RISC OS 5's BCM2835 HAL powers the USB host
 * controller through this legacy channel during HAL_InitDevices and polls
 * MAIL0_STATUS with no timeout, so without a peer on channel 0 it never
 * finishes booting.
 *
 * Refs: https://github.com/raspberrypi/firmware/wiki/Mailboxes
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/misc/bcm2835_mbox_defs.h"
#include "hw/misc/bcm2835_mbox_power.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

static uint64_t bcm2835_mbox_power_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    BCM2835MboxPowerState *s = opaque;
    uint32_t res = 0;

    switch (offset) {
    case MBOX_AS_DATA:
        res = s->state | MBOX_CHAN_POWER;
        s->pending = false;
        qemu_set_irq(s->mbox_irq, 0);
        break;

    case MBOX_AS_PENDING:
        res = s->pending;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        return 0;
    }

    return res;
}

static void bcm2835_mbox_power_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    BCM2835MboxPowerState *s = opaque;

    switch (offset) {
    case MBOX_AS_DATA:
        /* bcm2835_mbox checks our pending status before pushing */
        assert(!s->pending);
        /*
         * Everything we model is always on, so the new state is whatever was
         * asked for. Keep it so a read back reports the same mask.
         */
        s->state = value & ~0xfu;
        s->pending = true;
        qemu_set_irq(s->mbox_irq, 1);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        return;
    }
}

static const MemoryRegionOps bcm2835_mbox_power_ops = {
    .read = bcm2835_mbox_power_read,
    .write = bcm2835_mbox_power_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const VMStateDescription vmstate_bcm2835_mbox_power = {
    .name = TYPE_BCM2835_MBOX_POWER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(state, BCM2835MboxPowerState),
        VMSTATE_BOOL(pending, BCM2835MboxPowerState),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2835_mbox_power_init(Object *obj)
{
    BCM2835MboxPowerState *s = BCM2835_MBOX_POWER(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2835_mbox_power_ops, s,
                          TYPE_BCM2835_MBOX_POWER, 0x10);

    /*
     * As for bcm2835_property: these ops are reached from bcm2835_mbox, which
     * in turn reads back from this region.
     */
    s->iomem.disable_reentrancy_guard = true;

    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->mbox_irq);
}

static void bcm2835_mbox_power_reset(DeviceState *dev)
{
    BCM2835MboxPowerState *s = BCM2835_MBOX_POWER(dev);

    s->state = 0;
    s->pending = false;
}

static void bcm2835_mbox_power_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, bcm2835_mbox_power_reset);
    dc->vmsd = &vmstate_bcm2835_mbox_power;
}

static const TypeInfo bcm2835_mbox_power_info = {
    .name          = TYPE_BCM2835_MBOX_POWER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835MboxPowerState),
    .class_init    = bcm2835_mbox_power_class_init,
    .instance_init = bcm2835_mbox_power_init,
};

static void bcm2835_mbox_power_register_types(void)
{
    type_register_static(&bcm2835_mbox_power_info);
}

type_init(bcm2835_mbox_power_register_types)
