/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 * Upstreaming code cleanup [including bcm2835_*] (c) 2013 Jan Petrous
 *
 * Rasperry Pi 2 emulation and refactoring Copyright (c) 2015, Microsoft
 * Written by Andrew Baumann
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef BCM2835_FB_H
#define BCM2835_FB_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define UPPER_RAM_BASE 0x40000000

#define TYPE_BCM2835_FB "bcm2835-fb"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835FBState, BCM2835_FB)

/*
 * Configuration information about the fb which the guest can program
 * via the mailbox property interface.
 */
typedef struct {
    uint32_t xres, yres;
    uint32_t xres_virtual, yres_virtual;
    uint32_t xoffset, yoffset;
    uint32_t bpp;
    uint32_t base;
    uint32_t pixo;
    uint32_t alpha;
} BCM2835FBConfig;

struct BCM2835FBState {
    /*< private >*/
    SysBusDevice busdev;
    /*< public >*/

    uint32_t vcram_base, vcram_size;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    MemoryRegion iomem;
    MemoryRegionSection fbsection;
    QemuConsole *con;
    qemu_irq mbox_irq;

    bool lock, invalidate, pending;

    BCM2835FBConfig config;
    BCM2835FBConfig initial_config;

    /*
     * Bumped after every committed config change, so a reader on another
     * thread can tell a stale copy from a current one (see
     * bcm2835_fb_get_config).
     */
    uint32_t generation;
};

void bcm2835_fb_reconfigure(BCM2835FBState *s, BCM2835FBConfig *newconfig);

/**
 * bcm2835_fb_get_config: snapshot the config, with its generation
 * @s: the framebuffer device
 * @out: where to copy the config
 *
 * Copies the current config for a reader that does not hold the BQL. The
 * generation is read before and after the copy; if the config changed in
 * between, the copy is repeated, so the caller never sees half a config.
 * The value returned is the generation of the copy -- compare it against a
 * previous one to detect a mode change.
 */
uint32_t bcm2835_fb_get_config(BCM2835FBState *s, BCM2835FBConfig *out);

/**
 * bcm2835_fb_get_pitch: return number of bytes per line of the framebuffer
 * @config: configuration info for the framebuffer
 *
 * Return the number of bytes per line of the framebuffer, ie the number
 * that must be added to a pixel address to get the address of the pixel
 * directly below it on screen.
 */
static inline uint32_t bcm2835_fb_get_pitch(BCM2835FBConfig *config)
{
    uint32_t xres = MAX(config->xres, config->xres_virtual);
    /*
     * Round up to whole bytes: 1, 2 and 4 bpp lines are byte-packed.
     * (xres * (bpp >> 3) gave the sub-byte depths a zero pitch and a
     * zero-sized framebuffer with it.)
     */
    return (xres * config->bpp + 7) >> 3;
}

/**
 * bcm2835_fb_get_size: return total size of framebuffer in bytes
 * @config: configuration info for the framebuffer
 */
static inline uint32_t bcm2835_fb_get_size(BCM2835FBConfig *config)
{
    uint32_t yres = MAX(config->yres, config->yres_virtual);
    return yres * bcm2835_fb_get_pitch(config);
}

/**
 * bcm2835_fb_validate_config: check provided config
 *
 * Validates the configuration information provided by the guest and
 * adjusts it if necessary.
 */
void bcm2835_fb_validate_config(BCM2835FBConfig *config);

/**
 * bcm2835_fb_synth_mode: force the fb into a mode no guest asked for, with
 * a deterministic test pattern in the buffer -- the debug command behind
 * "synthfb", so the display pipeline's decoders can be exercised for
 * depths and sizes this guest's OS never programs.
 */
void bcm2835_fb_synth_mode(BCM2835FBState *s, uint32_t bpp,
                           uint32_t xres, uint32_t yres);

#endif
