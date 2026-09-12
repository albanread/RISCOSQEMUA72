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
#define VMCH_VERSION    0x04    /* protocol version: 1 */
#define VMCH_FEATURES   0x08    /* bitmask, see below */
#define VMCH_CMD        0x0c    /* write: request block's guest phys addr */
#define VMCH_STATUS     0x10    /* 0 idle, 1 done (always 1: synchronous) */
#define VMCH_REGION_SIZE 0x4000

#define VMCH_MAGIC_VALUE   0x48434D56
#define VMCH_VERSION_VALUE 1

#define VMCH_FEATURE_FS       0x1     /* file commands (root configured) */
#define VMCH_FEATURE_CONSOLE  0x2
#define VMCH_FEATURE_TIME     0x4
#define VMCH_FEATURE_FSENTRY  0x8     /* v1 register-frame commands */
#define VMCH_FEATURE_VIRTADDR 0x10    /* guest addresses on the wire are
                                       * logical, and the host walks the
                                       * MMU to reach them */

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
#define VMCH_CMD_SETSIZE  18

/* ---- v1: the wire speaks FileSwitch ---------------------------------
 *
 * A v1 request carries a RISC OS register frame verbatim: the module
 * copies R0..R7 in, rings the doorbell, and copies R0..R7 back out.  It
 * does not interpret them.  Addresses inside the frame are the guest's
 * own *logical* addresses, exactly as FileSwitch passed them; the host
 * reaches them through the CPU's MMU, the way semihosting's SYS_READ
 * and SYS_WRITE do.  See riscos-pi4/FSDESIGN-V1.md.
 *
 * The v1 header reuses the v0 words: +12 becomes the RISC OS error
 * number rather than a handle, and the eight registers occupy the
 * scratch area at +20.
 */
#define VMCH_CMD_FS_OPEN      0x100
#define VMCH_CMD_FS_GETBYTES  0x101
#define VMCH_CMD_FS_PUTBYTES  0x102
#define VMCH_CMD_FS_ARGS      0x103
#define VMCH_CMD_FS_CLOSE     0x104
#define VMCH_CMD_FS_FILE      0x105
#define VMCH_CMD_FS_FUNC      0x106

#define VMCH_HDR_ERRNUM   12   /* v1: RISC OS error number, 0 = none */
#define VMCH_HDR_REGS     20   /* v1: R0..R7, +20 .. +51 */
#define VMCH_HDR_PSR      52   /* v1: bit 29 = C flag to return */

/* FSEntry_GetBytes / _PutBytes on a buffered file (PRM 2-544, 2-546):
 *   R1 = the filing system's own handle
 *   R2 = address of the buffer, a guest logical address
 *   R3 = number of bytes
 *   R4 = file offset to transfer at
 * There are no exit registers.  A short read is not an error: the count
 * is a multiple of the file's buffer size, so the final block of a file
 * routinely runs past its extent. */

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
#define VMCH_RC_BADADDR  11   /* a guest address would not translate */

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
 *   +84 u32 load     ready-made: 0xFFFtttDD
 *   +88 u32 exec     ready-made: the low 32 bits of the instant
 * load/exec are computed here rather than in the module.  The module's
 * own version special-cased &FFF to all-ones, so every file in a *Ex
 * listing came back untyped and undated. */

/* CAT response: array of 64-byte entries until arglen is exhausted:
 *   +0  40 bytes: name, NUL padded (the name the guest sees: a ,xxx
 *                 suffix consumed, a host dot shown as a slash)
 *   +40 u32 load, +44 u32 exec   ready-made, so a listing is dated
 *   +48 u32 type, +52 u32 size, +56 u32 attrs, +60 u32 pad */

#define VMCH_MAX_OPEN 16
#define VMCH_MAX_NAME 40
#define VMCH_MAX_ARG  4032   /* one page: 64-byte header + 4032 */

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
