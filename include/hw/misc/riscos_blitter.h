/*
 * riscos-blitter — rectangle fill and copy done by the host.
 *
 * RISC OS asks its video driver to fill and copy rectangles through
 * GraphicsV_Render.  On a real Pi the driver answers CopyRectangle with
 * the 2D DMA controller and declines FillRectangle, so every window
 * background, every CLS and every menu erase is plotted by the CPU a
 * word at a time.  Under emulation that CPU is the slow part, and it is
 * the reason a large window redraws in visible pieces.
 *
 * The rectangle is written into registers and then run by a write to
 * GO.  A descriptor block in guest RAM would be one MMIO write instead
 * of eleven, but it would have to be named by physical address, and a
 * RISC OS module holds a logical one -- the RMA is not identity mapped,
 * so the translation costs an OS_Memory call per rectangle and cache
 * maintenance with it.  Eleven writes cost about a microsecond, which
 * is nothing beside the fill they replace.
 *
 * Offsets, not addresses: with BLIT_F_FB the guest gives byte offsets
 * into the framebuffer and the host supplies the base it already knows.
 * That is the only addressing RISC OS needs here, it saves the guest
 * the translation entirely, and it lets every row be checked against
 * the framebuffer's extent -- so a wrong rectangle spoils the screen
 * rather than memory belonging to something else.
 *
 * It models no real BCM2711 hardware.  It sits beside the HostFS
 * doorbell at 0xFD404000; the guest maps the page with OS_Memory 13
 * rather than assuming an address, because RISC OS builds its logical
 * map from what the HAL asks for and an unclaimed page simply aborts.
 */

#ifndef HW_MISC_RISCOS_BLITTER_H
#define HW_MISC_RISCOS_BLITTER_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

/* Register offsets */
#define BLIT_MAGIC      0x00    /* reads 'BLIT' */
#define BLIT_VERSION    0x04
#define BLIT_FEATURES   0x08
#define BLIT_GO         0x0c    /* write: run; read: last status */
#define BLIT_OP         0x10
#define BLIT_FLAGS      0x14
#define BLIT_DEST       0x18
#define BLIT_SRC        0x1c
#define BLIT_WIDTH      0x20    /* bytes per row */
#define BLIT_HEIGHT     0x24    /* rows */
#define BLIT_DSTRIDE    0x28    /* signed bytes from one row to the next */
#define BLIT_SSTRIDE    0x2c
#define BLIT_PATTERN    0x30    /* four words */
#define BLIT_PATLEN     0x40    /* 1..16, the pattern's repeat length */
/*
 * Sprite plots.  Everything here is in framebuffer pixels with the
 * origin at the top left, so the guest converts out of OS units and
 * its bottom-left origin once and the host does the clipping -- a
 * rectangle intersection is easy to get right in C and fiddly in
 * relocation-free ARM.
 */
#define BLIT_DSTX       0x44    /* left edge of the sprite */
#define BLIT_DSTY       0x48    /* top edge of the sprite */
#define BLIT_CLIPX0     0x4c    /* clip rectangle, inclusive */
#define BLIT_CLIPY0     0x50
#define BLIT_CLIPX1     0x54
#define BLIT_CLIPY1     0x58
#define BLIT_BPP        0x5c    /* bytes per pixel, source and dest */
#define BLIT_MASK       0x60    /* 1bpp transparency mask, guest virtual */
#define BLIT_MSTRIDE    0x64    /* bytes per mask row */
#define BLIT_SRCBPP     0x68    /* bits per source pixel: 1, 2, 4, 8 */
#define BLIT_TABLE      0x6c    /* wide colour table, guest virtual */
#define BLIT_REGION_SIZE 0x1000

#define BLIT_MAGIC_VALUE   0x54494c42  /* 'B','L','I','T' little-endian */
#define BLIT_VERSION_VALUE 0

#define BLIT_FEATURE_FILL  0x1
#define BLIT_FEATURE_COPY  0x2
#define BLIT_FEATURE_SPRITE 0x4
#define BLIT_FEATURE_MASK   0x8
#define BLIT_FEATURE_TABLE  0x10

#define BLIT_F_FB          0x1  /* DEST is a framebuffer byte offset */
/*
 * SRC is a guest *virtual* address.  A sprite lives in a sprite area
 * somewhere in the guest's map, and RISC OS dynamic areas are virtually
 * contiguous but physically scattered, so there is no one physical
 * address to hand over -- the documented alternative is OS_Memory 19
 * with scatter callbacks, per plot.  Letting the host walk the guest's
 * page tables costs the guest nothing and handles the scatter for free.
 */
#define BLIT_F_SRC_VIRT    0x2
/*
 * RISC OS stores a sprite's rows bottom first.  Flagged rather than
 * assumed: if it is ever the other way round this is a one-line change
 * on the guest side instead of a rewrite here.
 */
#define BLIT_F_BOTTOM_UP   0x4
/*
 * A 1bpp transparency mask at BLIT_MASK: one bit per pixel, least
 * significant first, rows padded to whole words, and a set bit means
 * the pixel is plotted.  RISC OS uses this shape for every sprite of
 * two bits per pixel or more.
 */
#define BLIT_F_MASK        0x8
/*
 * The source is packed at BLIT_SRCBPP bits a pixel and each value
 * indexes BLIT_TABLE, one word per entry, giving the screen pixel.
 * That is what RISC OS calls a wide translation table, the form it
 * builds for anything up to 8bpp going to a deeper screen -- which is
 * every old-format sprite the desktop still uses.
 */
#define BLIT_F_TABLE       0x10

#define BLIT_OP_NOP        0
#define BLIT_OP_FILL       1
#define BLIT_OP_COPY       2
#define BLIT_OP_SPRITE     3   /* RAM -> framebuffer, source may be virtual */

#define BLIT_RC_OK         0
#define BLIT_RC_BADOP      1
#define BLIT_RC_BADGEOM    2
#define BLIT_RC_NOFB       3   /* BLIT_F_FB, but no framebuffer configured */
#define BLIT_RC_RANGE      4   /* a row fell outside the framebuffer */
#define BLIT_RC_FAULT      5   /* a source row would not translate */

/*
 * Sanity limits.  A blit runs synchronously with the vCPU stopped, so a
 * wild height would hang the guest rather than fault it; refuse
 * anything no screen could need.
 */
#define BLIT_MAX_WIDTH    (1u << 16)
#define BLIT_MAX_HEIGHT   (1u << 14)
#define BLIT_MAX_BYTES    (64u << 20)

/*
 * Set on the vCPU thread whenever a blit lands, taken and cleared by
 * whoever is drawing the screen.  An atomic word and nothing more: the
 * front end runs on its own thread and must not touch memory regions
 * or take the big lock to ask a question this simple.
 */
void riscos_blitter_note_damage(void);
unsigned riscos_blitter_take_damage(void);

#define TYPE_RISCOS_BLITTER "riscos-blitter"
OBJECT_DECLARE_SIMPLE_TYPE(RISCOSBlitterState, RISCOS_BLITTER)

struct RISCOSBlitterState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mr;

    uint32_t op, flags, dest, src, width, height, patlen;
    int32_t dstride, sstride;
    int32_t dstx, dsty, clipx0, clipy0, clipx1, clipy1;
    uint32_t bpp, mask, mstride, srcbpp, table;
    uint32_t pattern[4];

    /*
     * Scratch for building rows, kept between blits: a full-screen fill
     * would otherwise claim and free a megabyte every time.  Not
     * migrated -- nothing in it outlives a blit.
     */
    uint8_t *scratch;
    uint32_t scratch_len;

    /*
     * The framebuffer device, found once.  object_resolve_path_type()
     * walks the composition tree, which is tens of microseconds -- more
     * than a small blit costs to perform, and it was being paid on
     * every one.  The device does not change identity.
     */
    Object *fb;

    uint32_t status;      /* of the last blit */
    uint64_t n_fill;      /* counters, for the trace and for measuring */
    uint64_t n_copy;
    uint64_t bytes;
};

#endif /* HW_MISC_RISCOS_BLITTER_H */
