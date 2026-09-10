/*
 * BCM2835 SMI (secondary memory interface): the vertical-sync latch
 *
 * The VideoCore firmware raises the SMI interrupt (VC IRQ 48) on every
 * vertical sync of the display it drives, and the ARM clears it by writing
 * the SMI control/status register. RISC OS's BCMVideo driver claims that
 * interrupt at start-up, counts arrivals for 20 centiseconds and adopts it
 * as its VSync source only if more than three came; otherwise it fakes
 * VSync from the 100 Hz ticker. This model has nothing of the memory
 * interface itself, only the latch: a "vsync" GPIO input that a display
 * backend pulses, the DONE flag it sets, and the interrupt that follows.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef BCM2835_SMI_H
#define BCM2835_SMI_H

#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qom/object.h"

#define TYPE_BCM2835_SMI "bcm2835-smi"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835SMIState, BCM2835_SMI)

/* GPIO input: a rising edge marks one vertical sync */
#define BCM2835_SMI_VSYNC_IN "vsync"

struct BCM2835SMIState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t cs;            /* SMI_CS; DONE (bit 1) is the pending vsync */
    uint64_t vsyncs;        /* pulses received since reset */
};

#endif
