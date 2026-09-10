/*
 * vmchannel — the doorbell device behind HostFS.
 *
 * A small MMIO block at a physical address no OS on this board names
 * (0xfd400000 on the BCM2711 low-peripheral window: nothing in RISC OS's
 * HAL device table lives at &014xxxxx).  The guest leaves a request block
 * in its own RAM and pokes CMD with the block's physical address; the
 * handler runs the command synchronously against the host and writes the
 * response into the same block.  No queues, no interrupts, no state
 * between requests — see riscos-pi4/FSDESIGN.md.
 *
 * This header is the protocol contract; the HostFS module build mirrors
 * these constants.
 */

#ifndef HW_MISC_VMCHANNEL_H
#define HW_MISC_VMCHANNEL_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

/* Register offsets within the 16 KiB region */
#define VMCH_MAGIC      0x00    /* reads 'VMCH' (0x48434D56 little-endian) */
#define VMCH_VERSION    0x04    /* protocol version: 0 */
#define VMCH_FEATURES   0x08    /* bitmask, see below */
#define VMCH_CMD        0x0c    /* write: request block's guest phys addr */
#define VMCH_STATUS     0x10    /* 0 idle, 1 done (always 1: synchronous) */
#define VMCH_REGION_SIZE 0x4000

#define VMCH_MAGIC_VALUE   0x48434D56
#define VMCH_VERSION_VALUE 0

#define VMCH_FEATURE_FS       0x1     /* file commands (root configured) */
#define VMCH_FEATURE_CONSOLE  0x2
#define VMCH_FEATURE_TIME     0x4

/* Request block: 64-byte header, then arg_len bytes of inline data.
 * All fields little-endian, guest-physical addressing. */
#define VMCH_HDR_CMD       0
#define VMCH_HDR_SEQ       4
#define VMCH_HDR_RC        8
#define VMCH_HDR_HANDLE   12
#define VMCH_HDR_ARGLEN   16
#define VMCH_HDR_ARG      20   /* words .. +64: scratch, per-command */
#define VMCH_HDR_SIZE     64

/* commands */
#define VMCH_CMD_PING      0
#define VMCH_CMD_OPEN      1
#define VMCH_CMD_CLOSE     2
#define VMCH_CMD_READ      3
#define VMCH_CMD_WRITE     4
#define VMCH_CMD_SEEK      5
#define VMCH_CMD_FILEARGS  6
#define VMCH_CMD_CAT       7
#define VMCH_CMD_CREATE    8
#define VMCH_CMD_DELETE    9
#define VMCH_CMD_RENAME   10
#define VMCH_CMD_CONSOLE  16
#define VMCH_CMD_TIME     17

/* rc codes */
#define VMCH_RC_OK        0
#define VMCH_RC_NOROOT    1   /* file commands need root= to be set */
#define VMCH_RC_NOTFOUND  2
#define VMCH_RC_ACCESS    3
#define VMCH_RC_BADPATH   4   /* .., absolute host path, or too long */
#define VMCH_RC_FULL      5
#define VMCH_RC_HANDLES   6   /* out of open-file slots */
#define VMCH_RC_NOTDIR    7
#define VMCH_RC_ISDIR     8
#define VMCH_RC_BADCMD    9
#define VMCH_RC_IOERR    10

/* OPEN: header word at +12 (before handle is filled) is the flags */
#define VMCH_OPEN_READ    0x1
#define VMCH_OPEN_WRITE   0x2
#define VMCH_OPEN_CREATE  0x4
#define VMCH_OPEN_TRUNC   0x8

/* READ/WRITE: word at +12 = guest phys buffer, +16 = length.  Response
 * word +12 = bytes moved.  (arglen = 0 for these.) */

/* SEEK: +12 offset (signed), +16 whence (0 set, 1 cur, 2 end);
 * response +12 = new absolute offset. */

/* FILEARGS response, inline after the header:
 *   +64 u32 size
 *   +68 u32 type (RISC OS 12-bit type; 0xFFF for plain data, 0 = dir)
 *   +72 u32 attrs (RISC OS-style: bit0 owner-read, 1 write, 2 locked...)
 *   +76 u32 date low, centiseconds since 1900
 *   +80 u32 date high (the instant is 40 bits; 0 = unknown)
 * The module synthesises load/exec from type and the 40-bit date. */

/* CAT response: array of 64-byte entries until arglen is exhausted:
 *   +0  48 bytes: name, NUL padded
 *   +48 u32 type, +52 u32 size, +56 u32 attrs, +60 u32 pad */

#define VMCH_MAX_OPEN 16
#define VMCH_MAX_NAME 48
#define VMCH_MAX_ARG  8192

#define TYPE_VMCHANNEL "vmchannel"
OBJECT_DECLARE_SIMPLE_TYPE(VMChannelState, VMCHANNEL)

struct VMChannelState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mr;
    char *root;                     /* host directory, UTF-8; NULL = none */

    int fds[VMCH_MAX_OPEN];
};

#endif /* HW_MISC_VMCHANNEL_H */
