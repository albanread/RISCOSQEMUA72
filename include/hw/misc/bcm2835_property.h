/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef BCM2835_PROPERTY_H
#define BCM2835_PROPERTY_H

#include "hw/core/sysbus.h"
#include "net/net.h"
#include "hw/display/bcm2835_fb.h"
#include "hw/nvram/bcm2835_otp.h"
#include "qom/object.h"

#define TYPE_BCM2835_PROPERTY "bcm2835-property"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835PropertyState, BCM2835_PROPERTY)

struct BCM2835PropertyState {
    /*< private >*/
    SysBusDevice busdev;
    /*< public >*/

    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    MemoryRegion iomem;
    qemu_irq mbox_irq;
    BCM2835FBState *fbdev;
    BCM2835OTPState *otp;

    MACAddr macaddr;
    uint32_t board_rev;
    uint32_t addr;
    char *command_line;
    uint32_t touchbuf;
    uint32_t gpiovirtbuf;
    bool pending;
    /*
     * -global bcm2835-property.mode=WxH: the EDID's preferred timing,
     * which is the size RISC OS brings the desktop up in. Empty keeps
     * the 800x600 the fork has always answered with.
     */
    char *mode;
    /* Resolved from `mode` at realize: the EDID's preferred timing, and
     * therefore the size the desktop comes up in. */
    uint32_t pref_w, pref_h;
};

/*
 * The display the firmware is pretending to drive. Dispmanx's display
 * space is the physical monitor, not whatever the guest has currently
 * programmed the framebuffer to, so this is what answers GetInfo.
 */
void bcm2835_property_preferred_mode(BCM2835PropertyState *s,
                                     uint32_t *w, uint32_t *h);

#endif
