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
#define BLIT_REGION_SIZE 0x1000

#define BLIT_MAGIC_VALUE   0x54494c42  /* 'B','L','I','T' little-endian */
#define BLIT_VERSION_VALUE 0

#define BLIT_FEATURE_FILL  0x1
#define BLIT_FEATURE_COPY  0x2

#define BLIT_F_FB          0x1  /* DEST/SRC are framebuffer byte offsets */

#define BLIT_OP_NOP        0
#define BLIT_OP_FILL       1
#define BLIT_OP_COPY       2

#define BLIT_RC_OK         0
#define BLIT_RC_BADOP      1
#define BLIT_RC_BADGEOM    2
#define BLIT_RC_NOFB       3   /* BLIT_F_FB, but no framebuffer configured */
#define BLIT_RC_RANGE      4   /* a row fell outside the framebuffer */

/*
 * Sanity limits.  A blit runs synchronously with the vCPU stopped, so a
 * wild height would hang the guest rather than fault it; refuse
 * anything no screen could need.
 */
#define BLIT_MAX_WIDTH    (1u << 16)
#define BLIT_MAX_HEIGHT   (1u << 14)
#define BLIT_MAX_BYTES    (64u << 20)

#define TYPE_RISCOS_BLITTER "riscos-blitter"
OBJECT_DECLARE_SIMPLE_TYPE(RISCOSBlitterState, RISCOS_BLITTER)

struct RISCOSBlitterState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mr;

    uint32_t op, flags, dest, src, width, height, patlen;
    int32_t dstride, sstride;
    uint32_t pattern[4];

    uint32_t status;      /* of the last blit */
    uint64_t n_fill;      /* counters, for the trace and for measuring */
    uint64_t n_copy;
    uint64_t bytes;
};

#endif /* HW_MISC_RISCOS_BLITTER_H */
