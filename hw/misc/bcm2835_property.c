/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/misc/bcm2835_property.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/core/irq.h"
#include "hw/misc/bcm2835_mbox_defs.h"
#include "hw/arm/raspberrypi-fw-defs.h"
#include "system/dma.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"
#include "hw/arm/raspi_platform.h"
#include "hw/core/qdev-properties.h"
#include "qemu/error-report.h"

#define VCHI_BUSADDR_SIZE       sizeof(uint32_t)

/* https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface */

/*
 * The monitor the firmware would have read over DDC.
 *
 * RISC OS builds its whole mode list out of this: ScreenModes reads the
 * EDID, filters it by what BCMVideo can drive, and the Display Manager
 * offers what survives. So the sizes a user can choose are decided here,
 * and nowhere else -- which is why it is a table rather than the 127
 * hand-written bytes it used to be.
 *
 * Nothing behind it is real, so the timings only have to be well formed
 * and self-consistent: the active area is what the framebuffer becomes,
 * and the rest exists to be believed. They are the standard DMT and
 * CVT-RB figures anyway, because a plausible block is easier to debug
 * than an invented one.
 */
typedef struct {
    uint16_t w, h;
    uint32_t clock_khz;             /* pixel clock */
    uint16_t hblank, hfp, hsync;    /* horizontal blanking, in pixels */
    uint16_t vblank, vfp, vsync;    /* vertical blanking, in lines */
} BCM2835EdidMode;

static const BCM2835EdidMode bcm2835_edid_modes[] = {
    {  640,  480,  25175, 160,  16,  96, 45, 10, 2 },   /* DMT */
    {  800,  600,  40000, 256,  40, 128, 28,  1, 4 },   /* DMT */
    { 1024,  768,  65000, 320,  24, 136, 38,  3, 6 },   /* DMT */
    { 1280,  720,  74250, 370, 110,  40, 30,  5, 5 },   /* CEA-861 */
    { 1280,  800,  71000, 160,  48,  32, 23,  3, 6 },   /* CVT-RB */
    { 1280, 1024, 108000, 408,  48, 112, 42,  1, 3 },   /* DMT */
    { 1440,  900,  88750, 160,  48,  32, 26,  3, 6 },   /* CVT-RB */
    { 1600, 1200, 162000, 560,  64, 192, 50,  1, 3 },   /* DMT */
    { 1920, 1080, 148500, 280,  88,  44, 45,  4, 5 },   /* CEA-861 */
    { 1920, 1200, 154000, 160,  48,  32, 35,  3, 6 },   /* CVT-RB */
};

/*
 * The eight standard-timing slots, which is where most of the list
 * lives. The encoding is (pixels / 8) - 31 in one byte, so it cannot
 * express a width over 2288 or one that is not a multiple of eight --
 * 1366 famously is not -- and the aspect is one of four. Anything that
 * does not fit would need a detailed timing or a CTA-861 extension
 * block; nothing here needs one yet.
 */
static const uint16_t bcm2835_edid_standard[8][2] = {
    { 1024,  768 }, { 1280,  720 }, { 1280,  800 }, { 1280, 1024 },
    { 1440,  900 }, { 1600, 1200 }, { 1920, 1080 }, { 1920, 1200 },
};

static const BCM2835EdidMode *bcm2835_edid_find(uint32_t w, uint32_t h)
{
    for (size_t i = 0; i < ARRAY_SIZE(bcm2835_edid_modes); i++) {
        if (bcm2835_edid_modes[i].w == w && bcm2835_edid_modes[i].h == h) {
            return &bcm2835_edid_modes[i];
        }
    }
    return NULL;
}

/* An 18-byte detailed timing descriptor */
static void bcm2835_edid_detailed(uint8_t *d, const BCM2835EdidMode *m)
{
    /* ~100 dpi, so the aspect the guest reads matches the mode's */
    uint32_t mm_w = m->w / 4, mm_h = m->h / 4;

    d[0] = (m->clock_khz / 10) & 0xff;          /* 10 kHz units */
    d[1] = (m->clock_khz / 10) >> 8;
    d[2] = m->w & 0xff;
    d[3] = m->hblank & 0xff;
    d[4] = ((m->w >> 8) << 4) | (m->hblank >> 8);
    d[5] = m->h & 0xff;
    d[6] = m->vblank & 0xff;
    d[7] = ((m->h >> 8) << 4) | (m->vblank >> 8);
    d[8] = m->hfp & 0xff;
    d[9] = m->hsync & 0xff;
    d[10] = ((m->vfp & 0xf) << 4) | (m->vsync & 0xf);
    d[11] = ((m->hfp >> 8) << 6) | ((m->hsync >> 8) << 4)
          | ((m->vfp >> 4) << 2) | (m->vsync >> 4);
    d[12] = mm_w & 0xff;
    d[13] = mm_h & 0xff;
    d[14] = ((mm_w >> 8) << 4) | (mm_h >> 8);
    d[15] = 0;                                  /* borders */
    d[16] = 0;
    d[17] = 0x1e;                               /* digital separate, +h +v */
}

/*
 * Build the block. `pref` is the preferred timing, which is the mode
 * RISC OS brings the desktop up in.
 */
static void bcm2835_edid_build(uint8_t *edid, const BCM2835EdidMode *pref)
{
    unsigned sum = 0;
    uint8_t *d;

    memset(edid, 0, 128);
    memset(edid + 1, 0xff, 6);                  /* header */
    edid[8] = 0x45; edid[9] = 0xb5;             /* "QMU" */
    edid[10] = 0x01;                            /* product 1 */
    edid[16] = 0x00; edid[17] = 0x24;           /* week -, year 2026 */
    edid[18] = 0x01; edid[19] = 0x03;           /* EDID 1.3 */
    edid[20] = 0x80;                            /* digital */
    edid[21] = pref->w / 40;                    /* cm, from the mode */
    edid[22] = pref->h / 40;
    edid[23] = 0x78;                            /* gamma 2.2 */
    edid[24] = 0x02;                            /* preferred timing in DTD 1 */
    memcpy(edid + 25, "\xee\x91\xa3\x54\x4c\x99\x26\x0f\x50\x54", 10);

    /* Established: 640x480@60 and 800x600@60, the two legacy sizes */
    edid[35] = 0x21;

    for (size_t i = 0; i < 8; i++) {
        uint32_t w = bcm2835_edid_standard[i][0];
        uint32_t h = bcm2835_edid_standard[i][1];
        uint32_t aspect;

        /* 0 = 16:10, 1 = 4:3, 2 = 5:4, 3 = 16:9 */
        if (w * 10 == h * 16) {
            aspect = 0;
        } else if (w * 3 == h * 4) {
            aspect = 1;
        } else if (w * 4 == h * 5) {
            aspect = 2;
        } else {
            aspect = 3;
        }
        edid[38 + i * 2] = (w / 8) - 31;
        edid[39 + i * 2] = (aspect << 6) | (60 - 60);
    }

    /* Descriptor 1: the preferred timing */
    bcm2835_edid_detailed(edid + 54, pref);

    /* Descriptor 2: monitor name */
    d = edid + 72;
    d[3] = 0xfc;
    memcpy(d + 5, "QEMU Pi 4\n         ", 13);

    /*
     * Descriptor 3: range limits, and the reason the big modes are
     * reachable at all. RISC OS checks every timing against these before
     * it will offer it, so a ceiling of 50 MHz -- which is what this
     * block used to carry -- silently discards anything above about
     * 800x600 however many timings are advertised. 1920x1200 wants
     * 154 MHz and 75 kHz.
     */
    d = edid + 90;
    d[3] = 0xfd;
    d[5] = 50;                                  /* 50-75 Hz vertical */
    d[6] = 75;
    d[7] = 15;                                  /* 15-200 kHz horizontal */
    d[8] = 200;
    d[9] = 60;                                  /* 600 MHz pixel clock */
    d[10] = 0x00;
    memset(d + 11, 0x20, 7);

    /* Descriptor 4: unused */
    edid[108 + 3] = 0x10;

    edid[126] = 0;                              /* no extension blocks */
    for (int i = 0; i < 127; i++) {
        sum += edid[i];
    }
    edid[127] = -sum;
}

void bcm2835_property_preferred_mode(BCM2835PropertyState *s,
                                     uint32_t *w, uint32_t *h)
{
    *w = s->pref_w;
    *h = s->pref_h;
}

static void bcm2835_property_mbox_push(BCM2835PropertyState *s, uint32_t value)
{
    uint32_t tot_len;

    /*
     * Copy the current state of the framebuffer config; we will update
     * this copy as we process tags and then ask the framebuffer to use
     * it at the end.
     */
    BCM2835FBConfig fbconfig = s->fbdev->config;
    bool fbconfig_updated = false;

    value &= ~0xf;

    s->addr = value;

    tot_len = ldl_le_phys(&s->dma_as, value);

    /* @(addr + 4) : Buffer response code */
    value = s->addr + 8;
    while (value + 8 <= s->addr + tot_len) {
        uint32_t tag = ldl_le_phys(&s->dma_as, value);
        uint32_t bufsize = ldl_le_phys(&s->dma_as, value + 4);
        /* @(value + 8) : Request/response indicator */
        size_t resplen = 0;
        switch (tag) {
        case RPI_FWREQ_PROPERTY_END:
            break;
        case RPI_FWREQ_GET_FIRMWARE_REVISION:
            stl_le_phys(&s->dma_as, value + 12, 346337);
            resplen = 4;
            break;
        case RPI_FWREQ_GET_BOARD_MODEL:
            qemu_log_mask(LOG_UNIMP,
                          "bcm2835_property: 0x%08x get board model NYI\n",
                          tag);
            resplen = 4;
            break;
        case RPI_FWREQ_GET_BOARD_REVISION:
            stl_le_phys(&s->dma_as, value + 12, s->board_rev);
            resplen = 4;
            break;
        case RPI_FWREQ_GET_BOARD_MAC_ADDRESS:
            resplen = sizeof(s->macaddr.a);
            dma_memory_write(&s->dma_as, value + 12, s->macaddr.a, resplen,
                             MEMTXATTRS_UNSPECIFIED);
            break;
        case RPI_FWREQ_GET_BOARD_SERIAL:
            qemu_log_mask(LOG_UNIMP,
                          "bcm2835_property: 0x%08x get board serial NYI\n",
                          tag);
            resplen = 8;
            break;
        case RPI_FWREQ_GET_ARM_MEMORY:
            /* base */
            stl_le_phys(&s->dma_as, value + 12, 0);
            /* size */
            stl_le_phys(&s->dma_as, value + 16, s->fbdev->vcram_base);
            resplen = 8;
            break;
        case RPI_FWREQ_GET_VC_MEMORY:
            /* base */
            stl_le_phys(&s->dma_as, value + 12, s->fbdev->vcram_base);
            /* size */
            stl_le_phys(&s->dma_as, value + 16, s->fbdev->vcram_size);
            resplen = 8;
            break;
        case RPI_FWREQ_SET_POWER_STATE:
        {
            /*
             * Assume that whatever device they asked for exists,
             * and we'll just claim we set it to the desired state.
             */
            uint32_t state = ldl_le_phys(&s->dma_as, value + 16);
            stl_le_phys(&s->dma_as, value + 16, (state & 1));
            resplen = 8;
            break;
        }

        /* Clocks */

        case RPI_FWREQ_GET_CLOCK_STATE:
            stl_le_phys(&s->dma_as, value + 16, 0x1);
            resplen = 8;
            break;

        case RPI_FWREQ_SET_CLOCK_STATE:
            qemu_log_mask(LOG_UNIMP,
                          "bcm2835_property: 0x%08x set clock state NYI\n",
                          tag);
            resplen = 8;
            break;

        case RPI_FWREQ_GET_CLOCK_RATE:
        case RPI_FWREQ_GET_MAX_CLOCK_RATE:
        case RPI_FWREQ_GET_MIN_CLOCK_RATE:
            switch (ldl_le_phys(&s->dma_as, value + 12)) {
            case RPI_FIRMWARE_EMMC_CLK_ID:
                stl_le_phys(&s->dma_as, value + 16, RPI_FIRMWARE_EMMC_CLK_RATE);
                break;
            case RPI_FIRMWARE_UART_CLK_ID:
                stl_le_phys(&s->dma_as, value + 16, RPI_FIRMWARE_UART_CLK_RATE);
                break;
            case RPI_FIRMWARE_CORE_CLK_ID:
                stl_le_phys(&s->dma_as, value + 16, RPI_FIRMWARE_CORE_CLK_RATE);
                break;
            default:
                stl_le_phys(&s->dma_as, value + 16,
                            RPI_FIRMWARE_DEFAULT_CLK_RATE);
                break;
            }
            resplen = 8;
            break;

        case RPI_FWREQ_GET_CLOCKS:
            /* TODO: add more clock IDs if needed */
            stl_le_phys(&s->dma_as, value + 12, 0);
            stl_le_phys(&s->dma_as, value + 16, RPI_FIRMWARE_ARM_CLK_ID);
            resplen = 8;
            break;

        case RPI_FWREQ_SET_CLOCK_RATE:
        case RPI_FWREQ_SET_MAX_CLOCK_RATE:
        case RPI_FWREQ_SET_MIN_CLOCK_RATE:
            qemu_log_mask(LOG_UNIMP,
                          "bcm2835_property: 0x%08x set clock rate NYI\n",
                          tag);
            resplen = 8;
            break;

        /* Temperature */

        case RPI_FWREQ_GET_TEMPERATURE:
            stl_le_phys(&s->dma_as, value + 16, 25000);
            resplen = 8;
            break;

        case RPI_FWREQ_GET_MAX_TEMPERATURE:
            stl_le_phys(&s->dma_as, value + 16, 99000);
            resplen = 8;
            break;

        /* Frame buffer */

        case RPI_FWREQ_FRAMEBUFFER_ALLOCATE:
            stl_le_phys(&s->dma_as, value + 12, fbconfig.base);
            stl_le_phys(&s->dma_as, value + 16,
                        bcm2835_fb_get_size(&fbconfig));
            resplen = 8;
            break;
        case RPI_FWREQ_FRAMEBUFFER_RELEASE:
            resplen = 0;
            break;
        case RPI_FWREQ_FRAMEBUFFER_BLANK:
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_PHYSICAL_WIDTH_HEIGHT:
        case RPI_FWREQ_FRAMEBUFFER_TEST_VIRTUAL_WIDTH_HEIGHT:
            resplen = 8;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_PHYSICAL_WIDTH_HEIGHT:
            fbconfig.xres = ldl_le_phys(&s->dma_as, value + 12);
            fbconfig.yres = ldl_le_phys(&s->dma_as, value + 16);
            bcm2835_fb_validate_config(&fbconfig);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_PHYSICAL_WIDTH_HEIGHT:
            stl_le_phys(&s->dma_as, value + 12, fbconfig.xres);
            stl_le_phys(&s->dma_as, value + 16, fbconfig.yres);
            resplen = 8;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_WIDTH_HEIGHT:
            fbconfig.xres_virtual = ldl_le_phys(&s->dma_as, value + 12);
            fbconfig.yres_virtual = ldl_le_phys(&s->dma_as, value + 16);
            bcm2835_fb_validate_config(&fbconfig);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_VIRTUAL_WIDTH_HEIGHT:
            stl_le_phys(&s->dma_as, value + 12, fbconfig.xres_virtual);
            stl_le_phys(&s->dma_as, value + 16, fbconfig.yres_virtual);
            resplen = 8;
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_DEPTH:
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_DEPTH:
            fbconfig.bpp = ldl_le_phys(&s->dma_as, value + 12);
            bcm2835_fb_validate_config(&fbconfig);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_DEPTH:
            stl_le_phys(&s->dma_as, value + 12, fbconfig.bpp);
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_PIXEL_ORDER:
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_PIXEL_ORDER:
            fbconfig.pixo = ldl_le_phys(&s->dma_as, value + 12);
            bcm2835_fb_validate_config(&fbconfig);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_PIXEL_ORDER:
            stl_le_phys(&s->dma_as, value + 12, fbconfig.pixo);
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_ALPHA_MODE:
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_ALPHA_MODE:
            fbconfig.alpha = ldl_le_phys(&s->dma_as, value + 12);
            bcm2835_fb_validate_config(&fbconfig);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_ALPHA_MODE:
            stl_le_phys(&s->dma_as, value + 12, fbconfig.alpha);
            resplen = 4;
            break;
        /*
         * The expander GPIOs, which on Pi 3 and later carry the activity and
         * power LEDs. Nothing here drives an LED, but the request has to be
         * answered at the documented length or a guest that checks sees a
         * failed call. Echo the pin back; report the state as off.
         */
        case RPI_FWREQ_SET_GPIO_STATE:
        case RPI_FWREQ_GET_GPIO_STATE:
            stl_le_phys(&s->dma_as, value + 16, 0);
            resplen = 8;
            break;

        /*
         * The display chain's one question of the firmware. RISC OS reads
         * the monitor's EDID through this, and picks its desktop mode from
         * the preferred timing; without an answer it falls back to the
         * kernel's oldest numbered modes and comes up in 640x256. A status
         * other than 0 is "no acknowledge", which is how the guest learns
         * there is no second block.
         */
        case RPI_FWREQ_GET_EDID_BLOCK:
        {
            uint32_t block = ldl_le_phys(&s->dma_as, value + 12);
            uint8_t edid[128];

            stl_le_phys(&s->dma_as, value + 16, block == 0 ? 0 : 1);
            if (block == 0) {
                const BCM2835EdidMode *pref =
                    bcm2835_edid_find(s->pref_w, s->pref_h);

                bcm2835_edid_build(edid, pref);
                trace_bcm2835_property_edid(pref->w, pref->h);
                dma_memory_write(&s->dma_as, value + 20, edid, sizeof(edid),
                                 MEMTXATTRS_UNSPECIFIED);
            }
            resplen = 8 + sizeof(edid);
            break;
        }

        /*
         * Buffers the firmware holds on the guest's behalf: the official
         * touchscreen's state page, and the virtual GPIO page that carries
         * the activity LED on boards where it hangs off the VideoCore rather
         * than the SoC's own pins. Both are just an address the guest hands
         * over and can ask for back.
         */
        case RPI_FWREQ_FRAMEBUFFER_SET_TOUCHBUF:
            s->touchbuf = ldl_le_phys(&s->dma_as, value + 12);
            stl_le_phys(&s->dma_as, value + 12, 0);
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_GET_TOUCHBUF:
            stl_le_phys(&s->dma_as, value + 12, s->touchbuf);
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_GPIOVIRTBUF:
            s->gpiovirtbuf = ldl_le_phys(&s->dma_as, value + 12);
            stl_le_phys(&s->dma_as, value + 12, 0);
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_GET_GPIOVIRTBUF:
            stl_le_phys(&s->dma_as, value + 12, s->gpiovirtbuf);
            resplen = 4;
            break;

        case RPI_FWREQ_FRAMEBUFFER_GET_PITCH:
            stl_le_phys(&s->dma_as, value + 12,
                        bcm2835_fb_get_pitch(&fbconfig));
            resplen = 4;
            break;
        case RPI_FWREQ_FRAMEBUFFER_TEST_VIRTUAL_OFFSET:
            resplen = 8;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_VIRTUAL_OFFSET:
            fbconfig.xoffset = ldl_le_phys(&s->dma_as, value + 12);
            fbconfig.yoffset = ldl_le_phys(&s->dma_as, value + 16);
            bcm2835_fb_validate_config(&fbconfig);
            fbconfig_updated = true;
            /* fall through */
        case RPI_FWREQ_FRAMEBUFFER_GET_VIRTUAL_OFFSET:
            stl_le_phys(&s->dma_as, value + 12, fbconfig.xoffset);
            stl_le_phys(&s->dma_as, value + 16, fbconfig.yoffset);
            resplen = 8;
            break;
        case RPI_FWREQ_FRAMEBUFFER_GET_OVERSCAN:
        case RPI_FWREQ_FRAMEBUFFER_TEST_OVERSCAN:
        case RPI_FWREQ_FRAMEBUFFER_SET_OVERSCAN:
            stl_le_phys(&s->dma_as, value + 12, 0);
            stl_le_phys(&s->dma_as, value + 16, 0);
            stl_le_phys(&s->dma_as, value + 20, 0);
            stl_le_phys(&s->dma_as, value + 24, 0);
            resplen = 16;
            break;
        case RPI_FWREQ_FRAMEBUFFER_SET_PALETTE:
        {
            uint32_t offset = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t length = ldl_le_phys(&s->dma_as, value + 16);
            int resp;

            if (offset > 255 || length < 1 || length > 256) {
                resp = 1; /* invalid request */
            } else {
                for (uint32_t e = 0; e < length; e++) {
                    uint32_t color = ldl_le_phys(&s->dma_as, value + 20 + (e << 2));
                    stl_le_phys(&s->dma_as,
                                s->fbdev->vcram_base + ((offset + e) << 2), color);
                }
                resp = 0;
            }
            stl_le_phys(&s->dma_as, value + 12, resp);
            resplen = 4;
            break;
        }
        case RPI_FWREQ_FRAMEBUFFER_GET_NUM_DISPLAYS:
            stl_le_phys(&s->dma_as, value + 12, 1);
            resplen = 4;
            break;

        case RPI_FWREQ_GET_DMA_CHANNELS:
            /* channels 2-5 */
            stl_le_phys(&s->dma_as, value + 12, 0x003C);
            resplen = 4;
            break;

        case RPI_FWREQ_GET_COMMAND_LINE:
            /*
             * We follow the firmware behaviour: no NUL terminator is
             * written to the buffer, and if the buffer is too short
             * we report the required length in the response header
             * and copy nothing to the buffer.
             */
            resplen = strlen(s->command_line);
            if (bufsize >= resplen)
                address_space_write(&s->dma_as, value + 12,
                                    MEMTXATTRS_UNSPECIFIED, s->command_line,
                                    resplen);
            break;

        case RPI_FWREQ_GET_THROTTLED:
            stl_le_phys(&s->dma_as, value + 12, 0);
            resplen = 4;
            break;

        case RPI_FWREQ_VCHIQ_INIT:
            stl_le_phys(&s->dma_as,
                        value + offsetof(rpi_firmware_prop_request_t, payload),
                        0);
            resplen = VCHI_BUSADDR_SIZE;
            break;

        /* Customer OTP */

        case RPI_FWREQ_GET_CUSTOMER_OTP:
        {
            uint32_t start_num = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t number = ldl_le_phys(&s->dma_as, value + 16);

            resplen = 8 + 4 * number;

            for (uint32_t n = start_num; n < start_num + number &&
                 n < BCM2835_OTP_CUSTOMER_OTP_LEN; n++) {
                uint32_t otp_row = bcm2835_otp_get_row(s->otp,
                                              BCM2835_OTP_CUSTOMER_OTP + n);
                stl_le_phys(&s->dma_as,
                            value + 20 + ((n - start_num) << 2), otp_row);
            }
            break;
        }
        case RPI_FWREQ_SET_CUSTOMER_OTP:
        {
            uint32_t start_num = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t number = ldl_le_phys(&s->dma_as, value + 16);

            resplen = 4;

            /* Magic numbers to permanently lock customer OTP */
            if (start_num == BCM2835_OTP_LOCK_NUM1 &&
                number == BCM2835_OTP_LOCK_NUM2) {
                bcm2835_otp_set_row(s->otp,
                                    BCM2835_OTP_ROW_32,
                                    BCM2835_OTP_ROW_32_LOCK);
                break;
            }

            /* If row 32 has the lock bit, don't allow further writes */
            if (bcm2835_otp_get_row(s->otp, BCM2835_OTP_ROW_32) &
                                    BCM2835_OTP_ROW_32_LOCK) {
                break;
            }

            for (uint32_t n = start_num; n < start_num + number &&
                 n < BCM2835_OTP_CUSTOMER_OTP_LEN; n++) {
                uint32_t otp_row = ldl_le_phys(&s->dma_as,
                                      value + 20 + ((n - start_num) << 2));
                bcm2835_otp_set_row(s->otp,
                                    BCM2835_OTP_CUSTOMER_OTP + n, otp_row);
            }
            break;
        }

        /* Device-specific private key */
        case RPI_FWREQ_GET_PRIVATE_KEY:
        {
            uint32_t start_num = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t number = ldl_le_phys(&s->dma_as, value + 16);

            resplen = 8 + 4 * number;

            for (uint32_t n = start_num; n < start_num + number &&
                 n < BCM2835_OTP_PRIVATE_KEY_LEN; n++) {
                uint32_t otp_row = bcm2835_otp_get_row(s->otp,
                                              BCM2835_OTP_PRIVATE_KEY + n);
                stl_le_phys(&s->dma_as,
                            value + 20 + ((n - start_num) << 2), otp_row);
            }
            break;
        }
        case RPI_FWREQ_SET_PRIVATE_KEY:
        {
            uint32_t start_num = ldl_le_phys(&s->dma_as, value + 12);
            uint32_t number = ldl_le_phys(&s->dma_as, value + 16);

            resplen = 4;

            /* If row 32 has the lock bit, don't allow further writes */
            if (bcm2835_otp_get_row(s->otp, BCM2835_OTP_ROW_32) &
                                    BCM2835_OTP_ROW_32_LOCK) {
                break;
            }

            for (uint32_t n = start_num; n < start_num + number &&
                 n < BCM2835_OTP_PRIVATE_KEY_LEN; n++) {
                uint32_t otp_row = ldl_le_phys(&s->dma_as,
                                      value + 20 + ((n - start_num) << 2));
                bcm2835_otp_set_row(s->otp,
                                    BCM2835_OTP_PRIVATE_KEY + n, otp_row);
            }
            break;
        }
        default:
            qemu_log_mask(LOG_UNIMP,
                          "bcm2835_property: unhandled tag 0x%08x\n", tag);
            break;
        }

        trace_bcm2835_mbox_property(tag, bufsize, resplen);
        if (tag == 0) {
            break;
        }

        stl_le_phys(&s->dma_as, value + 8, (1 << 31) | resplen);
        value += bufsize + 12;
    }

    /* Reconfigure framebuffer if required */
    if (fbconfig_updated) {
        bcm2835_fb_reconfigure(s->fbdev, &fbconfig);
    }

    /* Buffer response code */
    stl_le_phys(&s->dma_as, s->addr + 4, (1 << 31));
}

static uint64_t bcm2835_property_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    BCM2835PropertyState *s = opaque;
    uint32_t res = 0;

    switch (offset) {
    case MBOX_AS_DATA:
        res = MBOX_CHAN_PROPERTY | s->addr;
        s->pending = false;
        qemu_set_irq(s->mbox_irq, 0);
        break;

    case MBOX_AS_PENDING:
        res = s->pending;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        return 0;
    }

    return res;
}

static void bcm2835_property_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    BCM2835PropertyState *s = opaque;

    switch (offset) {
    case MBOX_AS_DATA:
        /* bcm2835_mbox should check our pending status before pushing */
        assert(!s->pending);
        s->pending = true;
        bcm2835_property_mbox_push(s, value);
        qemu_set_irq(s->mbox_irq, 1);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        return;
    }
}

static const MemoryRegionOps bcm2835_property_ops = {
    .read = bcm2835_property_read,
    .write = bcm2835_property_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const VMStateDescription vmstate_bcm2835_property = {
    .name = TYPE_BCM2835_PROPERTY,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_MACADDR(macaddr, BCM2835PropertyState),
        VMSTATE_UINT32(addr, BCM2835PropertyState),
        VMSTATE_UINT32(touchbuf, BCM2835PropertyState),
        VMSTATE_UINT32(gpiovirtbuf, BCM2835PropertyState),
        VMSTATE_BOOL(pending, BCM2835PropertyState),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2835_property_init(Object *obj)
{
    BCM2835PropertyState *s = BCM2835_PROPERTY(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &bcm2835_property_ops, s,
                          TYPE_BCM2835_PROPERTY, 0x10);

    /*
     * bcm2835_property_ops call into bcm2835_mbox, which in-turn reads from
     * iomem. As such, mark iomem as re-entracy safe.
     */
    s->iomem.disable_reentrancy_guard = true;

    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->mbox_irq);
}

static void bcm2835_property_reset(DeviceState *dev)
{
    BCM2835PropertyState *s = BCM2835_PROPERTY(dev);

    s->pending = false;
}

static void bcm2835_property_realize(DeviceState *dev, Error **errp)
{
    BCM2835PropertyState *s = BCM2835_PROPERTY(dev);
    {
        unsigned w, h;

        s->pref_w = 800;
        s->pref_h = 600;
        if (s->mode && sscanf(s->mode, "%ux%u", &w, &h) == 2) {
            if (bcm2835_edid_find(w, h)) {
                s->pref_w = w;
                s->pref_h = h;
            } else {
                warn_report("bcm2835-property: no timing for mode %s; the "
                            "desktop will come up at 800x600", s->mode);
            }
        }
    }
    Object *obj;

    obj = object_property_get_link(OBJECT(dev), "fb", &error_abort);
    s->fbdev = BCM2835_FB(obj);

    obj = object_property_get_link(OBJECT(dev), "dma-mr", &error_abort);
    s->dma_mr = MEMORY_REGION(obj);
    address_space_init(&s->dma_as, s->dma_mr, TYPE_BCM2835_PROPERTY "-memory");

    obj = object_property_get_link(OBJECT(dev), "otp", &error_abort);
    s->otp = BCM2835_OTP(obj);

    /* TODO: connect to MAC address of USB NIC device, once we emulate it */
    qemu_macaddr_default_if_unset(&s->macaddr);

    bcm2835_property_reset(dev);
}

static const Property bcm2835_property_props[] = {
    DEFINE_PROP_STRING("mode", BCM2835PropertyState, mode),
    DEFINE_PROP_UINT32("board-rev", BCM2835PropertyState, board_rev, 0),
    DEFINE_PROP_STRING("command-line", BCM2835PropertyState, command_line),
};

static void bcm2835_property_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, bcm2835_property_props);
    dc->realize = bcm2835_property_realize;
    dc->vmsd = &vmstate_bcm2835_property;
}

static const TypeInfo bcm2835_property_info = {
    .name          = TYPE_BCM2835_PROPERTY,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835PropertyState),
    .class_init    = bcm2835_property_class_init,
    .instance_init = bcm2835_property_init,
};

static void bcm2835_property_register_types(void)
{
    type_register_static(&bcm2835_property_info);
}

type_init(bcm2835_property_register_types)
