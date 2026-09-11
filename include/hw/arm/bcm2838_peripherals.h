/*
 * BCM2838 peripherals emulation
 *
 * Copyright (C) 2022 Ovchinnikov Vitalii <vitalii.ovchinnikov@auriga.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef BCM2838_PERIPHERALS_H
#define BCM2838_PERIPHERALS_H

#include "hw/arm/bcm2835_peripherals.h"
#include "hw/sd/sdhci.h"
#include "hw/gpio/bcm2838_gpio.h"
#include "hw/intc/bcm2838_ic.h"
#include "hw/misc/vmchannel.h"
#include "hw/misc/riscos_blitter.h"

/* SPI */
/*
 * The legacy VideoCore interrupts 0-63 appear on the GIC as SPI 64-127, so
 * VC IRQ n is SPI 64 + n. The system timer's four compare outputs are VC IRQ
 * 0-3; only 1 and 3 are free for the ARM to use on real hardware.
 */
#define GIC_SPI_INTERRUPT_SYSTIMER0    64
#define GIC_SPI_INTERRUPT_MBOX         33
#define GIC_SPI_INTERRUPT_DOORBELL0    34
#define GIC_SPI_INTERRUPT_MPHI         40
#define GIC_SPI_INTERRUPT_DWC2         73
#define GIC_SPI_INTERRUPT_SMI          112 /* VC IRQ 48: the vsync latch */
/* ARMC sources 0-7 appear as SPI 32-39 */
#define GIC_SPI_INTERRUPT_ARM_TIMER    32
#define GIC_SPI_INTERRUPT_DMA_0        80
#define GIC_SPI_INTERRUPT_DMA_6        86
#define GIC_SPI_INTERRUPT_DMA_7_8      87
#define GIC_SPI_INTERRUPT_DMA_9_10     88
#define GIC_SPI_INTERRUPT_AUX_UART1    93
#define GIC_SPI_INTERRUPT_I2C          117
#define GIC_SPI_INTERRUPT_SDHOST       120
#define GIC_SPI_INTERRUPT_UART0        121
#define GIC_SPI_INTERRUPT_RNG200       125
#define GIC_SPI_INTERRUPT_EMMC_EMMC2   126
#define GIC_SPI_INTERRUPT_PCI_INT_A    143
#define GIC_SPI_INTERRUPT_GENET_A      157
#define GIC_SPI_INTERRUPT_GENET_B      158


/* GPU (legacy) DMA interrupts */
#define GPU_INTERRUPT_DMA0      16
#define GPU_INTERRUPT_DMA1      17
#define GPU_INTERRUPT_DMA2      18
#define GPU_INTERRUPT_DMA3      19
#define GPU_INTERRUPT_DMA4      20
#define GPU_INTERRUPT_DMA5      21
#define GPU_INTERRUPT_DMA6      22
#define GPU_INTERRUPT_DMA7_8    23
#define GPU_INTERRUPT_DMA9_10   24
#define GPU_INTERRUPT_DMA11     25
#define GPU_INTERRUPT_DMA12     26
#define GPU_INTERRUPT_DMA13     27
#define GPU_INTERRUPT_DMA14     28
#define GPU_INTERRUPT_DMA15     31

/*
 * The PCIe root complex, in the low peripheral range. Not modelled: this is
 * only here so that a guest probing for it is told "no link" instead of
 * taking an external abort on unmapped memory.
 */
#define BCM2838_PCIE_OFFSET     0x1500000
#define BCM2838_PCIE_SIZE       0x100000

/*
 * The GENET Ethernet MAC in the low peripheral range: 0xfd580000 on a
 * BCM2711, which RISC OS's HAL hands its driver as a device at that address
 * (HAL_BCM2835 hdr/BCM2835: GENET_Base * &01580000). Not modelled; here so
 * a driver that probes it reads zero instead of taking an external abort.
 */
#define BCM2711_GENET_OFFSET    0x1580000
#define BCM2711_GENET_SIZE      0x10000

/*
 * The HostFS doorbell (riscos-pi4/FSDESIGN.md): 0xfd400000, a hole in the
 * BCM2711 low-peripheral map that RISC OS's HAL device table does not
 * name (nothing at &014xxxxx) and that no other model maps, so the guest
 * discovers it by magic alone.
 */
#define VMCHANNEL_OFFSET        0x1400000
#define VMCHANNEL_SIZE          0x4000

/* Next to the HostFS doorbell in the low window.  The FE00 section is
 * mapped by RISC OS only page by page as the HAL asks for it, so a page
 * in a hole there is not reachable -- the doorbell is at 0xFD400000 for
 * that reason and the blitter follows it to 0xFD404000. */
#define BLITTER_OFFSET          0x1404000

#define BCM2838_MPHI_OFFSET     0xb200
#define BCM2838_MPHI_SIZE       0x200

#define TYPE_BCM2838_PERIPHERALS "bcm2838-peripherals"
OBJECT_DECLARE_TYPE(BCM2838PeripheralState, BCM2838PeripheralClass,
                    BCM2838_PERIPHERALS)

struct BCM2838PeripheralState {
    /*< private >*/
    BCMSocPeripheralBaseState parent_obj;

    /*< public >*/
    MemoryRegion peri_low_mr;
    MemoryRegion peri_low_mr_alias;
    MemoryRegion mphi_mr_alias;

    UnimplementedDeviceState pcie;
    UnimplementedDeviceState genet;

    /* The HostFS doorbell, mapped at VMCHANNEL_OFFSET in both windows:
     * the low alias directly, the high one through vmchannel_mr_alias
     * (a region cannot live in two containers) */
    VMChannelState vmchannel;
    MemoryRegion vmchannel_mr_alias;
    char *vmchannel_root;          /* forwarded to the doorbell's root= */

    /* The rectangle blitter, one page past the doorbell at 0xFD404000. */
    RISCOSBlitterState blitter;

    SDHCIState emmc2;
    BCM2838GpioState gpio;

    OrIRQState mmc_irq_orgate;
    OrIRQState dma_7_8_irq_orgate;
    OrIRQState dma_9_10_irq_orgate;

    UnimplementedDeviceState asb;
    UnimplementedDeviceState clkisp;

    /*
     * The BCM2711's own legacy interrupt controller. It sits over the
     * BCM2835 one in parent_obj, which the common code maps at the same
     * address and which nothing on this SoC listens to.
     */
    BCM2838ICState ic;
    SplitIRQ dwc2_irq_splitter;     /* USB goes to the GIC and to ic */
    SplitIRQ smi_irq_splitter;      /* vsync, likewise */
    SplitIRQ armtmr_irq_splitter;   /* ARM timer, likewise */
};

struct BCM2838PeripheralClass {
    /*< private >*/
    BCMSocPeripheralBaseClass parent_class;
    /*< public >*/
    uint64_t peri_low_size; /* Peripheral lower range size */
};

#endif /* BCM2838_PERIPHERALS_H */
