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
#include "hw/core/cpu.h"
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
static Object *blit_fb(RISCOSBlitterState *s)
{
    if (!s->fb) {
        s->fb = object_resolve_path_type("", TYPE_BCM2835_FB, NULL);
    }
    return s->fb;
}

static bool blit_fb_resolve(RISCOSBlitterState *s, uint64_t *addr,
                            int32_t stride, bool *have_fb)
{
    BCM2835FBConfig cfg;
    Object *obj = blit_fb(s);
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

/*
 * Rows written one at a time cost an address-space dispatch each, and a
 * full-screen clear is a thousand of them.  Where the rectangle covers
 * whole rows back to back it is really one run, so write it in big
 * pieces instead -- measured on an M4, one contiguous write of 9 MB
 * costs 68us against 118us row by row, and that is before the dispatch
 * overhead this saves.
 *
 * Only when the width divides by the pattern length, because then a
 * continuous run and a per-row one put the same byte in the same place;
 * otherwise the phase would restart every row and the two disagree.
 */
static bool blit_contiguous(RISCOSBlitterState *s, int32_t stride)
{
    return (int64_t)stride == (int64_t)s->width
           && s->patlen != 0 && s->width % s->patlen == 0;
}

/*
 * The span a strided rectangle covers, from the lowest byte any row
 * touches to the highest.  A negative stride runs upwards through
 * memory, so the first row is not always the lowest.
 */
static void blit_span(RISCOSBlitterState *s, int32_t stride,
                      int64_t *lo, uint64_t *span)
{
    int64_t last = (int64_t)(s->height - 1) * stride;

    if (stride >= 0) {
        *lo = 0;
        *span = (uint64_t)(last + s->width);
    } else {
        *lo = last;
        *span = (uint64_t)(s->width - last);
    }
}

/*
 * The framebuffer is ordinary RAM, so the whole rectangle can be mapped
 * once and written with plain stores rather than a dma_memory_write per
 * row.  It matters more than the contiguous case does: a real session
 * logged 5878 fills averaging 108 KB, and only eleven of them spanned
 * whole rows -- the rest are windows, hundreds of rows each, and every
 * row was costing an address-space lookup.
 *
 * Returns NULL for anything that is not plain RAM, or that the address
 * space would only map in pieces, and the caller falls back.
 */
static uint8_t *blit_map(uint64_t first, uint64_t span, hwaddr *got)
{
    hwaddr len = span;
    void *p;

    if (span == 0 || span > BLIT_MAX_BYTES) {
        return NULL;
    }
    p = address_space_map(&address_space_memory, first, &len, true,
                          MEMTXATTRS_UNSPECIFIED);
    if (p && len == span) {
        *got = len;
        return p;
    }
    if (p) {
        address_space_unmap(&address_space_memory, p, len, true, 0);
    }
    return NULL;
}

/*
 * Unmapping with the full span as the access length is what marks the
 * pages dirty, which is how the display learns the screen changed.
 */
static void blit_unmap(uint8_t *p, hwaddr len)
{
    address_space_unmap(&address_space_memory, p, len, true, len);
}

static uint8_t *blit_scratch(RISCOSBlitterState *s, uint32_t len)
{
    if (s->scratch_len < len) {
        s->scratch = g_realloc(s->scratch, len);
        s->scratch_len = len;
    }
    return s->scratch;
}

static uint32_t blit_fill(RISCOSBlitterState *s)
{
    uint64_t dest = s->dest;
    uint8_t *row, *host;
    uint8_t pat[16];
    uint32_t chunk;
    hwaddr mapped;
    uint64_t span;
    int64_t lo;

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
     * The pattern is built out once and then written repeatedly.  For a
     * solid colour every repeat is identical, so where the run starts
     * within the pattern does not matter; a positional pattern is not
     * something this interface can express, and should not try.
     */
    row = blit_scratch(s, s->width);
    for (uint32_t off = 0; off < s->width; off += s->patlen) {
        memcpy(row + off, pat, MIN(s->patlen, s->width - off));
    }

    blit_span(s, s->dstride, &lo, &span);
    host = blit_map(dest + lo, span, &mapped);
    if (host) {
        for (uint32_t y = 0; y < s->height; y++) {
            memcpy(host + ((int64_t)y * s->dstride - lo), row, s->width);
        }
        blit_unmap(host, mapped);
    } else if (blit_contiguous(s, s->dstride)) {
        uint64_t total = (uint64_t)s->width * s->height;

        chunk = MIN(total, 1u << 20);
        chunk -= chunk % s->patlen;
        row = blit_scratch(s, chunk);
        for (uint32_t off = 0; off < chunk; off += s->patlen) {
            memcpy(row + off, pat, s->patlen);
        }
        while (total) {
            uint32_t n = MIN(total, chunk);

            dma_memory_write(&address_space_memory, dest, row, n,
                             MEMTXATTRS_UNSPECIFIED);
            dest = (uint64_t)(dest + n) & 0xffffffffULL;
            total -= n;
        }
    } else {
        for (uint32_t y = 0; y < s->height; y++) {
            dma_memory_write(&address_space_memory, dest, row, s->width,
                             MEMTXATTRS_UNSPECIFIED);
            dest = (uint64_t)((int64_t)dest + s->dstride) & 0xffffffffULL;
        }
    }

    s->n_fill++;
    s->bytes += (uint64_t)s->width * s->height;
    return BLIT_RC_OK;
}

static uint32_t blit_copy(RISCOSBlitterState *s)
{
    uint64_t dest = s->dest, src = s->src;
    uint8_t *row;

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
    if ((int64_t)s->dstride == (int64_t)s->width &&
        (int64_t)s->sstride == (int64_t)s->width) {
        uint64_t total = (uint64_t)s->width * s->height;
        uint32_t chunk = MIN(total, 1u << 20);

        row = blit_scratch(s, chunk);
        while (total) {
            uint32_t n = MIN(total, chunk);

            dma_memory_read(&address_space_memory, src, row, n,
                            MEMTXATTRS_UNSPECIFIED);
            dma_memory_write(&address_space_memory, dest, row, n,
                             MEMTXATTRS_UNSPECIFIED);
            src = (uint64_t)(src + n) & 0xffffffffULL;
            dest = (uint64_t)(dest + n) & 0xffffffffULL;
            total -= n;
        }
    } else {
        row = blit_scratch(s, s->width);
        for (uint32_t y = 0; y < s->height; y++) {
            dma_memory_read(&address_space_memory, src, row, s->width,
                            MEMTXATTRS_UNSPECIFIED);
            dma_memory_write(&address_space_memory, dest, row, s->width,
                             MEMTXATTRS_UNSPECIFIED);
            src = (uint64_t)((int64_t)src + s->sstride) & 0xffffffffULL;
            dest = (uint64_t)((int64_t)dest + s->dstride) & 0xffffffffULL;
        }
    }

    s->n_copy++;
    s->bytes += (uint64_t)s->width * s->height;
    return BLIT_RC_OK;
}

/*
 * A sprite plot: rows of pixels out of guest RAM and into the
 * framebuffer.  The guest has already clipped and worked out where the
 * first row lands, so this is a copy -- RISC OS stores sprite rows
 * bottom first, which the caller expresses with a negative destination
 * stride rather than this having to know.
 */
/*
 * Translating the source a row at a time is what a debug interface
 * invites and it is far too slow: a page-table walk per row, and a
 * 128-row sprite pays 128 of them to read 64K that spans sixteen
 * pages.  Walk it a page at a time instead and keep the mapping until
 * the rows leave that page.
 */
typedef struct {
    CPUState *cpu;
    uint64_t page;          /* guest virtual page currently mapped */
    uint8_t *host;          /* ... and where it lives, or NULL */
    void *mapped;
    hwaddr maplen;
    bool valid;
} BlitSrcWin;

static void blit_src_drop(BlitSrcWin *w)
{
    if (w->mapped) {
        address_space_unmap(&address_space_memory, w->mapped, w->maplen,
                            false, 0);
        w->mapped = NULL;
    }
    w->valid = false;
}

static bool blit_src_read(BlitSrcWin *w, uint64_t va, uint8_t *dst,
                          uint32_t len)
{
    while (len) {
        uint64_t page = va & ~(uint64_t)0xfff;
        uint32_t off = va & 0xfff;
        uint32_t n = MIN(len, 0x1000 - off);

        if (!w->valid || w->page != page) {
            TranslateForDebugResult tres;
            hwaddr plen = 0x1000;

            blit_src_drop(w);
            if (!cpu_translate_for_debug(w->cpu, page, &tres)) {
                return false;
            }
            w->mapped = address_space_map(&address_space_memory, tres.physaddr,
                                          &plen, false, tres.attrs);
            if (!w->mapped || plen < 0x1000) {
                blit_src_drop(w);
                return false;
            }
            w->maplen = plen;
            w->host = w->mapped;
            w->page = page;
            w->valid = true;
        }
        memcpy(dst, w->host + off, n);
        dst += n;
        va += n;
        len -= n;
    }
    return true;
}

static uint32_t blit_sprite(RISCOSBlitterState *s)
{
    BCM2835FBConfig cfg;
    Object *obj = blit_fb(s);
    BlitSrcWin win = { .cpu = current_cpu ? current_cpu : first_cpu };
    uint8_t *row, *fbhost = NULL;
    hwaddr fbmapped = 0;
    uint32_t pitch, fbsize, bpp = s->bpp;
    uint32_t rc = BLIT_RC_OK;
    int32_t x0, y0, x1, y1;

    if (!obj) {
        return BLIT_RC_NOFB;
    }
    if ((s->flags & BLIT_F_SRC_VIRT) && !win.cpu) {
        return BLIT_RC_FAULT;
    }
    if (bpp == 0 || bpp > 4) {
        return BLIT_RC_BADGEOM;
    }
    bcm2835_fb_get_config(BCM2835_FB(obj), &cfg);
    pitch = bcm2835_fb_get_pitch(&cfg);
    fbsize = bcm2835_fb_get_size(&cfg);

    /* The sprite, against the caller's clip rectangle and the screen.
     * All inclusive, all top-left origin. */
    x0 = MAX(s->dstx, s->clipx0);
    y0 = MAX(s->dsty, s->clipy0);
    x1 = MIN(s->dstx + (int32_t)s->width - 1, s->clipx1);
    y1 = MIN(s->dsty + (int32_t)s->height - 1, s->clipy1);
    x0 = MAX(x0, 0);
    y0 = MAX(y0, 0);
    x1 = MIN(x1, (int32_t)cfg.xres - 1);
    y1 = MIN(y1, (int32_t)cfg.yres - 1);
    if (x1 < x0 || y1 < y0) {
        return BLIT_RC_OK;                  /* wholly clipped */
    }

    {
        uint32_t w = (uint32_t)(x1 - x0 + 1);
        uint32_t h = (uint32_t)(y1 - y0 + 1);
        uint32_t skip_x = (uint32_t)(x0 - s->dstx);
        uint32_t skip_y = (uint32_t)(y0 - s->dsty);
        uint64_t first = (uint64_t)y0 * pitch + (uint64_t)x0 * bpp;
        uint64_t span = (uint64_t)(h - 1) * pitch + (uint64_t)w * bpp;

        if ((uint64_t)w * bpp > BLIT_MAX_WIDTH || h > BLIT_MAX_HEIGHT) {
            return BLIT_RC_BADGEOM;
        }
        if (first + span > fbsize) {
            return BLIT_RC_RANGE;
        }
        row = blit_scratch(s, w * bpp);
        /*
         * Mapping the whole span pays for the gaps between rows: a
         * 128-row sprite on a 1920-wide screen spans a megabyte to
         * write 64K, and unmapping dirties all of it.  Only worth it
         * when the rows nearly touch; otherwise write them one at a
         * time, which dirties exactly what changed.
         */
        if ((uint64_t)w * bpp * 2 >= pitch) {
            fbhost = blit_map(cfg.base + first, span, &fbmapped);
        }

        for (uint32_t j = 0; j < h; j++) {
            uint32_t sy = skip_y + j;
            uint64_t src, dest;

            if (s->flags & BLIT_F_BOTTOM_UP) {
                sy = s->height - 1 - sy;
            }
            src = s->src + (uint64_t)sy * s->sstride + (uint64_t)skip_x * bpp;

            if (s->flags & BLIT_F_SRC_VIRT) {
                if (!blit_src_read(&win, src, row, w * bpp)) {
                    rc = BLIT_RC_FAULT;
                    break;
                }
            } else {
                dma_memory_read(&address_space_memory, src, row, w * bpp,
                                MEMTXATTRS_UNSPECIFIED);
            }
            if (fbhost) {
                memcpy(fbhost + (uint64_t)j * pitch, row, w * bpp);
            } else {
                dest = cfg.base + first + (uint64_t)j * pitch;
                dma_memory_write(&address_space_memory, dest, row, w * bpp,
                                 MEMTXATTRS_UNSPECIFIED);
            }
        }

        blit_src_drop(&win);
        if (fbhost) {
            blit_unmap(fbhost, fbmapped);
        }
        if (rc == BLIT_RC_OK) {
            s->n_copy++;
            s->bytes += (uint64_t)w * h * bpp;
        }
    }
    return rc;
}

static void blit_go(RISCOSBlitterState *s)
{
    int64_t t0 = g_get_monotonic_time();
    uint32_t rc;

    if (!blit_geom_ok(s)) {
        rc = BLIT_RC_BADGEOM;
    } else if (s->op == BLIT_OP_SPRITE) {
        rc = blit_sprite(s);            /* geometry is in pixels, not bytes */
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
        case BLIT_OP_SPRITE:
            rc = blit_sprite(s);
            break;
        default:
            rc = BLIT_RC_BADOP;
            break;
        }
    }

    trace_riscos_blitter_go(s->op, s->width, s->height,
                            s->dstride, s->sstride, rc,
                            (uint32_t)(g_get_monotonic_time() - t0));
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
        return BLIT_FEATURE_FILL | BLIT_FEATURE_COPY | BLIT_FEATURE_SPRITE;
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
    case BLIT_DSTX:    s->dstx = v;     break;
    case BLIT_DSTY:    s->dsty = v;     break;
    case BLIT_CLIPX0:  s->clipx0 = v;   break;
    case BLIT_CLIPY0:  s->clipy0 = v;   break;
    case BLIT_CLIPX1:  s->clipx1 = v;   break;
    case BLIT_CLIPY1:  s->clipy1 = v;   break;
    case BLIT_BPP:     s->bpp = v;      break;
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
    s->fb = NULL;
    /* the scratch is a cache, not state: it survives a reset */
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
        VMSTATE_INT32(dstx, RISCOSBlitterState),
        VMSTATE_INT32(dsty, RISCOSBlitterState),
        VMSTATE_INT32(clipx0, RISCOSBlitterState),
        VMSTATE_INT32(clipy0, RISCOSBlitterState),
        VMSTATE_INT32(clipx1, RISCOSBlitterState),
        VMSTATE_INT32(clipy1, RISCOSBlitterState),
        VMSTATE_UINT32(bpp, RISCOSBlitterState),
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

static void blit_finalize(Object *obj)
{
    RISCOSBlitterState *s = RISCOS_BLITTER(obj);

    g_free(s->scratch);
    s->scratch = NULL;
    s->scratch_len = 0;
}

static const TypeInfo blit_type = {
    .name = TYPE_RISCOS_BLITTER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RISCOSBlitterState),
    .instance_finalize = blit_finalize,
    .class_init = blit_class_init,
};

static void blit_register(void)
{
    type_register_static(&blit_type);
}

type_init(blit_register);
