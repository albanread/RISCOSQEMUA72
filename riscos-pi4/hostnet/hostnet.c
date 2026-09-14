/*
 * HostNet — the guest half.  Internet 6.00.
 *
 * It claims the Internet module's name and its SWI chunk (&41200), and
 * every one of the thirty-five socket SWIs does the same thing: copy R0-R7
 * into a request block, ring the doorbell at the GENET window, copy R0
 * back.  It does not interpret the registers, does not know what a socket
 * is, and holds no state beyond the block itself.  All the networking is
 * on the host (hw/misc/hostnet.c).
 *
 * Why the name matters: the disc's boot sequence is full of
 * `RMEnsure Internet 5.40` (!Internet's !Run) and `RMEnsure Internet 5.02`
 * (Omni).  Calling ourselves Internet 6.00 means none of it has to change,
 * and the ROM's Internet 5.67 is unplugged in CMOS instead.  Where both
 * are present the kernel gives the later claimant of an in-use SWI chunk
 * priority (Kernel/s/ModHand), so this wins either way — but relying on
 * that is not the plan, it is the safety net.
 *
 * ABI, from Networking/AUN/Internet/riscos/c/socket_swi: the Internet
 * module casts _kernel_swi_regs straight onto each BSD *_args struct, so
 * R0..Rn are the C arguments in order and the result comes back in R0.
 * An error is V set with a block whose number is &20E00 + errno; socklib's
 * _copyerror subtracts &20E00 to recover errno.  That is the whole
 * contract, and it is why this module can be this small.
 *
 * Sprint 0 (ROS_PRIVATE design/HOSTNET-SPRINTS.md): the host answers
 * Version and Gettsize and refuses the rest with EOPNOTSUPP.  The point is
 * not the stubs, it is that the path — veneer, frame, doorbell, MMU walk,
 * result, RISC OS error — is exercised by all thirty-five from the start.
 */

typedef unsigned int   uint32_t;
typedef int            int32_t;

/* ---- the doorbell --------------------------------------------------- */

/*
 * The BCM2711 GENET MAC's window, which nothing models: physical
 * 0xFD580000, the low peripheral window's base plus the GENET offset.
 *
 * Physical, and mapped at run time.  RISC OS maps peripherals into
 * logical space on demand, not at a fixed window, so the logical address
 * is only known once OS_Memory 13 (MapIOPermanent) has handed it back.
 * Taking the physical address for the logical one costs a data abort
 * during boot with nothing on screen to say why, which is exactly what
 * the first version of this module did.  OS_ValidateAddress does not
 * catch it either: it calls the range valid and the load still aborts.
 */
#define HN_PHYS          0xFD580000u
#define HN_PAGE          0x1000u

#define HN_MAGIC         0x00
#define HN_VERSION       0x04
#define HN_FEATURES      0x08
#define HN_CMD           0x0C

#define HN_MAGIC_VALUE   0x54454E48u   /* 'HNET' */
#define HN_FEATURE_SOCKETS 0x1u

static volatile uint32_t *hn_base;   /* from OS_Memory 13, at init */

#define hn_reg(idx) (hn_base[(idx)])

/* Request block, mirroring include/hw/misc/hostnet.h exactly.  Word
 * indices, because the block is addressed as words on both sides. */
#define H_CMD     0
#define H_SEQ     1
#define H_RC      2
#define H_ERRNO   3
#define H_SWI     4
#define H_REGS    5    /* R0..R7 at words 5..12 */
#define H_RESULT  13
#define H_WORDS   16   /* 64-byte header */

#define HN_CMD_PING  0
#define HN_CMD_SWI   1

#define HN_RC_OK        0
#define HN_RC_BADCMD    1
#define HN_RC_BADSWI    2
#define HN_RC_BADADDR   3
#define HN_RC_NOSOCKETS 4

/*
 * volatile, and not as a formality: the host writes into this block
 * through the guest's MMU while the compiler believes nothing can have
 * touched it.  Without it, an optimising build folds the read of the
 * result back to the poison value written a line earlier and every
 * request appears to fail.  (HostFS avoids this only by building at -O0.)
 */
static volatile uint32_t req[H_WORDS] __attribute__((aligned(16)));
static uint32_t seq;
static uint32_t live;          /* the doorbell answered at init */

/* ---- RISC OS ---------------------------------------------------------- */

typedef struct {
    int32_t errnum;
    char errmess[32];
} _kernel_oserror;

/*
 * Socket errors are returned in cyclic blocks, as the Internet module
 * does (riscos/c/module, "reduce likelihood of an error message being
 * returned from a Socket SWI, a callback going off as the SWI is exited,
 * and a new error being generated overwriting the original one before the
 * application gets a chance to copy it").  Four is what it uses.
 */
#define ERRBLOCKS 4
static _kernel_oserror errblk[ERRBLOCKS];
static uint32_t errslot;

/* The DCI4 error block: errno lands in the bottom seven bits.
 * sys/h/dcistructs: SETDCI4ERRNO(x,y) ((x) = 0x20E00 + ((y) & 0x7f)) */
#define DCI4ERRORBLOCK 0x20E00

static _kernel_oserror err_nodev = { 0x1E4, "Internet: no HostNet device" };
static _kernel_oserror err_nosock = { 0x1E4, "Internet: host serves no sockets" };
static _kernel_oserror err_badswi = { 0x1E4, "Internet: bad socket SWI" };
static _kernel_oserror err_badaddr = { 0x1E4, "Internet: bad address" };

/*
 * OS_Memory 13, MapIOPermanent: R1 physical, R2 size, R3 back the logical
 * address the page now lives at.  Zero on failure, which is the only
 * answer this module acts on.
 */
static volatile uint32_t *os_map_io(uint32_t phys, uint32_t size)
{
    register uint32_t r0 __asm("r0") = 13;
    register uint32_t r1 __asm("r1") = phys;
    register uint32_t r2 __asm("r2") = size;
    register uint32_t r3 __asm("r3");
    register uint32_t v __asm("r4");

    __asm volatile("swi 0x20068\n\tmrs r4, cpsr"
                   : "+r"(r0), "+r"(r1), "+r"(r2), "=r"(r3), "=r"(v)
                   :
                   : "r12", "lr", "cc", "memory");
    if (v & (1u << 28)) {
        return 0;
    }
    return (volatile uint32_t *)r3;
}

static void put_str(char *dst, const char *src, unsigned n)
{
    unsigned i = 0;
    while (i + 1 < n && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

/*
 * The 4.4BSD errno names, which is what an application sees through
 * _inet_err().  Only the ones a socket call can actually produce are
 * spelled out; anything else gets its number, which is the part that
 * matters -- socklib recovers errno by subtracting &20E00 and never
 * parses the text.
 */
static const char *errno_name(uint32_t e)
{
    switch (e) {
    case 9:  return "BADF";
    case 12: return "NOMEM";
    case 13: return "ACCES";
    case 14: return "FAULT";
    case 22: return "INVAL";
    case 23: return "NFILE";
    case 24: return "MFILE";
    case 35: return "WOULDBLOCK";
    case 36: return "INPROGRESS";
    case 37: return "ALREADY";
    case 38: return "NOTSOCK";
    case 39: return "DESTADDRREQ";
    case 40: return "MSGSIZE";
    case 41: return "PROTOTYPE";
    case 42: return "NOPROTOOPT";
    case 43: return "PROTONOSUPPORT";
    case 45: return "OPNOTSUPP";
    case 47: return "AFNOSUPPORT";
    case 48: return "ADDRINUSE";
    case 49: return "ADDRNOTAVAIL";
    case 50: return "NETDOWN";
    case 51: return "NETUNREACH";
    case 53: return "CONNABORTED";
    case 54: return "CONNRESET";
    case 55: return "NOBUFS";
    case 56: return "ISCONN";
    case 57: return "NOTCONN";
    case 60: return "TIMEDOUT";
    case 61: return "CONNREFUSED";
    case 65: return "HOSTUNREACH";
    default: return "Socket error";
    }
}

static _kernel_oserror *sock_error(uint32_t e)
{
    _kernel_oserror *b;

    errslot = (errslot + 1) & (ERRBLOCKS - 1);
    b = &errblk[errslot];
    b->errnum = (int32_t)(DCI4ERRORBLOCK + (e & 0x7F));
    put_str(b->errmess, errno_name(e), sizeof(b->errmess));
    return b;
}

/* ---- the transport ---------------------------------------------------- */

static uint32_t hn_go(void)
{
    req[H_SEQ] = ++seq;
    req[H_RC] = 0xFFFFFFFFu;        /* poison: an unanswered request shows */
    hn_reg(HN_CMD / 4) = (uint32_t)req;
    return req[H_RC];
}

/*
 * One socket SWI.  `regs` is the caller's R0-R9 block, as the generated
 * veneer stacked it; only R0-R7 cross, which is every argument any of
 * these calls takes.  On the way back only R0 is written, because that is
 * all the Internet module ever sets (`if (!error) r->r[0] = rval`).
 */
static _kernel_oserror *hn_call(uint32_t swi, unsigned *regs)
{
    uint32_t rc, i;

    if (!live) {
        return &err_nodev;
    }
    req[H_CMD] = HN_CMD_SWI;
    req[H_SWI] = swi;
    req[H_ERRNO] = 0;
    req[H_RESULT] = 0;
    for (i = 0; i < 8; i++) {
        req[H_REGS + i] = regs[i];
    }

    rc = hn_go();

    switch (rc) {
    case HN_RC_OK:
        break;
    case HN_RC_NOSOCKETS:
        return &err_nosock;
    case HN_RC_BADSWI:
        return &err_badswi;
    case HN_RC_BADADDR:
        return &err_badaddr;
    default:
        return &err_nodev;
    }

    if (req[H_ERRNO]) {
        return sock_error(req[H_ERRNO]);
    }
    regs[0] = req[H_RESULT];
    return 0;
}

/* ---- module entries ---------------------------------------------------- */

int hostnet_init(void *ws)
{
    (void)ws;
    live = 0;

    hn_base = os_map_io(HN_PHYS, HN_PAGE);
    if (!hn_base) {
        return 0;                   /* no device: load, but serve nothing */
    }
    if (hn_reg(HN_MAGIC / 4) != HN_MAGIC_VALUE) {
        return 0;                   /* something else lives there */
    }
    if (!(hn_reg(HN_FEATURES / 4) & HN_FEATURE_SOCKETS)) {
        return 0;                   /* present, sockets switched off */
    }
    live = 1;
    return 0;
}

int hostnet_final(unsigned fatal, void *ws)
{
    (void)fatal;
    (void)ws;
    /* Sprint 1 closes the guest's sockets here.  Nothing holds one yet. */
    live = 0;
    return 0;
}

/* *HostNetPing — is the host there, and does it answer? */
int hostnet_command_ping(const char *tail, int argc, void *ws)
{
    static _kernel_oserror err_ping = { 0x1E4, "HostNet: no answer" };
    uint32_t i;

    (void)tail;
    (void)argc;
    (void)ws;

    if (!live) {
        return (int)&err_nodev;
    }
    for (i = 0; i < H_WORDS; i++) {
        req[i] = 0;
    }
    req[H_CMD] = HN_CMD_PING;
    if (hn_go() != HN_RC_OK || req[H_RESULT] != HN_MAGIC_VALUE) {
        return (int)&err_ping;
    }
    return 0;
}

/*
 * The thirty-five entry points.  One symbol each, because the kernel's
 * decoding table needs one per SWI; each does nothing but name its own
 * number.  Generated into hostnet_swis.inc from swis.txt by
 * build-hostnet.sh, so the order cannot drift from the Internet module's.
 */
#define HN_ENTRY(sym, num)                                   \
    int hn_swi_##sym(unsigned *regs, void *ws)               \
    {                                                        \
        (void)ws;                                            \
        return (int)hn_call(num, regs);                      \
    }

#include "hostnet_swis.inc"
