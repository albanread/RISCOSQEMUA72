/*
 * riscos-blitter — rectangle fill and copy done by the host.
 *
 * See include/hw/misc/riscos_blitter.h for the protocol.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/riscos_blitter.h"
#include "hw/display/bcm2835_fb.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "trace.h"

/*
 * Geometry the guest asked for must be something a screen could want.
 * Width and height are unsigned; the strides are deliberately signed so
 * a bottom-up rectangle, or a downward overlapping copy, is expressed
 * the same way the 2D DMA controller expresses it.
 */
static bool blit_geom_ok(RISCOSBlitterState *s)
{
    if (s->width > BLIT_MAX_WIDTH || s->height > BLIT_MAX_HEIGHT) {
        return false;
    }
    return (uint64_t)s->width * s->height <= BLIT_MAX_BYTES;
}

/*
 * Resolve a framebuffer-relative rectangle to an address, proving that
 * every row lands inside the framebuffer.  The rows are walked rather
 * than just the first and last because a signed stride can step out of
 * the buffer and back in again.
 */
static bool blit_fb_resolve(RISCOSBlitterState *s, uint64_t *addr,
                            int32_t stride, bool *have_fb)
{
    BCM2835FBConfig cfg;
    Object *obj = object_resolve_path_type("", TYPE_BCM2835_FB, NULL);
    int64_t off = (int64_t)(uint32_t)*addr;
    uint32_t fbsize;

    *have_fb = (obj != NULL);
    if (!obj) {
        return false;
    }
    bcm2835_fb_get_config(BCM2835_FB(obj), &cfg);
    fbsize = bcm2835_fb_get_size(&cfg);

    for (uint32_t y = 0; y < s->height; y++) {
        if (off < 0 || (uint64_t)off + s->width > fbsize) {
            return false;
        }
        off += stride;
    }
    *addr = cfg.base + (uint32_t)*addr;
    return true;
}

static uint32_t blit_fill(RISCOSBlitterState *s)
{
    uint64_t dest = s->dest;
    g_autofree uint8_t *row = NULL;
    uint8_t pat[16];

    if (s->patlen == 0 || s->patlen > sizeof(pat)) {
        return BLIT_RC_BADGEOM;
    }
    if (s->flags & BLIT_F_FB) {
        bool have_fb;
        if (!blit_fb_resolve(s, &dest, s->dstride, &have_fb)) {
            return have_fb ? BLIT_RC_RANGE : BLIT_RC_NOFB;
        }
    }

    memcpy(pat, s->pattern, sizeof(pat));

    /*
     * One row built once and written height times.  For a solid colour
     * every repeat is identical, so the phase of the pattern at the
     * start of each row does not matter; a positional pattern is not
     * something this interface can express, and should not try.
     */
    row = g_malloc(s->width);
    for (uint32_t off = 0; off < s->width; off += s->patlen) {
        memcpy(row + off, pat, MIN(s->patlen, s->width - off));
    }

    for (uint32_t y = 0; y < s->height; y++) {
        dma_memory_write(&address_space_memory, dest, row, s->width,
                         MEMTXATTRS_UNSPECIFIED);
        dest = (uint64_t)((int64_t)dest + s->dstride) & 0xffffffffULL;
    }

    s->n_fill++;
    s->bytes += (uint64_t)s->width * s->height;
    return BLIT_RC_OK;
}

static uint32_t blit_copy(RISCOSBlitterState *s)
{
    uint64_t dest = s->dest, src = s->src;
    g_autofree uint8_t *row = NULL;

    if (s->flags & BLIT_F_FB) {
        bool have_fb;
        if (!blit_fb_resolve(s, &dest, s->dstride, &have_fb) ||
            !blit_fb_resolve(s, &src, s->sstride, &have_fb)) {
            return have_fb ? BLIT_RC_RANGE : BLIT_RC_NOFB;
        }
    }

    /*
     * Rows go through a bounce buffer, so a row overlapping itself is
     * safe whichever way it moves.  Overlap *between* rows is the
     * caller's to get right by the sign of the strides, exactly as it
     * would be when driving the DMA controller.
     */
    row = g_malloc(s->width);
    for (uint32_t y = 0; y < s->height; y++) {
        dma_memory_read(&address_space_memory, src, row, s->width,
                        MEMTXATTRS_UNSPECIFIED);
        dma_memory_write(&address_space_memory, dest, row, s->width,
                         MEMTXATTRS_UNSPECIFIED);
        src = (uint64_t)((int64_t)src + s->sstride) & 0xffffffffULL;
        dest = (uint64_t)((int64_t)dest + s->dstride) & 0xffffffffULL;
    }

    s->n_copy++;
    s->bytes += (uint64_t)s->width * s->height;
    return BLIT_RC_OK;
}

static void blit_go(RISCOSBlitterState *s)
{
    uint32_t rc;

    if (!blit_geom_ok(s)) {
        rc = BLIT_RC_BADGEOM;
    } else if (s->width == 0 || s->height == 0) {
        rc = BLIT_RC_OK;                /* nothing to do, but not an error */
    } else {
        switch (s->op) {
        case BLIT_OP_NOP:
            rc = BLIT_RC_OK;
            break;
        case BLIT_OP_FILL:
            rc = blit_fill(s);
            break;
        case BLIT_OP_COPY:
            rc = blit_copy(s);
            break;
        default:
            rc = BLIT_RC_BADOP;
            break;
        }
    }

    trace_riscos_blitter_go(s->op, s->width, s->height,
                            s->dstride, s->sstride, rc);
    s->status = rc;
}

static uint64_t blit_read(void *opaque, hwaddr offset, unsigned size)
{
    RISCOSBlitterState *s = RISCOS_BLITTER(opaque);

    switch (offset) {
    case BLIT_MAGIC:
        return BLIT_MAGIC_VALUE;
    case BLIT_VERSION:
        return BLIT_VERSION_VALUE;
    case BLIT_FEATURES:
        return BLIT_FEATURE_FILL | BLIT_FEATURE_COPY;
    case BLIT_GO:
        return s->status;
    default:
        return 0;
    }
}

static void blit_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    RISCOSBlitterState *s = RISCOS_BLITTER(opaque);
    uint32_t v = value;

    switch (offset) {
    case BLIT_GO:      blit_go(s);      break;
    case BLIT_OP:      s->op = v;       break;
    case BLIT_FLAGS:   s->flags = v;    break;
    case BLIT_DEST:    s->dest = v;     break;
    case BLIT_SRC:     s->src = v;      break;
    case BLIT_WIDTH:   s->width = v;    break;
    case BLIT_HEIGHT:  s->height = v;   break;
    case BLIT_DSTRIDE: s->dstride = v;  break;
    case BLIT_SSTRIDE: s->sstride = v;  break;
    case BLIT_PATLEN:  s->patlen = v;   break;
    default:
        if (offset >= BLIT_PATTERN && offset < BLIT_PATTERN + 16) {
            s->pattern[(offset - BLIT_PATTERN) / 4] = v;
        }
        break;
    }
}

static const MemoryRegionOps blit_ops = {
    .read = blit_read,
    .write = blit_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void blit_realize(DeviceState *dev, Error **errp)
{
    RISCOSBlitterState *s = RISCOS_BLITTER(dev);

    memory_region_init_io(&s->mr, OBJECT(dev), &blit_ops, s,
                          TYPE_RISCOS_BLITTER, BLIT_REGION_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mr);
}

static void blit_reset(DeviceState *dev)
{
    RISCOSBlitterState *s = RISCOS_BLITTER(dev);

    s->op = s->flags = s->dest = s->src = 0;
    s->width = s->height = s->patlen = 0;
    s->dstride = s->sstride = 0;
    memset(s->pattern, 0, sizeof(s->pattern));
    s->status = BLIT_RC_OK;
    s->n_fill = s->n_copy = s->bytes = 0;
}

static const VMStateDescription blit_vmstate = {
    .name = TYPE_RISCOS_BLITTER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(op, RISCOSBlitterState),
        VMSTATE_UINT32(flags, RISCOSBlitterState),
        VMSTATE_UINT32(dest, RISCOSBlitterState),
        VMSTATE_UINT32(src, RISCOSBlitterState),
        VMSTATE_UINT32(width, RISCOSBlitterState),
        VMSTATE_UINT32(height, RISCOSBlitterState),
        VMSTATE_UINT32(patlen, RISCOSBlitterState),
        VMSTATE_INT32(dstride, RISCOSBlitterState),
        VMSTATE_INT32(sstride, RISCOSBlitterState),
        VMSTATE_UINT32_ARRAY(pattern, RISCOSBlitterState, 4),
        VMSTATE_UINT32(status, RISCOSBlitterState),
        VMSTATE_UINT64(n_fill, RISCOSBlitterState),
        VMSTATE_UINT64(n_copy, RISCOSBlitterState),
        VMSTATE_UINT64(bytes, RISCOSBlitterState),
        VMSTATE_END_OF_LIST()
    }
};

static void blit_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = blit_realize;
    device_class_set_legacy_reset(dc, blit_reset);
    dc->vmsd = &blit_vmstate;
}

static const TypeInfo blit_type = {
    .name = TYPE_RISCOS_BLITTER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RISCOSBlitterState),
    .class_init = blit_class_init,
};

static void blit_register(void)
{
    type_register_static(&blit_type);
}

type_init(blit_register);
