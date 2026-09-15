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
 * Two things here are not plain forwarding, and both are the guest doing
 * what only the guest can: the wait loop, because the host answers from
 * inside an MMIO write and cannot block, and Internet Event 19, because
 * the host cannot call in.  See hn_wait_ring and hostnet_c_tick.
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
#define HN_CMD_POLL  2

/* Sockets one poll may report; the reply is this many words after
 * the header, each (local port << 16) | descriptor. */
#define HN_POLL_MAX  16

#define HN_RC_OK        0
#define HN_RC_BADCMD    1
#define HN_RC_BADSWI    2
#define HN_RC_BADADDR   3
#define HN_RC_NOSOCKETS 4
#define HN_RC_RETRY     5    /* would block, and the guest is the one that waits */

/* Which SWI is Select; it is the one call that is not a plain forward. */
#define HN_SWI_SELECT   17

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

/* 252 is the PRM's size, and what the Internet module's own error blocks
 * use.  The 32 this was held "Internet: HostNet needs the emulator"
 * unterminated, and err_notemul overran it. */
typedef struct {
    int32_t errnum;
    char errmess[252];
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

/* OS_ReadMonotonicTime: centiseconds since the machine started.  The only
 * clock a module can read without a SWI that might not be there. */
static uint32_t os_monotonic(void)
{
    register uint32_t r0 __asm("r0");

    __asm volatile("swi 0x20042"
                   : "=r"(r0)
                   :
                   : "r1", "r2", "r3", "r12", "lr", "cc", "memory");
    return r0;
}

/*
 * OS_UpCall 6 (SleepNoMore): offer the machine to anyone who can use it
 * while we wait.  A TaskWindow claims it, lets the Wimp run, and returns;
 * with nothing to claim it, it returns immediately and the loop is a spin.
 *
 * It returns rather than blocking, which is why it can be called on every
 * turn of the loop with a pollword that never changes — that is exactly
 * what the Internet module's tsleep() does with taskwindow_sleep().
 * R1 must still point at a real word: the claimant may read it.
 */
static uint32_t upcall_pollword;

static void os_yield(void)
{
    register uint32_t r0 __asm("r0") = 6;
    register uint32_t r1 __asm("r1") = (uint32_t)&upcall_pollword;

    __asm volatile("swi 0x20033"
                   : "+r"(r0), "+r"(r1)
                   :
                   : "r2", "r3", "r12", "lr", "cc", "memory");
}

/* OS_ReadEscapeState: C set means the user wants out. */
static uint32_t os_escape(void)
{
    register uint32_t v __asm("r0");

    __asm volatile("swi 0x2002C\n\tmrs r0, cpsr"
                   : "=r"(v)
                   :
                   : "r1", "r2", "r3", "r12", "lr", "cc", "memory");
    return (v & (1u << 29)) ? 1 : 0;      /* C */
}

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
static uint32_t hn_ring_once(uint32_t swi, unsigned *regs)
{
    uint32_t i;

    req[H_CMD] = HN_CMD_SWI;
    req[H_SWI] = swi;
    req[H_ERRNO] = 0;
    req[H_RESULT] = 0;
    for (i = 0; i < 8; i++) {
        req[H_REGS + i] = regs[i];
    }
    return hn_go();
}

/*
 * The wait.
 *
 * The host cannot block -- it answers from inside the vCPU's MMIO write,
 * holding the BQL -- so an operation that would have blocked on a socket
 * the guest thinks is blocking comes back HN_RC_RETRY, and the waiting
 * happens here.  That is not a workaround: it is where RISC OS's blocking
 * semantics have always lived, in tsleep()'s spin in the Internet
 * module's lib/c/unixenv.
 *
 * Each turn offers the machine to anyone who can use it (OS_UpCall 6, as
 * tsleep does) and checks Escape.  Outside a TaskWindow nothing claims the
 * upcall and this is a spin -- which is what a blocking RISC OS SWI has
 * always been, and why a long timeout still freezes the desktop.  What it
 * must not do is spin *without* offering, which would stop a TaskWindow
 * multitasking at all.
 */
static _kernel_oserror *hn_wait_ring(uint32_t swi, unsigned *regs,
                                     uint32_t *rc_out)
{
    for (;;) {
        uint32_t rc = hn_ring_once(swi, regs);

        if (rc != HN_RC_RETRY) {
            *rc_out = rc;
            return 0;
        }
        if (os_escape()) {
            return sock_error(4);          /* EINTR */
        }
        os_yield();
    }
}

static _kernel_oserror *hn_select(unsigned *regs);

static _kernel_oserror *hn_call(uint32_t swi, unsigned *regs)
{
    _kernel_oserror *e;
    uint32_t rc;

    if (!live) {
        return &err_nodev;
    }
    if (swi == HN_SWI_SELECT) {
        return hn_select(regs);
    }

    e = hn_wait_ring(swi, regs, &rc);
    if (e) {
        return e;
    }

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

/*
 * Select is the one call the module does not simply forward, because the
 * timeout is the caller's and the host has no way to honour it.  Each ring
 * is a single poll; the host writes the output sets back only when
 * something is ready, so the input sets survive a zero answer and the loop
 * can just ask again.
 *
 * R0 nd, R1 read set, R2 write, R3 except, R4 struct timeval * (0 means
 * wait for ever).  The result is the number of ready descriptors.
 */
static _kernel_oserror *hn_select(unsigned *regs)
{
    uint32_t rc, deadline = 0, timed = 0;

    if (regs[4]) {
        /* struct timeval { long tv_sec; long tv_usec; } -- guest memory,
         * ours to read directly. */
        const volatile uint32_t *tv = (const volatile uint32_t *)regs[4];
        deadline = os_monotonic() + tv[0] * 100u + tv[1] / 10000u;
        timed = 1;
    }

    for (;;) {
        rc = hn_ring_once(HN_SWI_SELECT, regs);
        if (rc != HN_RC_OK) {
            return rc == HN_RC_NOSOCKETS ? &err_nosock
                 : rc == HN_RC_BADSWI    ? &err_badswi
                 : rc == HN_RC_BADADDR   ? &err_badaddr
                 : &err_nodev;
        }
        if (req[H_ERRNO]) {
            return sock_error(req[H_ERRNO]);
        }
        if (req[H_RESULT] != 0) {
            regs[0] = req[H_RESULT];       /* something is ready */
            return 0;
        }
        /* Signed compare, so the monotonic counter wrapping does not turn
         * a short wait into a very long one. */
        if (timed && (int32_t)(os_monotonic() - deadline) >= 0) {
            regs[0] = 0;                   /* timed out: not an error */
            return 0;
        }
        if (os_escape()) {
            return sock_error(4);          /* EINTR */
        }
        os_yield();
    }
}

/* ---- module entries ---------------------------------------------------- */

/* ---- events ----------------------------------------------------------- */

/*
 * Internet Event 19 is how a RISC OS program finds out that a socket has
 * woken.  The Internet module raised it from its own receive path; there
 * is no receive path here, and the host cannot call into the guest, so the
 * module asks on a ticker and raises the events itself.
 *
 * This is not a nicety.  The stock Resolver sets FIOASYNC, sends its
 * query, and then does nothing at all until an event arrives — it never
 * polls and never reads.  Without this, DNS queries go out, answers come
 * back to the host, and the guest never asks for them.
 */
#define HN_TICK_CS   2          /* how often to ask: 50 times a second */

extern void hostnet_tick(void);
extern void hostnet_callback(void);

static volatile uint32_t pollreq[H_WORDS + HN_POLL_MAX]
    __attribute__((aligned(16)));
static uint32_t cb_pending;
static uint32_t tick_count, cb_count, event_count;

/* The static base, which is what R9 holds in a -frwpi build.  Both veneers
 * are handed it as their R12 value and move it straight back into R9. */
static uint32_t static_base(void)
{
    register uint32_t r9 __asm("r9");

    __asm volatile("" : "=r"(r9));
    return r9;
}


/* OS_AddCallBack (R0 = code, R1 = R12 value) */
static void os_add_callback(uint32_t code, uint32_t r12)
{
    register uint32_t a0 __asm("r0") = code;
    register uint32_t a1 __asm("r1") = r12;

    __asm volatile("swi 0x20054"
                   : "+r"(a0), "+r"(a1)
                   :
                   : "r2", "r3", "r12", "lr", "cc", "memory");
}

/* OS_CallEvery, SWI &3C (R0 = delay in cs, R1 = code, R2 = R12 value).
 * Not &3D: that is OS_RemoveTickerEvent, and calling it instead registers
 * nothing, reports nothing, and leaves the ticker silently dead. */
static void os_call_every(uint32_t cs, uint32_t code, uint32_t r12)
{
    register uint32_t a0 __asm("r0") = cs;
    register uint32_t a1 __asm("r1") = code;
    register uint32_t a2 __asm("r2") = r12;

    __asm volatile("swi 0x2003C"
                   : "+r"(a0), "+r"(a1), "+r"(a2)
                   :
                   : "r3", "r12", "lr", "cc", "memory");
}

/* OS_RemoveTickerEvent, SWI &3D (R0 = code, R1 = R12 value) */
static void os_remove_ticker(uint32_t code, uint32_t r12)
{
    register uint32_t a0 __asm("r0") = code;
    register uint32_t a1 __asm("r1") = r12;

    __asm volatile("swi 0x2003D"
                   : "+r"(a0), "+r"(a1)
                   :
                   : "r2", "r3", "r12", "lr", "cc", "memory");
}

/*
 * Service_InternetStatus (&B0), reason 0 = AddressChanged: the stack is up
 * and has an address.
 *
 * Emitted once at init for form's sake.  Everything that used to listen
 * for it -- Freeway, Net, the DHCP client, LanManFS -- is unplugged in a
 * HostNet machine, so nothing is expected to answer; a program loaded
 * later that waits for it would otherwise wait for ever.
 *
 * Only AddressChanged.  InterfaceUpDown wants a device information block
 * in R4, and there is no interface and no DIB to point at; handing a
 * listener a null one would be worse than staying quiet.
 */
static void os_service_internetstatus(void)
{
    register uint32_t r0 __asm("r0") = 0;      /* AddressChanged */
    register uint32_t r1 __asm("r1") = 0xB0;   /* Service_InternetStatus */

    __asm volatile("swi 0x20030"
                   : "+r"(r0), "+r"(r1)
                   :
                   : "r2", "r3", "r12", "lr", "cc", "memory");
}

/* OS_GenerateEvent: R0 event, R1 reason, R2 socket, R3 local port. */
static void os_generate_event(uint32_t ev, uint32_t reason,
                              uint32_t sock, uint32_t port)
{
    register uint32_t a0 __asm("r0") = ev;
    register uint32_t a1 __asm("r1") = reason;
    register uint32_t a2 __asm("r2") = sock;
    register uint32_t a3 __asm("r3") = port;

    __asm volatile("swi 0x20022"
                   : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3)
                   :
                   : "r12", "lr", "cc", "memory");
}

/*
 * The ticker.  IRQ mode, interrupts off, almost nothing safe to call — so
 * it does the one thing that is meant to be called from here, and leaves.
 */
void hostnet_c_tick(void);
void hostnet_c_tick(void)
{
    tick_count++;
    if (!live || cb_pending) {
        return;
    }
    cb_pending = 1;
    os_add_callback((uint32_t)hostnet_callback, static_base());
}

/*
 * The callback.  USR mode, everything available.  Ask the host which
 * sockets have woken and raise one event for each.
 *
 * pollreq is its own request block, separate from the one the SWIs use:
 * a callback can land between a SWI writing the doorbell and reading its
 * answer, and sharing the block would let one overwrite the other.
 */
void hostnet_c_callback(void);
void hostnet_c_callback(void)
{
    uint32_t i, n;

    cb_pending = 0;
    cb_count++;
    if (!live) {
        return;
    }
    for (i = 0; i < H_WORDS; i++) {
        pollreq[i] = 0;
    }
    pollreq[H_CMD] = HN_CMD_POLL;
    pollreq[H_SEQ] = ++seq;
    pollreq[H_RC] = 0xFFFFFFFFu;
    hn_reg(HN_CMD / 4) = (uint32_t)pollreq;
    if (pollreq[H_RC] != HN_RC_OK) {
        return;
    }
    n = pollreq[H_RESULT];
    if (n > HN_POLL_MAX) {
        n = HN_POLL_MAX;
    }
    for (i = 0; i < n; i++) {
        uint32_t w = pollreq[H_WORDS + i];

        /*
         * Event_Internet is 19; the host packed the rest into one word --
         * port in the top half, reason in the middle byte, descriptor in
         * the bottom.  R1 reason, R2 socket, R3 local port.
         */
        os_generate_event(19, (w >> 8) & 0xFFu, w & 0xFFu, w >> 16);
        event_count++;
    }
}

/*
 * Refuse to start unless the doorbell is actually there.
 *
 * This matters more than the usual "stay dormant" politeness, because this
 * module is called Internet and claims the Internet module's SWI chunk.
 * A module that loads and then refuses every call does not sit quietly out
 * of the way -- it shadows the real Internet module, and the kernel gives
 * the later claimant of an in-use chunk priority (Kernel/s/ModHand).  On a
 * real Raspberry Pi that would mean a machine with networking hardware,
 * a working stack in ROM, and nothing able to reach it.
 *
 * So the test is not "can I work?" but "should I exist?", and the answer
 * is no unless the host is on the other side of that window.  Declining
 * leaves the SWI chunk unclaimed and the genuine Internet module in
 * charge, which is exactly right for hardware this was never meant for.
 */
static _kernel_oserror err_notemul = {
    0x1E4, "Internet: HostNet needs the emulator"
};

int hostnet_init(void *ws)
{
    (void)ws;
    live = 0;
    cb_pending = 0;

    hn_base = os_map_io(HN_PHYS, HN_PAGE);
    if (!hn_base || hn_reg(HN_MAGIC / 4) != HN_MAGIC_VALUE) {
        /* No doorbell: this is not our emulator.  Do not load. */
        return (int)&err_notemul;
    }
    if (!(hn_reg(HN_FEATURES / 4) & HN_FEATURE_SOCKETS)) {
        /* The emulator is ours but sockets were not asked for.  Same
         * answer: better no Internet module than a deaf one. */
        return (int)&err_notemul;
    }
    live = 1;

    /* From here the module asks the host, fifty times a second, whether
     * anything has woken.  Nothing else will: the host cannot call in. */
    os_call_every(HN_TICK_CS, (uint32_t)hostnet_tick, static_base());

    /* And tell anyone listening that there is a working stack, which from
     * their point of view is what has just happened. */
    os_service_internetstatus();
    return 0;
}

int hostnet_final(unsigned fatal, void *ws)
{
    (void)fatal;
    (void)ws;
    if (live) {
        /* Before anything else: a ticker pointing into a module that is
         * about to be unplugged is a branch into free memory. */
        os_remove_ticker((uint32_t)hostnet_tick, static_base());
    }
    live = 0;
    return 0;
}

/* OS_Write0: a string to the current output stream. */
static void os_write0(const char *s)
{
    register uint32_t r0 __asm("r0") = (uint32_t)s;

    __asm volatile("swi 0x20002"
                   : "+r"(r0)
                   :
                   : "r1", "r2", "r3", "r12", "lr", "cc", "memory");
}

static void put_u32(char *out, uint32_t v)
{
    char tmp[12];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        *out++ = tmp[--n];
    }
    *out = 0;
}

/*
 * *HostNetInfo — what the module thinks is going on.
 *
 * The ticks and callbacks are here because "the guest never asked the host
 * anything" and "the guest asked and the host had nothing" are impossible
 * to tell apart from the host's side, and the difference is a registered
 * ticker versus a dead one.
 */
int hostnet_command_info(const char *tail, int argc, void *ws);
int hostnet_command_info(const char *tail, int argc, void *ws)
{
    char line[64], num[12];

    (void)tail;
    (void)argc;
    (void)ws;

    os_write0("HostNet: doorbell ");
    os_write0(live ? "live" : "not found");
    os_write0(", ticks ");
    put_u32(num, tick_count);
    os_write0(num);
    os_write0(", callbacks ");
    put_u32(num, cb_count);
    os_write0(num);
    os_write0(", events ");
    put_u32(num, event_count);
    os_write0(num);
    os_write0("\r\n");
    (void)line;
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
