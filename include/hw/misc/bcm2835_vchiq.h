/*
 * BCM2835 VideoCore mailbox channel 3 - VCHIQ
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef BCM2835_VCHIQ_H
#define BCM2835_VCHIQ_H

#include "hw/core/sysbus.h"
#include "qemu/hrtimer.h"
#include "qemu/audio.h"
#include "qom/object.h"

#define TYPE_BCM2835_VCHIQ "bcm2835-vchiq"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835VchiqState, BCM2835_VCHIQ)

/*
 * Slot zero: the structure the guest builds in memory and whose bus address it
 * posts to mailbox channel 3. Offsets were read out of a live RISC OS 5.30
 * guest and agree with the Linux driver's struct vchiq_slot_zero.
 *
 *   +0x00  magic 'VCHI'   +0x04  version, version_min   +0x08 slot_zero_size
 *   +0x0c  slot_size      +0x10  max_slots             +0x14 max_slots_per_side
 *   +0x18  platform_data[2]
 *   +0x20  the VideoCore's shared state ("master")
 *   +0x20+S the ARM's shared state ("slave")
 *
 * S is derived rather than hardcoded: the debug array sits last in the shared
 * state and its length is a build option, so the offsets *within* a side are
 * fixed but the slave's base moves.
 */
#define VCHIQ_SLOT_MAGIC            0x56434849  /* 'VCHI' */

#define VCHIQ_SZ_MAGIC              0x00
#define VCHIQ_SZ_VERSION            0x04
#define VCHIQ_SZ_SLOT_ZERO_SIZE     0x08
#define VCHIQ_SZ_SLOT_SIZE          0x0c
#define VCHIQ_SZ_MAX_SLOTS          0x10
#define VCHIQ_SZ_MAX_SLOTS_PER_SIDE 0x14
#define VCHIQ_SZ_MASTER             0x20

/* Fields within one shared state */
#define VCHIQ_SS_INITIALISED        0x00
#define VCHIQ_SS_SLOT_FIRST         0x04
#define VCHIQ_SS_SLOT_LAST          0x08
#define VCHIQ_SS_SLOT_SYNC          0x0c
#define VCHIQ_SS_TRIGGER            0x10
#define VCHIQ_SS_TX_POS             0x1c
#define VCHIQ_SS_RECYCLE            0x20
#define VCHIQ_SS_SLOT_QUEUE_RECYCLE 0x2c
#define VCHIQ_SS_SYNC_TRIGGER       0x30
#define VCHIQ_SS_SYNC_RELEASE       0x3c
#define VCHIQ_SS_SLOT_QUEUE         0x48

/* A remote event: the waiter arms it, the far side fires it and rings a bell */
#define VCHIQ_EV_ARMED              0x00
#define VCHIQ_EV_FIRED              0x04

/* Message types, from VCHIQ_MAKE_MSG(type, srcport, dstport) */
#define VCHIQ_MSG_PADDING           0
#define VCHIQ_MSG_CONNECT           1
#define VCHIQ_MSG_OPEN              2
#define VCHIQ_MSG_OPENACK           3
#define VCHIQ_MSG_CLOSE             4
#define VCHIQ_MSG_DATA              5
#define VCHIQ_MSG_BULK_RX           6
#define VCHIQ_MSG_BULK_TX           7
#define VCHIQ_MSG_BULK_RX_DONE      8
#define VCHIQ_MSG_BULK_TX_DONE      9

#define VCHIQ_MSG_HDR_SIZE          8
#define VCHIQ_MAKE_MSG(t, src, dst) (((t) << 24) | ((src) << 12) | (dst))
#define VCHIQ_MSG_TYPE(id)          ((id) >> 24)
#define VCHIQ_MSG_SRCPORT(id)       (((id) >> 12) & 0xfff)
#define VCHIQ_MSG_DSTPORT(id)       ((id) & 0xfff)

/*
 * The audio service. RISC OS reaches the speaker through this and nothing
 * else: BCMSound calls itself a "VCHIQ audio service controller", opens
 * 'AUDS' and ships PCM over it, so there is no audio hardware to model --
 * see riscos-pi4/SOUND.md. Message layout is Linux's
 * vc_vchi_audioserv_defs.h, which BCMSound's own equates agree with.
 */
#define VCHIQ_FOURCC_AUDS           0x41554453  /* 'AUDS' */
#define VCHIQ_AUDS_VERSION          2
/* Our port for the service. Any value the guest can hold; it records this
 * as the service's remoteport from our OPENACK and checks it thereafter. */
#define VCHIQ_AUDS_VC_PORT          1

#define VC_AUDIO_MSG_TYPE_RESULT    0
#define VC_AUDIO_MSG_TYPE_COMPLETE  1
#define VC_AUDIO_MSG_TYPE_CONFIG    2
#define VC_AUDIO_MSG_TYPE_CONTROL   3
#define VC_AUDIO_MSG_TYPE_OPEN      4
#define VC_AUDIO_MSG_TYPE_CLOSE     5
#define VC_AUDIO_MSG_TYPE_START     6
#define VC_AUDIO_MSG_TYPE_STOP      7
#define VC_AUDIO_MSG_TYPE_WRITE     8

/* s32 type plus 16 bytes of payload, which is what BCMSound dequeues */
#define VC_AUDIO_MSG_WORDS          5
#define VC_AUDIO_MSG_SIZE           (VC_AUDIO_MSG_WORDS * 4)

/*
 * How often the reports go out. Well under a buffer, so the guest's byte
 * count moves smoothly rather than in steps; BCMSound accumulates them
 * and does not care how they are chopped up.
 */
#define VCHIQ_AUDS_TICK_NS          (5 * 1000 * 1000)

/*
 * Room for the samples between the guest's bursts and the sound card's
 * pull. BCMSound runs about 50 ms ahead and only sends more when we
 * report, so this is several times what it can ever have outstanding.
 */
#define VCHIQ_AUDS_RING_BYTES       (128 * 1024)

/*
 * The dispmanx display service -- the pointer, said yes to. RISC OS's
 * hardware pointer is a dispmanx element over this service (BCMVideo
 * s/HWPointer), so answering it makes the pointer a sprite the host
 * composites and takes out of the framebuffer entirely -- see
 * riscos-pi4/GPUDESIGN.md section 2. Message numbers, layouts and the
 * reply protocol are BCMVideo/s/{Dispmanx,HWPointer}, which are the de
 * facto specification the same way Linux's headers were for 'AUDS'.
 */
#define VCHIQ_FOURCC_DISP           0x44495350  /* 'DISP' */
#define VCHIQ_FOURCC_UPDH           0x55504448  /* 'UPDH', update notify */
#define VCHIQ_DISP_VERSION          2
/* Our ports for the two services. The guest records each as its
 * service's remoteport and matches on it thereafter, so they must stay
 * distinct and constant for the session. */
#define VCHIQ_DISP_VC_PORT          2
#define VCHIQ_UPDH_VC_PORT          3

/* EDispman* command numbers, BCMVideo/s/Dispmanx:19-50 */
#define EDISPMAN_RESOURCE_CREATE            3
#define EDISPMAN_RESOURCE_DELETE            5
#define EDISPMAN_DISPLAY_OPEN               8
#define EDISPMAN_DISPLAY_GET_INFO           14
#define EDISPMAN_DISPLAY_CLOSE              15
#define EDISPMAN_UPDATE_START               16
#define EDISPMAN_UPDATE_SUBMIT              17
#define EDISPMAN_ELEMENT_ADD                19
#define EDISPMAN_ELEMENT_REMOVE             21
#define EDISPMAN_ELEMENT_CHANGE_ATTRIBUTES  24
#define EDISPMAN_BULK_WRITE                 27
#define EDISPMAN_NO_REPLY                   (1u << 31)

/* The one layer we serve: the pointer's (Depth_Pointer, BCMVideo:272).
 * ElementAdd with any other layer is refused, which is what keeps
 * GVOverlay inert while the service is open. */
#define EDISPMAN_LAYER_POINTER              2000

/* The ROM's pointer resource is 32x32 ARGB; 64 is a sanity bound. */
#define VCHIQ_DISP_SPRITE_MAX               64
#define VCHIQ_DISP_IMAGE_BYTES \
    (VCHIQ_DISP_SPRITE_MAX * VCHIQ_DISP_SPRITE_MAX * 4)

/* The committed pointer sprite, as the compositor reads it.  Single
 * writer (the main loop under the BQL); the UI thread reads it without
 * a lock, using the generation as a seqlock exactly as the framebuffer
 * config does.  The image words are little-endian 0xAARRGGBB, which is
 * what HWP_Update's REV-plus-alpha leaves in guest memory. */
typedef struct VchiqCursor {
    uint32_t generation;
    bool stale;                 /* read raced a commit: keep the last one */
    bool visible;
    int32_t x, y;               /* dest rect, display pixels, top-left */
    int32_t w, h;               /* dest rect size, display pixels */
    uint32_t img_w, img_h;      /* the sprite's own resolution, texels */
    uint32_t disp_w, disp_h;    /* the display the dest rect is measured in */
    const uint32_t *argb;       /* img_w * img_h words */
} VchiqCursor;

bool bcm2835_vchiq_get_cursor(VchiqCursor *out);

/*
 * The bulk transfers do not carry the samples: they carry the bus address
 * of a pagelist describing where the samples are. Linux's
 * vchiq_pagelist.h is the layout, and RISC OS ships a copy of it.
 *   +0 length (bytes)  +4 type  +6 offset into the first page
 *   +8 addrs[], each a page-aligned address | (consecutive pages - 1)
 */
#define VCHIQ_PAGELIST_LENGTH       0x00
#define VCHIQ_PAGELIST_TYPE         0x04
#define VCHIQ_PAGELIST_OFFSET       0x06
#define VCHIQ_PAGELIST_ADDRS        0x08
#define VCHIQ_PAGELIST_WRITE        0       /* what a guest transmit uses */
#define VCHIQ_PAGE_SIZE             4096
/*
 * How an addrs[] entry packs an address and a run length depends on
 * whether the guest is using 36-bit physical addresses, and on a BCM2711
 * it is. RISC OS's vchiq_riscos.c says it plainly:
 *
 *   With 32bit physical addresses, the top 20 bits are the upper 20 bits
 *   of the address, and the low 12 are the consecutive page count.
 *   With 36bit physical addresses, the top 24 bits are the upper 24 bits
 *   of the address, and the low 8 bits are the consecutive page count.
 *
 * so the entry is calc_bulk_addr(phys) | (pages - 1) with the address
 * already shifted down by four. Reading it the 32-bit way rounds two
 * buffers a page apart to the same address, which is silence that looks
 * like working code.
 */
#define VCHIQ_PL36_COUNT_MASK       0xff
#define VCHIQ_PL36_ADDR_SHIFT       4
/* A guest's buffer is a couple of KB; this is only a sanity bound */
#define VCHIQ_BULK_MAX              (1 << 20)

/* Doorbell registers, relative to the region base (mailbox base + 0x40) */
#define VCHIQ_BELL0                 0x00  /* VC -> ARM, read to clear */
#define VCHIQ_BELL2                 0x08  /* ARM -> VC */
#define VCHIQ_BELL_RUNG             0x04  /* the only bit the guest's ISR tests */

struct BCM2835VchiqState {
    /*< private >*/
    SysBusDevice busdev;
    /*< public >*/

    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    MemoryRegion iomem_mbox;    /* channel-3 window inside mbox_mr */
    MemoryRegion iomem_bell;    /* doorbells, overlaid on peri_mr */
    qemu_irq bell_irq;

    uint32_t bell0;             /* VC->ARM pending bits */
    uint32_t slot0;             /* bus address of slot zero, 0 when idle */
    uint32_t master_base;
    uint32_t slave_base;
    uint32_t slot_size;
    uint32_t max_slots;         /* from slot zero, bound every slot index by */
    uint32_t per_side;          /* entries in each side's slot queue */
    uint32_t tx_slot;           /* the slot we last wrote a message into */
    uint32_t tx_pos;            /* monotonic byte position in our stream */
    uint32_t rx_pos;            /* our cursor into the guest's message stream */
    bool connected;

    /* The audio service, while the guest has it open */
    bool auds_open;
    uint32_t auds_port;         /* the guest's port for it */
    uint32_t auds_rate;         /* from CONFIG: Hz, channels, bits */
    uint32_t auds_channels;
    uint32_t auds_bps;
    uint32_t auds_cookie1;      /* echoed back in COMPLETE */
    uint32_t auds_cookie2;
    bool auds_running;          /* between START and STOP */

    /*
     * COMPLETE is the clock RISC OS makes sound on -- it counts the bytes
     * we report and calls SoundDMA once per buffer's worth -- so the
     * reports are paced on the virtual clock at the rate CONFIG asked
     * for. Sprint 3 moves the pacing onto the audio backend, which is
     * the host's real sound card and cannot drift against it.
     */
    /*
     * The voice. What the host's sound card takes is what gets reported
     * in COMPLETE, so the guest is clocked by the real output device and
     * cannot drift against it -- which is the whole reason the audio
     * backend is the right thing to hang this on rather than a timer.
     */
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    uint8_t *ring;              /* samples waiting for the sound card */
    uint32_t ring_size, ring_head, ring_tail, ring_used;

    /*
     * The fallback, for a machine with no audio backend at all: the same
     * reports paced on the virtual clock instead. Slower and driftier,
     * but it keeps the guest's sound loop turning.
     */
    HRTimer *auds_timer;
    uint64_t auds_outstanding;  /* bytes taken but not yet reported played */
    int64_t auds_played_ns;     /* when the audio queued so far runs out */

    /*
     * The dispmanx display service, while the guest has it open. The
     * staging half holds what the current transaction has gathered;
     * UpdateSubmit commits it into the ptr_* half atomically, which is
     * dispmanx's own vsync semantics and what makes a torn sprite
     * impossible rather than merely unlikely.
     */
    bool disp_open;
    uint32_t disp_port;         /* the guest's port for 'DISP' */
    bool updh_open;
    uint32_t updh_port;
    uint32_t disp_next_handle;  /* minted nonzero: display/resource/update/element */
    uint32_t disp_resource;     /* the pointer's resource, 0 when none */
    uint32_t disp_res_w, disp_res_h;
    uint32_t disp_element;      /* the pointer's element, 0 when none */
    uint32_t disp_bulk_len;     /* length the last BulkWrite announced */
    uint32_t disp_stage[VCHIQ_DISP_SPRITE_MAX * VCHIQ_DISP_SPRITE_MAX];
    uint32_t disp_stage_len;    /* bytes the last bulk gathered */
    bool disp_tx_pending;       /* the transaction touched the pointer */
    bool disp_tx_visible;
    int32_t disp_tx_x, disp_tx_y, disp_tx_w, disp_tx_h;
    uint32_t disp_info_w, disp_info_h;    /* what GetInfo answered */

    uint32_t ptr_image[VCHIQ_DISP_SPRITE_MAX * VCHIQ_DISP_SPRITE_MAX];
    uint32_t ptr_gen;
    bool ptr_visible;
    int32_t ptr_x, ptr_y, ptr_w, ptr_h;
    uint32_t ptr_img_w, ptr_img_h;       /* the sprite's own resolution */

    /* The samples, gathered out of the pagelist each bulk describes */
    uint8_t *bulk_buf;
    uint32_t bulk_buf_size;

    /* -global bcm2835-vchiq.wav=<file>: what the guest is playing, so it
     * can be looked at before there is anything to listen to */
    char *wav_path;
    FILE *wav;
    uint32_t wav_bytes;
};

#endif
