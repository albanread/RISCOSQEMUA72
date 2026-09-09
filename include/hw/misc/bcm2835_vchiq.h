/*
 * BCM2835 VideoCore mailbox channel 3 - VCHIQ
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef BCM2835_VCHIQ_H
#define BCM2835_VCHIQ_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BCM2835_VCHIQ "bcm2835-vchiq"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835VchiqState, BCM2835_VCHIQ)

/*
 * Slot zero: the structure the guest builds in memory and whose bus address it
 * posts to mailbox channel 3. Offsets were read out of a live RISC OS 5.30
 * guest and agree with the Linux driver's struct vchiq_slot_zero.
 *
 *   +0x00  magic 'VCHI'   +0x04  version, version_min   +0x08 slot_zero_size
 *   +0x0c  slot_size      +0x10  max_slots             +0x14 max_slots_per_side
 *   +0x18  platform_data[2]
 *   +0x20  the VideoCore's shared state ("master")
 *   +0x20+S the ARM's shared state ("slave")
 *
 * S is derived rather than hardcoded: the debug array sits last in the shared
 * state and its length is a build option, so the offsets *within* a side are
 * fixed but the slave's base moves.
 */
#define VCHIQ_SLOT_MAGIC            0x56434849  /* 'VCHI' */

#define VCHIQ_SZ_MAGIC              0x00
#define VCHIQ_SZ_VERSION            0x04
#define VCHIQ_SZ_SLOT_ZERO_SIZE     0x08
#define VCHIQ_SZ_SLOT_SIZE          0x0c
#define VCHIQ_SZ_MAX_SLOTS          0x10
#define VCHIQ_SZ_MAX_SLOTS_PER_SIDE 0x14
#define VCHIQ_SZ_MASTER             0x20

/* Fields within one shared state */
#define VCHIQ_SS_INITIALISED        0x00
#define VCHIQ_SS_SLOT_FIRST         0x04
#define VCHIQ_SS_SLOT_LAST          0x08
#define VCHIQ_SS_SLOT_SYNC          0x0c
#define VCHIQ_SS_TRIGGER            0x10
#define VCHIQ_SS_TX_POS             0x1c
#define VCHIQ_SS_RECYCLE            0x20
#define VCHIQ_SS_SLOT_QUEUE_RECYCLE 0x2c
#define VCHIQ_SS_SYNC_TRIGGER       0x30
#define VCHIQ_SS_SYNC_RELEASE       0x3c
#define VCHIQ_SS_SLOT_QUEUE         0x48

/* A remote event: the waiter arms it, the far side fires it and rings a bell */
#define VCHIQ_EV_ARMED              0x00
#define VCHIQ_EV_FIRED              0x04

/* Message types, from VCHIQ_MAKE_MSG(type, srcport, dstport) */
#define VCHIQ_MSG_PADDING           0
#define VCHIQ_MSG_CONNECT           1
#define VCHIQ_MSG_OPEN              2
#define VCHIQ_MSG_OPENACK           3
#define VCHIQ_MSG_CLOSE             4

#define VCHIQ_MSG_HDR_SIZE          8
#define VCHIQ_MAKE_MSG(t, src, dst) (((t) << 24) | ((src) << 12) | (dst))
#define VCHIQ_MSG_TYPE(id)          ((id) >> 24)
#define VCHIQ_MSG_SRCPORT(id)       (((id) >> 12) & 0xfff)

/* Doorbell registers, relative to the region base (mailbox base + 0x40) */
#define VCHIQ_BELL0                 0x00  /* VC -> ARM, read to clear */
#define VCHIQ_BELL2                 0x08  /* ARM -> VC */
#define VCHIQ_BELL_RUNG             0x04  /* the only bit the guest's ISR tests */

struct BCM2835VchiqState {
    /*< private >*/
    SysBusDevice busdev;
    /*< public >*/

    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    MemoryRegion iomem_mbox;    /* channel-3 window inside mbox_mr */
    MemoryRegion iomem_bell;    /* doorbells, overlaid on peri_mr */
    qemu_irq bell_irq;

    uint32_t bell0;             /* VC->ARM pending bits */
    uint32_t slot0;             /* bus address of slot zero, 0 when idle */
    uint32_t master_base;
    uint32_t slave_base;
    uint32_t slot_size;
    uint32_t tx_slot;           /* the slot we write our messages into */
    uint32_t tx_pos;
    uint32_t rx_pos;            /* our cursor into the guest's message stream */
    bool connected;
};

#endif
