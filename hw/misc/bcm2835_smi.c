/*
 * BCM2835 SMI (secondary memory interface): the vertical-sync latch
 *
 * See include/hw/misc/bcm2835_smi.h. Register layout from the BCM2835 ARM
 * Peripherals datasheet and the Linux bcm2835_smi driver; only CS does
 * anything here.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/misc/bcm2835_smi.h"
#include "migration/vmstate.h"

#define SMI_CS          0x00
#define SMI_CS_DONE     (1u << 1)
#define SMI_REGION_SIZE 0x100

static void bcm2835_smi_update(BCM2835SMIState *s)
{
    qemu_set_irq(s->irq, (s->cs & SMI_CS_DONE) != 0);
}

static uint64_t bcm2835_smi_read(void *opaque, hwaddr offset, unsigned size)
{
    BCM2835SMIState *s = opaque;

    if (offset == SMI_CS) {
        return s->cs;
    }
    qemu_log_mask(LOG_UNIMP, "%s: read of unimplemented register 0x%"
                  HWADDR_PRIx "\n", __func__, offset);
    return 0;
}

static void bcm2835_smi_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    BCM2835SMIState *s = opaque;

    if (offset == SMI_CS) {
        /* Any write acknowledges the vsync: RISC OS writes zero, and the
         * datasheet's write-one-to-clear lands in the same place. */
        s->cs = value & ~SMI_CS_DONE;
        bcm2835_smi_update(s);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "%s: write of unimplemented register 0x%"
                  HWADDR_PRIx "\n", __func__, offset);
}

static const MemoryRegionOps bcm2835_smi_ops = {
    .read = bcm2835_smi_read,
    .write = bcm2835_smi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void bcm2835_smi_vsync(void *opaque, int n, int level)
{
    BCM2835SMIState *s = opaque;

    if (level) {
        s->cs |= SMI_CS_DONE;
        s->vsyncs++;
        bcm2835_smi_update(s);
    }
}

static void bcm2835_smi_reset(DeviceState *dev)
{
    BCM2835SMIState *s = BCM2835_SMI(dev);

    s->cs = 0;
    s->vsyncs = 0;
    bcm2835_smi_update(s);
}

static void bcm2835_smi_init(Object *obj)
{
    BCM2835SMIState *s = BCM2835_SMI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2835_smi_ops, s,
                          TYPE_BCM2835_SMI, SMI_REGION_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_in_named(DEVICE(obj), bcm2835_smi_vsync,
                            BCM2835_SMI_VSYNC_IN, 1);
}

static const VMStateDescription vmstate_bcm2835_smi = {
    .name = TYPE_BCM2835_SMI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cs, BCM2835SMIState),
        VMSTATE_UINT64(vsyncs, BCM2835SMIState),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2835_smi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, bcm2835_smi_reset);
    dc->vmsd = &vmstate_bcm2835_smi;
}

static const TypeInfo bcm2835_smi_info = {
    .name = TYPE_BCM2835_SMI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835SMIState),
    .instance_init = bcm2835_smi_init,
    .class_init = bcm2835_smi_class_init,
};

static void bcm2835_smi_register_types(void)
{
    type_register_static(&bcm2835_smi_info);
}

type_init(bcm2835_smi_register_types)
