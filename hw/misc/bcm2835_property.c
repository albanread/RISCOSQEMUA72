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

#define VCHI_BUSADDR_SIZE       sizeof(uint32_t)

/* https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface */

/*
 * The monitor the firmware would have read over DDC: an EDID 1.3 block
 * describing a digital 800x600 display, preferred timing VESA DMT 800x600
 * at 60 Hz (40 MHz, 1056x628 total), with 640x480 and 800x600 among the
 * established timings and a range-limits descriptor that admits them. The
 * checksum byte is filled in when the block is handed over.
 */
static const uint8_t bcm2835_edid_800x600[127] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,     /* header */
    0x45, 0xb5, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,     /* "QMU", product 1 */
    0x00, 0x24, 0x01, 0x03,                             /* 2026, EDID 1.3 */
    0x80, 0x20, 0x18, 0x78, 0x02,                       /* digital, 32x24 cm, gamma 2.2, preferred timing */
    0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54,
    0x21, 0x00, 0x00,                                   /* established: 640x480@60, 800x600@60 */
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,     /* no standard timings */
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    /* detailed timing: 800x600@60 */
    0xa0, 0x0f, 0x20, 0x00, 0x31, 0x58, 0x1c, 0x20, 0x28, 0x80, 0x14, 0x00,
    0x40, 0xf0, 0x10, 0x00, 0x00, 0x1e,
    /* monitor name */
    0x00, 0x00, 0x00, 0xfc, 0x00, 'Q', 'E', 'M', 'U', ' ', 'P', 'i', ' ', '4',
    0x0a, 0x20, 0x20, 0x20,
    /* range limits: 50-75 Hz, 30-50 kHz, 50 MHz */
    0x00, 0x00, 0x00, 0xfd, 0x00, 0x32, 0x4b, 0x1e, 0x32, 0x05, 0x00,
    0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    /* unused descriptor */
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,                                               /* no extension blocks */
};

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
            unsigned sum = 0;

            stl_le_phys(&s->dma_as, value + 16, block == 0 ? 0 : 1);
            if (block == 0) {
                memcpy(edid, bcm2835_edid_800x600, sizeof(edid) - 1);
                for (int i = 0; i < sizeof(edid) - 1; i++) {
                    sum += edid[i];
                }
                edid[sizeof(edid) - 1] = -sum;
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
