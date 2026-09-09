/*
 * BCM2835 VideoCore mailbox channel 0 — power management
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef BCM2835_MBOX_POWER_H
#define BCM2835_MBOX_POWER_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BCM2835_MBOX_POWER "bcm2835-mbox-power"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835MboxPowerState, BCM2835_MBOX_POWER)

/* Device bits in a channel-0 message, from the firmware wiki */
#define MBOX_POWER_SD_CARD  (1 << 0)
#define MBOX_POWER_UART0    (1 << 1)
#define MBOX_POWER_UART1    (1 << 2)
#define MBOX_POWER_USB_HCD  (1 << 3)
#define MBOX_POWER_I2C0     (1 << 4)
#define MBOX_POWER_I2C1     (1 << 5)
#define MBOX_POWER_I2C2     (1 << 6)
#define MBOX_POWER_SPI      (1 << 7)
#define MBOX_POWER_CCP2TX   (1 << 8)

struct BCM2835MboxPowerState {
    /*< private >*/
    SysBusDevice busdev;
    /*< public >*/

    MemoryRegion iomem;
    qemu_irq mbox_irq;

    uint32_t state;     /* last requested power mask, in message form */
    bool pending;
};

#endif
