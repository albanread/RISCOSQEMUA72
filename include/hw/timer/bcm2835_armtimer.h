/*
 * BCM2835 ARM timer, as a latch
 *
 * The SP804-derived timer in the ARM control block, reduced to what the
 * guest can observe: its registers hold what was written, the free-running
 * counter advances, and the interrupt is raised by a "fire" GPIO input
 * rather than by a modelled countdown. RISC OS's BCMVideo arms the timer
 * after every vertical sync for half the frame, so the vsync generator
 * fires it halfway through each frame and BCMVideo's handler sends its
 * pending screen updates to the GPU then.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef BCM2835_ARMTIMER_H
#define BCM2835_ARMTIMER_H

#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qom/object.h"

#define TYPE_BCM2835_ARMTIMER "bcm2835-armtimer"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835ARMTimerState, BCM2835_ARMTIMER)

/* GPIO input: the timer reaches zero (if it is enabled) */
#define BCM2835_ARMTIMER_FIRE_IN "fire"

struct BCM2835ARMTimerState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t counter_hz;        /* property: free-running counter rate */

    uint32_t load;
    uint32_t reload;
    uint32_t control;
    uint32_t prediv;
    uint32_t raw_irq;
    uint64_t fires;             /* pulses that raised the interrupt */
};

#endif
