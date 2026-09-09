/*
 * BCM2711 (Raspberry Pi 4) legacy interrupt controller
 *
 * Layout from the BCM2711 ARM Peripherals datasheet, section 6.5.3, relative
 * to ARMC+0x200:
 *
 *   0x000 + 0x40 * n   IRQ bank, core n      0x100 + 0x40 * n   FIQ bank, core n
 *
 * Every bank holds PENDING0-2 at +0x00, SET_EN_0-2 at +0x10 and CLR_EN_0-2
 * at +0x20; the raw lines, IRQ_STATUS0-2, sit at +0x30 of the first bank
 * only. A bank's output is the OR of its enabled sources, and the pending
 * registers show only enabled ones. There is nothing to acknowledge: a
 * source stays pending until the device drops its line.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/intc/bcm2838_ic.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"

#define REG_PENDING(w)      (0x00 + 4 * (w))
#define REG_SET_EN(w)       (0x10 + 4 * (w))
#define REG_CLR_EN(w)       (0x20 + 4 * (w))
#define REG_STATUS(w)       (0x30 + 4 * (w))

/*
 * Word 2: sixteen sources, then two bits summarising the other words, then
 * a copy of the core's interrupt line. Only the sources gate anything.
 */
#define WORD2_SOURCES       0x0000ffff
#define WORD2_INT31_0       (1u << 24)
#define WORD2_INT63_32      (1u << 25)
#define WORD2_LINE          (1u << 31)

static bool bcm2838_ic_bank_active(BCM2838ICState *s, int bank)
{
    return (s->level[0] & s->enable[bank][0])
        || (s->level[1] & s->enable[bank][1])
        || (s->level[2] & s->enable[bank][2] & WORD2_SOURCES);
}

static uint32_t bcm2838_ic_pending(BCM2838ICState *s, int bank, int word)
{
    uint32_t pending = s->level[word] & s->enable[bank][word];

    if (word == 2) {
        pending &= WORD2_SOURCES;
        if (s->level[0] & s->enable[bank][0]) {
            pending |= WORD2_INT31_0;
        }
        if (s->level[1] & s->enable[bank][1]) {
            pending |= WORD2_INT63_32;
        }
        if (bcm2838_ic_bank_active(s, bank)) {
            pending |= WORD2_LINE;
        }
    }
    return pending;
}

static void bcm2838_ic_update(BCM2838ICState *s)
{
    for (int bank = 0; bank < BCM2838_IC_NUM_BANKS; bank++) {
        qemu_irq out = bank < BCM2838_IC_NUM_CORES
                       ? s->irq[bank] : s->fiq[bank - BCM2838_IC_NUM_CORES];

        qemu_set_irq(out, bcm2838_ic_bank_active(s, bank));
    }
}

static void bcm2838_ic_set_irq(void *opaque, int irq, int level)
{
    BCM2838ICState *s = opaque;

    trace_bcm2838_ic_set_irq(irq, level);
    s->level[irq >> 5] = deposit32(s->level[irq >> 5], irq & 31, 1,
                                   level != 0);
    bcm2838_ic_update(s);
}

static uint64_t bcm2838_ic_read(void *opaque, hwaddr offset, unsigned size)
{
    BCM2838ICState *s = opaque;
    int bank = offset >> 6;
    int reg = offset & 0x3c;
    int word = (reg >> 2) & 3;

    switch (reg) {
    case REG_PENDING(0) ... REG_PENDING(2):
        return bcm2838_ic_pending(s, bank, word);
    case REG_SET_EN(0) ... REG_SET_EN(2):
    case REG_CLR_EN(0) ... REG_CLR_EN(2):
        return s->enable[bank][word];
    case REG_STATUS(0) ... REG_STATUS(2):
        if (bank == 0) {
            return s->level[word];
        }
        break;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"HWADDR_PRIx"\n",
                  __func__, offset);
    return 0;
}

static void bcm2838_ic_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    BCM2838ICState *s = opaque;
    int bank = offset >> 6;
    int reg = offset & 0x3c;
    int word = (reg >> 2) & 3;
    uint32_t writable = word == 2 ? WORD2_SOURCES | WORD2_LINE : 0xffffffff;

    trace_bcm2838_ic_write(offset, value);
    switch (reg) {
    case REG_SET_EN(0) ... REG_SET_EN(2):
        s->enable[bank][word] |= value & writable;
        break;
    case REG_CLR_EN(0) ... REG_CLR_EN(2):
        s->enable[bank][word] &= ~(value & writable);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x%"HWADDR_PRIx"\n",
                      __func__, offset);
        return;
    }
    bcm2838_ic_update(s);
}

static const MemoryRegionOps bcm2838_ic_ops = {
    .read = bcm2838_ic_read,
    .write = bcm2838_ic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    /* RISC OS reads the low byte of PENDING2 with LDRB */
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void bcm2838_ic_reset(DeviceState *dev)
{
    BCM2838ICState *s = BCM2838_IC(dev);

    memset(s->enable, 0, sizeof(s->enable));
}

static void bcm2838_ic_init(Object *obj)
{
    BCM2838ICState *s = BCM2838_IC(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2838_ic_ops, s, TYPE_BCM2838_IC,
                          BCM2838_IC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);

    qdev_init_gpio_in(dev, bcm2838_ic_set_irq, BCM2838_IC_NUM_IRQS);
    qdev_init_gpio_out_named(dev, s->irq, BCM2838_IC_IRQ_OUT,
                             BCM2838_IC_NUM_CORES);
    qdev_init_gpio_out_named(dev, s->fiq, BCM2838_IC_FIQ_OUT,
                             BCM2838_IC_NUM_CORES);
}

static const VMStateDescription vmstate_bcm2838_ic = {
    .name = TYPE_BCM2838_IC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(level, BCM2838ICState, 3),
        VMSTATE_UINT32_2DARRAY(enable, BCM2838ICState, BCM2838_IC_NUM_BANKS, 3),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2838_ic_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, bcm2838_ic_reset);
    dc->vmsd = &vmstate_bcm2838_ic;
}

static const TypeInfo bcm2838_ic_info = {
    .name          = TYPE_BCM2838_IC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2838ICState),
    .class_init    = bcm2838_ic_class_init,
    .instance_init = bcm2838_ic_init,
};

static void bcm2838_ic_register_types(void)
{
    type_register_static(&bcm2838_ic_info);
}

type_init(bcm2838_ic_register_types)
