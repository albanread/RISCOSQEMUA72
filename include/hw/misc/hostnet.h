/*
 * HostNet — the guest's sockets, served by the host.
 *
 * The second doorbell.  HostFS (hw/misc/vmchannel.c) forwards filing
 * operations; this forwards the Internet module's socket SWIs, so RISC OS
 * does no networking of its own: no IP, no ARP, no routing, no DHCP, no
 * emulated NIC.  The reasoning, the measurements that led here and the
 * plan are in ROS_PRIVATE design/HOSTNET.md and HOSTNET-SPRINTS.md.
 *
 * It lives at the BCM2711 GENET MAC's address, 0xFD580000 in the guest's
 * low peripheral window.  That window was an unimplemented-device
 * stand-in whose only job was to read as zero so EtherGENET's revision
 * check declined politely instead of aborting.  With the guest out of the
 * networking business EtherGENET is unplugged, the stand-in has nothing
 * left to do, and the 64 KB is a known-sized hole at a known address that
 * RISC OS maps the same way it maps the HostFS doorbell.  Squatting there
 * is not modelling GENET; it is using the hole.
 *
 * A request is one register frame.  The module copies the SWI's R0-R7
 * into the block, writes the block's address to CMD, and copies R0-R7
 * back out; it does not interpret them.  Addresses inside the frame are
 * the guest's own logical addresses, exactly as the application passed
 * them, and the host reaches them by walking the guest MMU
 * (vmch_guest_rw).  So send(s, buf, 65536) reads the application's buffer
 * where it lies: no bounce buffer, no window copy, no packets.
 */

#ifndef HW_MISC_HOSTNET_H
#define HW_MISC_HOSTNET_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

/* ---- registers ------------------------------------------------------ */

#define HN_MAGIC        0x00    /* reads 'HNET' */
#define HN_VERSION      0x04    /* protocol version: 1 */
#define HN_FEATURES     0x08    /* bitmask, below */
#define HN_CMD          0x0c    /* write: request block's guest address */

#define HN_REGION_SIZE  0x10000 /* the GENET window, whole */

#define HN_MAGIC_VALUE   0x54454E48   /* 'H','N','E','T' little-endian */
#define HN_VERSION_VALUE 1

#define HN_FEATURE_SOCKETS 0x1  /* socket commands are served */

/*
 * Request block: 64-byte header, then any inline data.  All fields
 * little-endian.  The layout deliberately mirrors the vmchannel v1
 * header, so the two modules' transports read the same way.
 */
#define HN_HDR_CMD      0
#define HN_HDR_SEQ      4
#define HN_HDR_RC       8       /* transport result, HN_RC_* */
#define HN_HDR_ERRNO   12       /* BSD errno for the guest, 0 = none */
#define HN_HDR_SWI     16       /* which socket SWI, 0-34 */
#define HN_HDR_REGS    20       /* R0..R7, +20 .. +51 */
#define HN_HDR_RESULT  52       /* value to return in R0 */
#define HN_HDR_SIZE    64

/* commands */
#define HN_CMD_PING     0
#define HN_CMD_SWI      1       /* one socket SWI, by HN_HDR_SWI */
/*
 * Which sockets have woken since last asked.  The host cannot call into
 * the guest, so Internet Event 19 is raised by the module from a ticker,
 * and this is what it asks on each tick.  Edge-triggered: a socket is
 * reported once when it becomes readable, not on every tick until it is
 * read, which would bury the guest in events.
 *
 * The answer is a count in HN_HDR_RESULT and that many words from
 * HN_HDR_SIZE, each
 *
 *     (local port << 16) | (reason << 8) | descriptor
 *
 * which is R3, R1 and R2 of the event, ready to use.  The descriptor fits
 * in eight bits because there are 256 of them, which leaves the middle
 * byte free for the reason.
 */
#define HN_CMD_POLL     2

#define HN_POLL_MAX     16      /* sockets reported in one tick */

/*
 * Internet Event 19's reason codes, from the Internet module's
 * lib/c/unixenv: SIGIO, SIGURG and SIGPIPE as a RISC OS program sees them.
 */
#define HN_EV_ASYNC     1       /* something to read */
#define HN_EV_URGENT    2       /* out-of-band data */
#define HN_EV_BROKEN    3       /* the connection has gone */

/* transport result codes.  A socket call that fails for an ordinary
 * networking reason is HN_RC_OK with HN_HDR_ERRNO set: that is not a
 * transport failure, it is the answer. */
#define HN_RC_OK        0
#define HN_RC_BADCMD    1       /* unknown command */
#define HN_RC_BADSWI    2       /* SWI index out of range */
#define HN_RC_BADADDR   3       /* a guest address would not translate */
#define HN_RC_NOSOCKETS 4       /* present, but sockets not enabled */
/*
 * Not an error and not an answer: the call would have blocked on a socket
 * the guest believes is blocking.  The host cannot wait -- it is inside
 * the vCPU's MMIO write, holding the BQL -- so the guest waits and rings
 * again.  That is where RISC OS's blocking semantics actually live.
 */
#define HN_RC_RETRY     5

/*
 * ioctl requests, from Lib/TCPIPLibs/headers/sys/h/filio through the
 * _IOR/_IOW macros in sys/h/ioccom.  Spelled out rather than recomputed:
 * these are the guest's numbers and do not depend on the host's.
 */
#define HN_FIONREAD     0x4004667F
#define HN_FIONBIO      0x8004667E
#define HN_FIOASYNC     0x8004667D
#define HN_FIOSETOWN    0x8004667C
#define HN_FIOGETOWN    0x4004667B

/*
 * One fd_set, in bytes.  FD_SETSIZE is 256 on RISC OS
 * (Lib/TCPIPLibs/headers/sys/h/types) and must agree with HN_MAX_SOCKETS
 * below, because the bit index in the set *is* the descriptor.
 */
#define HN_FDSET_BYTES  32

/*
 * Most one call moves through the doorbell in a single ring.  Not a
 * protocol limit: a bound on what the host will allocate for one request,
 * generous beyond any datagram and any sane stream read.
 */
#define HN_MAX_XFER     (1u << 20)

/* Entries in one readv/writev.  RISC OS's own limit is UIO_MAXIOV; this
 * only has to be larger than anything real and small enough that a bad
 * count cannot ask the host for an absurd allocation. */
#define HN_MAX_IOV      1024

/*
 * The socket SWIs, in chunk order from &41200 — the Internet module's
 * own numbering (Networking/AUN/Internet/build/cmhg/InetHdr).  The six
 * without a _1 suffix that have one are the Internet 4 forms, taking the
 * old 16-bit-family sockaddr; binaries built with COMPAT_INET4 still
 * issue them, so both forms have to work.
 */
enum {
    HN_SWI_CREAT = 0,  HN_SWI_BIND,        HN_SWI_LISTEN,   HN_SWI_ACCEPT,
    HN_SWI_CONNECT,    HN_SWI_RECV,        HN_SWI_RECVFROM, HN_SWI_RECVMSG,
    HN_SWI_SEND,       HN_SWI_SENDTO,      HN_SWI_SENDMSG,  HN_SWI_SHUTDOWN,
    HN_SWI_SETSOCKOPT, HN_SWI_GETSOCKOPT,  HN_SWI_GETPEERNAME,
    HN_SWI_GETSOCKNAME,HN_SWI_CLOSE,       HN_SWI_SELECT,   HN_SWI_IOCTL,
    HN_SWI_READ,       HN_SWI_WRITE,       HN_SWI_STAT,     HN_SWI_READV,
    HN_SWI_WRITEV,     HN_SWI_GETTSIZE,    HN_SWI_SENDTOSM, HN_SWI_SYSCTL,
    HN_SWI_ACCEPT_1,   HN_SWI_RECVFROM_1,  HN_SWI_RECVMSG_1,
    HN_SWI_SENDMSG_1,  HN_SWI_GETPEERNAME_1, HN_SWI_GETSOCKNAME_1,
    HN_SWI_INTERNALLOOKUP, HN_SWI_VERSION,
    HN_SWI_COUNT
};

/* Sockets the guest may hold at once.  The Internet module's socktab is
 * 256 entries (build/h/module) and Socket_Gettsize reports it, so the
 * guest already believes this number. */
#define HN_MAX_SOCKETS 256

#define TYPE_HOSTNET "hostnet"
OBJECT_DECLARE_SIMPLE_TYPE(HostNetState, HOSTNET)

struct HostNetState {
    SysBusDevice parent_obj;
    MemoryRegion mr;

    uint32_t seq;               /* last sequence number seen */
    bool enabled;               /* sockets served: the -global switch, or
                                 * lit mid-session by the window menu's
                                 * HostNet item (ui/dx11.c), never darkened */
    bool rung;                  /* the module has rung since the last reset:
                                 * HostNet is what this boot is running */

    /* Slot -> host socket.  -1 is free; the guest's descriptor is the
     * slot number, allocated lowest-free like the Internet module's
     * getsockslot(), because programs notice if descriptors come back in
     * a different order. */
    int fds[HN_MAX_SOCKETS];

    /* What the guest asked for with FIONBIO.  Every host socket is
     * non-blocking whatever this says; it decides only whether a
     * would-block is reported as EWOULDBLOCK or turned into HN_RC_RETRY
     * for the guest to wait on. */
    bool nonblock[HN_MAX_SOCKETS];

    /* What the guest asked for with FIOASYNC: deliver Internet Event 19
     * when this socket becomes readable.  Recorded from Sprint 1 because
     * the stock Resolver sets it before it will use a socket at all;
     * acted on in Sprint 3, when the events are actually raised. */
    bool async[HN_MAX_SOCKETS];
    uint32_t owner[HN_MAX_SOCKETS];     /* FIOSETOWN / FIOGETOWN */

    /*
     * What HN_CMD_POLL last reported about this socket: 0 for nothing, or
     * one of HN_EV_*.  An event is raised when this *changes*, not merely
     * when the socket is readable.
     *
     * A plain readable/not-readable edge is not enough, and the difference
     * is not academic: a socket that has data and then breaks stays
     * readable throughout, so tracking readability alone reports the first
     * wake and silently swallows the disconnection -- which is the one a
     * program most needs to hear about.
     */
    uint8_t woke[HN_MAX_SOCKETS];

    /* A non-blocking connect() is in flight.  The second call must ask
     * whether it finished rather than start it again -- connect() on a
     * socket already connecting answers EALREADY, which says nothing about
     * whether it worked. */
    bool connecting[HN_MAX_SOCKETS];
};

#endif /* HW_MISC_HOSTNET_H */
