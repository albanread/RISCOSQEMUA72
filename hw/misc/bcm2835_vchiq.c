/*
 * BCM2835 VideoCore mailbox channel 3 - VCHIQ.
 *
 * Just enough of the VideoCore's half of VCHIQ to let a guest finish
 * connecting and then get on with its life. This is not a VideoCore: it
 * completes the initialisation handshake, answers the guest's CONNECT, and
 * refuses every service the guest then tries to open.
 *
 * That refusal is the point. RISC OS 5 runs BCMSound before its video driver,
 * and BCMSound's module init calls VCHIQ_Connect unconditionally and waits on
 * a semaphore with no timeout, no deadline and no register to poll -- so a
 * guest whose channel-3 message goes unanswered stops there forever, and the
 * video driver never even starts. Answering the connect and declining the
 * services gets the boot moving again, leaves the display to the property
 * channel, and costs a fraction of what emulating the real interface would.
 *
 * The protocol is not documented by Broadcom; the Linux driver under
 * drivers/staging/vc04_services is the de facto specification, and the
 * structure offsets here were additionally read out of a live guest.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/misc/bcm2835_mbox_defs.h"
#include "hw/misc/bcm2835_vchiq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"

static uint32_t vchiq_ld(BCM2835VchiqState *s, uint32_t addr)
{
    return ldl_le_phys(&s->dma_as, addr);
}

static void vchiq_st(BCM2835VchiqState *s, uint32_t addr, uint32_t val)
{
    stl_le_phys(&s->dma_as, addr, val);
}

/* Fire one of the guest's own remote events and ring the VC->ARM doorbell */
static void vchiq_signal_guest(BCM2835VchiqState *s, uint32_t event)
{
    vchiq_st(s, event + VCHIQ_EV_FIRED, 1);
    s->bell0 |= VCHIQ_BELL_RUNG;
    trace_bcm2835_vchiq_bell(s->bell0);
    qemu_set_irq(s->bell_irq, 1);
}

/* Append a message to our slot and return true if it fitted */
static bool vchiq_queue_msg(BCM2835VchiqState *s, uint32_t msgid, uint32_t size)
{
    uint32_t stride = QEMU_ALIGN_UP(size + VCHIQ_MSG_HDR_SIZE, 8);
    uint32_t hdr;

    if (s->tx_pos + stride > s->slot_size) {
        /*
         * We never recycle slots: this peer only ever emits one CONNECT and a
         * handful of CLOSEs, which is a few dozen bytes of a 4K slot. Running
         * out means the guest is doing something this model was not built for,
         * and silently wrapping would corrupt its ring.
         */
        qemu_log_mask(LOG_UNIMP, "%s: message slot full, dropping msgid 0x%08x\n",
                      __func__, msgid);
        return false;
    }

    hdr = s->slot0 + s->tx_slot * s->slot_size + s->tx_pos;
    vchiq_st(s, hdr, msgid);
    vchiq_st(s, hdr + 4, size);
    s->tx_pos += stride;
    vchiq_st(s, s->master_base + VCHIQ_SS_TX_POS, s->tx_pos);

    trace_bcm2835_vchiq_tx(msgid, VCHIQ_MSG_TYPE(msgid), size);
    return true;
}

/*
 * The guest has handed us slot zero. Set up our half of it, answer its
 * CONNECT, and wake it.
 */
static void vchiq_init_from_slot_zero(BCM2835VchiqState *s, uint32_t value)
{
    uint32_t magic, slot_zero_size, max_slots, per_side, shared_size;
    uint32_t slot_first, slot_last, i;

    /*
     * The low nibble carried the channel; the rest is a VideoCore bus
     * address. Which 1GB alias it uses depends on the SoC (0x40000000 on a
     * BCM2835, 0xC0000000 afterwards) and dma_as covers them all, so do not
     * second-guess it: the magic word is the real test of whether this is
     * pointing at a slot zero.
     */
    s->slot0 = value & ~0xfu;
    if (!s->slot0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: null slot zero address\n",
                      __func__);
        return;
    }

    magic = vchiq_ld(s, s->slot0 + VCHIQ_SZ_MAGIC);
    slot_zero_size = vchiq_ld(s, s->slot0 + VCHIQ_SZ_SLOT_ZERO_SIZE);
    s->slot_size = vchiq_ld(s, s->slot0 + VCHIQ_SZ_SLOT_SIZE);
    max_slots = vchiq_ld(s, s->slot0 + VCHIQ_SZ_MAX_SLOTS);
    per_side = vchiq_ld(s, s->slot0 + VCHIQ_SZ_MAX_SLOTS_PER_SIDE);

    trace_bcm2835_vchiq_slot_zero(s->slot0, magic, s->slot_size, max_slots);

    /*
     * Everything from here on indexes guest memory using numbers the guest
     * gave us, so each one is bounded before it is used. A slot is one page
     * in every implementation, but allow a range rather than insist.
     */
    if (magic != VCHIQ_SLOT_MAGIC ||
        s->slot_size < 2 * VCHIQ_MSG_HDR_SIZE || s->slot_size > 0x10000 ||
        !max_slots || max_slots > 4096 ||
        !per_side || per_side > max_slots) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: slot zero at 0x%08x does not look like VCHIQ "
                      "(magic 0x%08x, slot_size %u, max_slots %u, per_side %u)\n",
                      __func__, s->slot0, magic, s->slot_size, max_slots,
                      per_side);
        s->slot0 = 0;
        return;
    }
    s->max_slots = max_slots;
    s->per_side = per_side;

    /*
     * The debug array is last in each shared state and its size is a build
     * option, so derive the stride from slot_zero_size rather than assume it.
     * It must at least hold the fixed fields and the slot queue.
     */
    if (slot_zero_size < VCHIQ_SZ_MASTER + max_slots * 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: slot_zero_size %u too small\n",
                      __func__, slot_zero_size);
        s->slot0 = 0;
        return;
    }
    shared_size = (slot_zero_size - VCHIQ_SZ_MASTER - max_slots * 4) / 2;
    if (shared_size < VCHIQ_SS_SLOT_QUEUE + per_side * 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: shared state of %u bytes cannot hold a %u-entry "
                      "slot queue\n", __func__, shared_size, per_side);
        s->slot0 = 0;
        return;
    }
    s->master_base = s->slot0 + VCHIQ_SZ_MASTER;
    s->slave_base = s->master_base + shared_size;

    slot_first = vchiq_ld(s, s->master_base + VCHIQ_SS_SLOT_FIRST);
    slot_last = vchiq_ld(s, s->master_base + VCHIQ_SS_SLOT_LAST);
    if (slot_last < slot_first || slot_last >= max_slots ||
        slot_last - slot_first + 1 > per_side) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad slot range %u..%u for a "
                      "%u-entry queue\n", __func__, slot_first, slot_last,
                      per_side);
        s->slot0 = 0;
        return;
    }

    /*
     * The guest zeroes our slot queue and leaves it for us to fill. It indexes
     * straight into it when parsing our messages, so leaving it at zero would
     * have it read slot zero itself as message data.
     */
    for (i = 0; i <= slot_last - slot_first; i++) {
        vchiq_st(s, s->master_base + VCHIQ_SS_SLOT_QUEUE + i * 4,
                 slot_first + i);
    }
    vchiq_st(s, s->master_base + VCHIQ_SS_SLOT_QUEUE_RECYCLE,
             slot_last - slot_first + 1);

    s->tx_slot = slot_first;
    s->tx_pos = 0;
    s->rx_pos = 0;

    /*
     * Arm our trigger before anything else: the guest only rings the ARM->VC
     * doorbell when it sees this armed, so without it we would never hear
     * about the messages it queues.
     */
    vchiq_st(s, s->master_base + VCHIQ_SS_TRIGGER + VCHIQ_EV_ARMED, 1);
    vchiq_st(s, s->master_base + VCHIQ_SS_INITIALISED, 1);

    /*
     * Answer the connect. The guest's wait is on a counting semaphore, so it
     * does not matter whether it is already blocked or has not got there yet.
     */
    if (vchiq_queue_msg(s, VCHIQ_MAKE_MSG(VCHIQ_MSG_CONNECT, 0, 0), 0)) {
        s->connected = true;
        vchiq_signal_guest(s, s->slave_base + VCHIQ_SS_TRIGGER);
    }
}

/* Drain the messages the guest has queued for us since we last looked */
static void vchiq_parse_guest_messages(BCM2835VchiqState *s)
{
    uint32_t tx_pos, replies = 0;
    int guard = 256;

    if (!s->slot0) {
        return;
    }

    /* Consume our trigger but stay armed, so the guest keeps ringing */
    vchiq_st(s, s->master_base + VCHIQ_SS_TRIGGER + VCHIQ_EV_FIRED, 0);
    vchiq_st(s, s->master_base + VCHIQ_SS_TRIGGER + VCHIQ_EV_ARMED, 1);

    tx_pos = vchiq_ld(s, s->slave_base + VCHIQ_SS_TX_POS);

    while (s->rx_pos != tx_pos && guard-- > 0) {
        uint32_t qidx = (s->rx_pos / s->slot_size) % s->per_side;
        uint32_t slot = vchiq_ld(s, s->slave_base + VCHIQ_SS_SLOT_QUEUE
                                    + qidx * 4);
        uint32_t hdr, msgid, size, type;

        /* The queue entry and the header both come from the guest */
        if (slot >= s->max_slots) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: slot queue entry %u out of "
                          "range\n", __func__, slot);
            break;
        }
        hdr = s->slot0 + slot * s->slot_size + (s->rx_pos % s->slot_size);
        msgid = vchiq_ld(s, hdr);
        size = vchiq_ld(s, hdr + 4);
        type = VCHIQ_MSG_TYPE(msgid);
        if (size > s->slot_size - VCHIQ_MSG_HDR_SIZE) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: message of %u bytes cannot "
                          "fit a slot\n", __func__, size);
            break;
        }

        s->rx_pos += QEMU_ALIGN_UP(size + VCHIQ_MSG_HDR_SIZE, 8);

        switch (type) {
        case VCHIQ_MSG_OPEN:
        {
            /*
             * Payload is { fourcc, client_id, version, version_min }. We have
             * no services, so close it straight back; the guest treats that as
             * a clean refusal and carries on.
             */
            uint32_t fourcc = vchiq_ld(s, hdr + VCHIQ_MSG_HDR_SIZE);
            uint32_t srcport = VCHIQ_MSG_SRCPORT(msgid);

            trace_bcm2835_vchiq_open(fourcc, srcport);
            replies += vchiq_queue_msg(s,
                VCHIQ_MAKE_MSG(VCHIQ_MSG_CLOSE, 0, srcport), 0);
            break;
        }
        case VCHIQ_MSG_CONNECT:
        case VCHIQ_MSG_CLOSE:
        case VCHIQ_MSG_PADDING:
            trace_bcm2835_vchiq_rx(msgid, type, size);
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "%s: unhandled VCHIQ message type %u "
                          "(msgid 0x%08x)\n", __func__, type, msgid);
            break;
        }
    }

    if (replies) {
        vchiq_signal_guest(s, s->slave_base + VCHIQ_SS_TRIGGER);
    }
}

/* The mailbox channel-3 window */

static uint64_t bcm2835_vchiq_mbox_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    switch (offset) {
    case MBOX_AS_DATA:
        /*
         * VCHIQ messages are fire and forget: the guest never reads a reply
         * from the mailbox, and queueing one here would fill a FIFO nothing
         * drains, eventually blocking the property channel too.
         */
        return 0;

    case MBOX_AS_PENDING:
        return 0;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        return 0;
    }
}

static void bcm2835_vchiq_mbox_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    BCM2835VchiqState *s = opaque;

    switch (offset) {
    case MBOX_AS_DATA:
        trace_bcm2835_vchiq_mbox_write((uint32_t)value);
        vchiq_init_from_slot_zero(s, (uint32_t)value);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        break;
    }
}

static const MemoryRegionOps bcm2835_vchiq_mbox_ops = {
    .read = bcm2835_vchiq_mbox_read,
    .write = bcm2835_vchiq_mbox_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* The doorbells */

static uint64_t bcm2835_vchiq_bell_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    BCM2835VchiqState *s = opaque;
    uint32_t val;

    switch (offset) {
    case VCHIQ_BELL0:
        /*
         * Read to clear. The guest's VCHIQ device declares no device-specific
         * interrupt clear, so this read is the only thing that lowers the
         * line -- and the interrupt is level triggered, so failing to lower
         * it here would put the guest in an interrupt storm.
         */
        val = s->bell0;
        s->bell0 = 0;
        qemu_set_irq(s->bell_irq, 0);
        return val;

    case VCHIQ_BELL2:
        return 0;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        return 0;
    }
}

static void bcm2835_vchiq_bell_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    BCM2835VchiqState *s = opaque;

    switch (offset) {
    case VCHIQ_BELL2:
        /* The guest has queued something for us */
        vchiq_parse_guest_messages(s);
        break;

    case VCHIQ_BELL0:
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        break;
    }
}

static const MemoryRegionOps bcm2835_vchiq_bell_ops = {
    .read = bcm2835_vchiq_bell_read,
    .write = bcm2835_vchiq_bell_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const VMStateDescription vmstate_bcm2835_vchiq = {
    .name = TYPE_BCM2835_VCHIQ,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(bell0, BCM2835VchiqState),
        VMSTATE_UINT32(slot0, BCM2835VchiqState),
        VMSTATE_UINT32(master_base, BCM2835VchiqState),
        VMSTATE_UINT32(slave_base, BCM2835VchiqState),
        VMSTATE_UINT32(slot_size, BCM2835VchiqState),
        VMSTATE_UINT32(max_slots, BCM2835VchiqState),
        VMSTATE_UINT32(per_side, BCM2835VchiqState),
        VMSTATE_UINT32(tx_slot, BCM2835VchiqState),
        VMSTATE_UINT32(tx_pos, BCM2835VchiqState),
        VMSTATE_UINT32(rx_pos, BCM2835VchiqState),
        VMSTATE_BOOL(connected, BCM2835VchiqState),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2835_vchiq_init(Object *obj)
{
    BCM2835VchiqState *s = BCM2835_VCHIQ(obj);

    memory_region_init_io(&s->iomem_mbox, obj, &bcm2835_vchiq_mbox_ops, s,
                          TYPE_BCM2835_VCHIQ "-mbox", 0x10);
    /* Reached from bcm2835_mbox, which reads back from us in the same call */
    s->iomem_mbox.disable_reentrancy_guard = true;

    memory_region_init_io(&s->iomem_bell, obj, &bcm2835_vchiq_bell_ops, s,
                          TYPE_BCM2835_VCHIQ "-bell", 0x10);

    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem_mbox);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem_bell);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->bell_irq);
}

static void bcm2835_vchiq_reset(DeviceState *dev)
{
    BCM2835VchiqState *s = BCM2835_VCHIQ(dev);

    s->bell0 = 0;
    s->slot0 = 0;
    s->master_base = 0;
    s->slave_base = 0;
    s->slot_size = 0;
    s->max_slots = 0;
    s->per_side = 0;
    s->tx_slot = 0;
    s->tx_pos = 0;
    s->rx_pos = 0;
    s->connected = false;
}

static void bcm2835_vchiq_realize(DeviceState *dev, Error **errp)
{
    BCM2835VchiqState *s = BCM2835_VCHIQ(dev);
    Object *obj;

    obj = object_property_get_link(OBJECT(dev), "dma-mr", &error_abort);
    s->dma_mr = MEMORY_REGION(obj);
    address_space_init(&s->dma_as, s->dma_mr, TYPE_BCM2835_VCHIQ "-memory");

    bcm2835_vchiq_reset(dev);
}

static void bcm2835_vchiq_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bcm2835_vchiq_realize;
    device_class_set_legacy_reset(dc, bcm2835_vchiq_reset);
    dc->vmsd = &vmstate_bcm2835_vchiq;
}

static const TypeInfo bcm2835_vchiq_info = {
    .name          = TYPE_BCM2835_VCHIQ,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835VchiqState),
    .class_init    = bcm2835_vchiq_class_init,
    .instance_init = bcm2835_vchiq_init,
};

static void bcm2835_vchiq_register_types(void)
{
    type_register_static(&bcm2835_vchiq_info);
}

type_init(bcm2835_vchiq_register_types)
