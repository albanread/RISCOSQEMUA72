/*
 * BCM2711 (Raspberry Pi 4) legacy interrupt controller
 *
 * The "ARMC interrupt registers" of the BCM2711 datasheet: the BCM2835's
 * interrupt controller grown to a bank of enables per core, for IRQ and for
 * FIQ. The GIC-400 is the primary controller and sees the same sources, so a
 * guest only comes here for what the GIC will not give it -- on a Pi 4 the
 * boot stub leaves the GIC unable to raise a FIQ, and a guest that wants one
 * enables it in its core's FIQ bank, from where it reaches the core through
 * the GIC's legacy-FIQ bypass.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef BCM2838_IC_H
#define BCM2838_IC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BCM2838_IC "bcm2838-ic"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2838ICState, BCM2838_IC)

/*
 * Input lines, in the order of the pending words:
 *   0-63   VideoCore peripheral interrupts
 *   64-71  ARMC: timer, mailbox, doorbell 0, doorbell 1, VPU0 halted,
 *          VPU1 halted, ARM address error, ARM AXI error
 *   72-79  the eight software interrupts
 */
#define BCM2838_IC_NUM_IRQS     80
#define BCM2838_IC_ARMC_BASE    64  /* inputs 64-71: the ARMC sources */
#define BCM2838_IC_NUM_CORES    4
#define BCM2838_IC_NUM_BANKS    (2 * BCM2838_IC_NUM_CORES)  /* IRQ0-3, FIQ0-3 */

/* IRQ and FIQ banks, ARMC+0x200 to +0x3ef; SWIRQ_SET/CLEAR at +0x3f0 are not ours */
#define BCM2838_IC_SIZE         0x1f0

/* One output per core on each */
#define BCM2838_IC_IRQ_OUT      "irq"
#define BCM2838_IC_FIQ_OUT      "fiq"

struct BCM2838ICState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    qemu_irq irq[BCM2838_IC_NUM_CORES];
    qemu_irq fiq[BCM2838_IC_NUM_CORES];

    uint32_t level[3];                              /* raw input lines */
    uint32_t enable[BCM2838_IC_NUM_BANKS][3];
};

#endif
