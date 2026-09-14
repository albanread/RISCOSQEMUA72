/*
 * HostNet — the guest's sockets, served by the host.
 *
 * Sprint 1 (ROS_PRIVATE design/HOSTNET-SPRINTS.md): UDP, end to end.
 * Creat, Bind, Sendto, Recvfrom in both ABI forms, Close, Ioctl FIONBIO,
 * Select for readability, Gettsize, Version, Sysctl.  Enough for the
 * stock Resolver module to do DNS over it, unmodified, which is the
 * sprint's test.  Everything else still answers EOPNOTSUPP.
 *
 * Why the host must never block here: this runs synchronously inside the
 * vCPU's MMIO write, under the BQL.  A blocking recvfrom() would freeze
 * the display, the monitor and the machine.  So every host socket is
 * non-blocking, and an operation that would block on a socket the guest
 * believes is blocking comes back as HN_RC_RETRY — the guest spins its
 * own wait loop, which is exactly what it does today (tsleep() in the
 * Internet module's lib/c/unixenv spins on UpCall 6 and Portable_Idle:
 * there is no scheduler to sleep on).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/sockets.h"
#include "hw/misc/hostnet.h"
#include "hw/misc/vmchannel.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

/*
 * poll() and its POLL* flags live in <poll.h>, which qemu/osdep.h does not
 * pull in on macOS; and a socket is closed with closesocket() only on
 * Windows (WinSock) — on POSIX it is plain close().  qemu/osdep.h already
 * declares close() (via <unistd.h>).
 */
#ifndef _WIN32
#include <poll.h>
#define closesocket close
#endif

/*
 * RISC OS's errno values are 4.4BSD's (Lib/TCPIPLibs/headers/sys/h/errno,
 * and the name table in the Internet module's riscos/c/module).  The
 * host's are the host's: native on macOS, and on Windows the C runtime's,
 * which QEMU's socket wrappers set from WSAGetLastError().  Neither is
 * ours to assume, so the mapping below is by name and the numbers are
 * RISC OS's.
 */
enum {
    ROS_EINTR = 4,        ROS_EBADF = 9,          ROS_EACCES = 13,
    ROS_EFAULT = 14,      ROS_EINVAL = 22,        ROS_EMFILE = 24,
    ROS_EWOULDBLOCK = 35, ROS_EINPROGRESS = 36,   ROS_EALREADY = 37,
    ROS_ENOTSOCK = 38,    ROS_EDESTADDRREQ = 39,  ROS_EMSGSIZE = 40,
    ROS_EPROTOTYPE = 41,  ROS_ENOPROTOOPT = 42,   ROS_EPROTONOSUPPORT = 43,
    ROS_EOPNOTSUPP = 45,  ROS_EAFNOSUPPORT = 47,  ROS_EADDRINUSE = 48,
    ROS_EADDRNOTAVAIL = 49, ROS_ENETDOWN = 50,    ROS_ENETUNREACH = 51,
    ROS_ECONNABORTED = 53, ROS_ECONNRESET = 54,   ROS_ENOBUFS = 55,
    ROS_EISCONN = 56,     ROS_ENOTCONN = 57,      ROS_ETIMEDOUT = 60,
    ROS_ECONNREFUSED = 61, ROS_EHOSTUNREACH = 65,
};

static uint32_t hn_errno(void)
{
    switch (errno) {
    case EINTR:           return ROS_EINTR;
    case EBADF:           return ROS_EBADF;
    case EACCES:          return ROS_EACCES;
    case EFAULT:          return ROS_EFAULT;
    case EINVAL:          return ROS_EINVAL;
    case EMFILE:          return ROS_EMFILE;
    case EAGAIN:          return ROS_EWOULDBLOCK;
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:     return ROS_EWOULDBLOCK;
#endif
    case EINPROGRESS:     return ROS_EINPROGRESS;
    case EALREADY:        return ROS_EALREADY;
    case ENOTSOCK:        return ROS_ENOTSOCK;
    case EDESTADDRREQ:    return ROS_EDESTADDRREQ;
    case EMSGSIZE:        return ROS_EMSGSIZE;
    case EPROTOTYPE:      return ROS_EPROTOTYPE;
    case ENOPROTOOPT:     return ROS_ENOPROTOOPT;
    case EPROTONOSUPPORT: return ROS_EPROTONOSUPPORT;
    case EOPNOTSUPP:      return ROS_EOPNOTSUPP;
    case EAFNOSUPPORT:    return ROS_EAFNOSUPPORT;
    case EADDRINUSE:      return ROS_EADDRINUSE;
    case EADDRNOTAVAIL:   return ROS_EADDRNOTAVAIL;
    case ENETDOWN:        return ROS_ENETDOWN;
    case ENETUNREACH:     return ROS_ENETUNREACH;
    case ECONNABORTED:    return ROS_ECONNABORTED;
    case ECONNRESET:      return ROS_ECONNRESET;
    case ENOBUFS:         return ROS_ENOBUFS;
    case EISCONN:         return ROS_EISCONN;
    case ENOTCONN:        return ROS_ENOTCONN;
    case ETIMEDOUT:       return ROS_ETIMEDOUT;
    case ECONNREFUSED:    return ROS_ECONNREFUSED;
    case EHOSTUNREACH:    return ROS_EHOSTUNREACH;
    default:              return ROS_EINVAL;
    }
}

static bool hn_blocked(void)
{
    return errno == EAGAIN
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        || errno == EWOULDBLOCK
#endif
        ;
}

/*
 * Every host socket is non-blocking, always.  There is no portable helper
 * for this in the tree, and the two platforms disagree about which call
 * does it: ioctlsocket is one of the ones QEMU wraps on Windows, fcntl is
 * the POSIX way.  Failure is not checked because there is nothing useful
 * to do about it — the caller would still have to cope with a blocking
 * socket, and it cannot.
 */
static void hn_set_nonblock(int fd)
{
#ifdef _WIN32
    unsigned long one = 1;
    ioctlsocket(fd, FIONBIO, &one);
#else
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) {
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
#endif
}

/* ---- tracing ---------------------------------------------------------- */

/*
 * HOSTNET_TRACE=<file> logs every call: which SWI, its registers in, and
 * what went back.  Off unless the variable is set, and the file is opened
 * once and appended to, like vmchannel's VMCH_TRACE.  Worth having from
 * the first sprint: when a guest program says only "Failed to look up",
 * this is the difference between reading the answer and guessing at it.
 */
static const char * const hn_swi_name[HN_SWI_COUNT] = {
    "Creat", "Bind", "Listen", "Accept", "Connect", "Recv", "Recvfrom",
    "Recvmsg", "Send", "Sendto", "Sendmsg", "Shutdown", "Setsockopt",
    "Getsockopt", "Getpeername", "Getsockname", "Close", "Select",
    "Ioctl", "Read", "Write", "Stat", "Readv", "Writev", "Gettsize",
    "Sendtosm", "Sysctl", "Accept_1", "Recvfrom_1", "Recvmsg_1",
    "Sendmsg_1", "Getpeername_1", "Getsockname_1", "InternalLookup",
    "Version",
};

static void hn_trace(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

static void hn_trace(const char *fmt, ...)
{
    static FILE *f;
    static int tried;
    va_list ap;

    if (!tried) {
        const char *env = getenv("HOSTNET_TRACE");
        tried = 1;
        f = (env && *env) ? fopen(env, "a") : NULL;
    }
    if (!f) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);
}

/* ---- guest memory ---------------------------------------------------- */

static uint32_t hn_ld32(uint64_t addr)
{
    uint32_t v = 0;
    vmch_guest_rw(addr, &v, 4, false);
    return v;
}

static void hn_st32(uint64_t addr, uint32_t v)
{
    vmch_guest_rw(addr, &v, 4, true);
}

/* ---- descriptors ----------------------------------------------------- */

/*
 * Lowest free slot, like the Internet module's getsockslot(): programs do
 * notice if descriptors come back in a different order, and the guest's
 * fd_set is indexed by exactly this number.
 */
static int hn_alloc(HostNetState *s, int fd)
{
    int i;

    for (i = 0; i < HN_MAX_SOCKETS; i++) {
        if (s->fds[i] < 0) {
            s->fds[i] = fd;
            s->nonblock[i] = false;
            s->async[i] = false;
            s->woke[i] = 0;
            s->connecting[i] = false;
            return i;
        }
    }
    return -1;
}

static int hn_fd(HostNetState *s, uint32_t id)
{
    if (id >= HN_MAX_SOCKETS) {
        return -1;
    }
    return s->fds[id];
}

/* ---- addresses -------------------------------------------------------- */

/*
 * A RISC OS sockaddr comes in two shapes and the host's is a third, so
 * each is built field by field rather than copied:
 *
 *   new (the _1 SWIs, 4.4BSD):  len, family, port, addr, zero[8]
 *   old (the plain SWIs):       family as a 16-bit word, then the same
 *   host:                       macOS has sa_len, Windows does not
 *
 * Ports and addresses are network byte order everywhere, so they move as
 * bytes and never through htons().
 */
#define HN_SA_LEN 16

static bool hn_sa_in(uint64_t addr, uint32_t len, struct sockaddr_in *sin)
{
    uint8_t g[HN_SA_LEN];

    if (len < 8 || len > HN_SA_LEN) {
        return false;
    }
    memset(g, 0, sizeof(g));
    if (!vmch_guest_rw(addr, g, len, false)) {
        return false;
    }
    memset(sin, 0, sizeof(*sin));
    sin->sin_family = AF_INET;
    memcpy(&sin->sin_port, g + 2, 2);
    memcpy(&sin->sin_addr, g + 4, 4);
    return true;
}

static void hn_sa_out(uint64_t addr, uint64_t lenaddr, bool newform,
                      const struct sockaddr_in *sin)
{
    uint8_t g[HN_SA_LEN];
    uint32_t want;

    if (!addr) {
        return;
    }
    memset(g, 0, sizeof(g));
    if (newform) {
        g[0] = HN_SA_LEN;          /* sa_len */
        g[1] = AF_INET;
    } else {
        g[0] = AF_INET;            /* 16-bit family, little-endian */
    }
    memcpy(g + 2, &sin->sin_port, 2);
    memcpy(g + 4, &sin->sin_addr, 4);

    want = lenaddr ? hn_ld32(lenaddr) : HN_SA_LEN;
    if (want > HN_SA_LEN) {
        want = HN_SA_LEN;
    }
    vmch_guest_rw(addr, g, want, true);
    if (lenaddr) {
        hn_st32(lenaddr, HN_SA_LEN);
    }
}

/* ---- readability ------------------------------------------------------ */

/*
 * Readiness, for Select and for the wake-up poll.
 *
 * Sprint 1 answered "is it readable?" with a one-byte MSG_PEEK, which is
 * portable and needs no fd-to-SOCKET translation.  It cannot answer "is it
 * writable?", and a non-blocking connect() reports completion precisely by
 * becoming writable, so TCP needs the real thing.
 *
 * The real thing is poll(), and on Windows WSAPoll() — which takes a
 * SOCKET, not the C runtime file descriptor QEMU hands out.  QEMU builds
 * those with _open_osfhandle(), so _get_osfhandle() gives the SOCKET back.
 *
 * POLLERR and POLLHUP are folded into *both* readable and except: a
 * connection that failed or closed must wake a reader, so the read itself
 * can report why, rather than leaving it blocked for ever on a socket that
 * will never speak again.
 */
#define HN_R 1u
#define HN_W 2u
#define HN_X 4u

/*
 * The raw answer.  Kept separate from hn_ready because the event path
 * needs to know *why* a socket woke — readable, urgent or broken are three
 * different Internet Event reasons — and hn_ready deliberately flattens
 * that into what select() is allowed to say.
 */
static short hn_revents(int fd, short ev)
{
#ifdef _WIN32
    WSAPOLLFD p;

    p.fd = (SOCKET)_get_osfhandle(fd);
    p.events = ev;
    p.revents = 0;
    if (p.fd == (SOCKET)INVALID_HANDLE_VALUE || WSAPoll(&p, 1, 0) <= 0) {
        return 0;
    }
    return p.revents;
#else
    struct pollfd p;

    p.fd = fd;
    p.events = ev;
    p.revents = 0;
    if (poll(&p, 1, 0) <= 0) {
        return 0;
    }
    return p.revents;
#endif
}

static uint32_t hn_ready(int fd, uint32_t want)
{
    uint32_t got = 0;
    short ev = 0, re;

    if (want & HN_R) {
        ev |= POLLRDNORM;
    }
    if (want & HN_W) {
        ev |= POLLWRNORM;
    }
    re = hn_revents(fd, ev);

    if (re & (POLLRDNORM | POLLERR | POLLHUP)) {
        got |= HN_R;
    }
    if (re & (POLLWRNORM | POLLERR | POLLHUP)) {
        got |= HN_W;
    }
    if (re & (POLLERR | POLLHUP | POLLPRI)) {
        got |= HN_X;
    }
    return got & (want | HN_X);
}

static bool hn_readable(int fd)
{
    return (hn_ready(fd, HN_R) & HN_R) != 0;
}

/*
 * macOS has no MSG_NOSIGNAL; it has SO_NOSIGPIPE on the socket instead.
 * Either way the guest must not be able to kill the emulator by writing
 * to a connection its peer has closed.
 */
static void hn_no_sigpipe(int fd)
{
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&one, sizeof(one));
#else
    (void)fd;
#endif
}

/*
 * Message flags, guest to host.
 *
 * Translated, never passed through.  The guest's set and the host's do not
 * agree beyond the first few bits, and a flag the host does not recognise
 * is not ignored by it — send() rejects the whole call with EOPNOTSUPP.
 * That is not hypothetical: c-ares sets 0x400 (its own MSG_NOSIGNAL, a
 * value RISC OS does not define at all) on every send, and passing it on
 * made every HTTPS fetch fail at the first byte while the trace showed a
 * perfectly good connected socket.
 *
 * So: the four that mean the same thing on both sides are mapped, and
 * everything else is dropped. MSG_DONTWAIT needs no mapping because the
 * socket is already non-blocking.  MSG_NOSIGNAL is added rather than
 * relayed — a SIGPIPE from a guest writing to a closed socket would take
 * down the emulator, not the guest.
 */
static int hn_msgflags(uint32_t g)
{
    int h = 0;

    if (g & 0x1) {
        h |= MSG_OOB;
    }
    if (g & 0x2) {
        h |= MSG_PEEK;
    }
    if (g & 0x4) {
        h |= MSG_DONTROUTE;
    }
#ifdef MSG_WAITALL
    if (g & 0x40) {
        h |= MSG_WAITALL;
    }
#endif
#ifdef MSG_NOSIGNAL
    h |= MSG_NOSIGNAL;
#endif
    return h;
}

/* ---- the verbs --------------------------------------------------------- */

/* Filled in by each verb; the doorbell turns it into the reply. */
typedef struct {
    int32_t  result;      /* R0 on success */
    uint32_t err;         /* RISC OS errno, 0 = none */
    uint32_t rc;          /* transport, HN_RC_* */
} HNReply;

static void hn_ok(HNReply *r, int32_t v)
{
    r->result = v;
    r->err = 0;
}

static void hn_fail(HNReply *r)
{
    r->result = -1;
    r->err = hn_errno();
}

static void hn_errset(HNReply *r, uint32_t e)
{
    r->result = -1;
    r->err = e;
}

/*
 * Would have blocked.  On a socket the guest set non-blocking that is the
 * answer; on one it believes is blocking it is no answer at all, and the
 * guest is told to wait and ask again.
 */
static void hn_wouldblock(HostNetState *s, HNReply *r, uint32_t id)
{
    if (id < HN_MAX_SOCKETS && !s->nonblock[id]) {
        r->rc = HN_RC_RETRY;
        return;
    }
    hn_errset(r, ROS_EWOULDBLOCK);
}

static void hn_creat(HostNetState *s, uint32_t *R, HNReply *r)
{
    int fd, id;

    if (R[0] != AF_INET) {
        hn_errset(r, ROS_EAFNOSUPPORT);   /* IPv6 is in no sprint yet */
        return;
    }
    fd = qemu_socket(AF_INET, (int)R[1], (int)R[2]);
    if (fd < 0) {
        hn_fail(r);
        return;
    }
    id = hn_alloc(s, fd);
    if (id < 0) {
        closesocket(fd);
        hn_errset(r, ROS_EMFILE);
        return;
    }
    /* Always non-blocking underneath, whatever the guest believes: the
     * doorbell handler holds the BQL and has to return. */
    hn_set_nonblock(fd);
    hn_no_sigpipe(fd);
    hn_ok(r, id);
}

static void hn_bind(HostNetState *s, uint32_t *R, HNReply *r)
{
    int fd = hn_fd(s, R[0]);
    struct sockaddr_in sin;

    if (fd < 0) {
        hn_errset(r, ROS_EBADF);
        return;
    }
    if (!hn_sa_in(R[1], R[2], &sin)) {
        hn_errset(r, ROS_EINVAL);
        return;
    }
    if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        hn_fail(r);
        return;
    }
    hn_ok(r, 0);
}

static void hn_sendto(HostNetState *s, uint32_t *R, HNReply *r)
{
    int fd = hn_fd(s, R[0]);
    struct sockaddr_in sin;
    g_autofree uint8_t *buf = NULL;
    uint32_t len = R[2];
    ssize_t n;

    if (fd < 0) {
        hn_errset(r, ROS_EBADF);
        return;
    }
    if (len > HN_MAX_XFER) {
        len = HN_MAX_XFER;
    }
    buf = g_malloc(len ? len : 1);
    if (!vmch_guest_rw(R[1], buf, len, false)) {
        r->rc = HN_RC_BADADDR;
        return;
    }
    if (R[4]) {
        if (!hn_sa_in(R[4], R[5], &sin)) {
            hn_errset(r, ROS_EINVAL);
            return;
        }
        n = sendto(fd, (char *)buf, len, hn_msgflags(R[3]),
                   (struct sockaddr *)&sin, sizeof(sin));
    } else {
        n = send(fd, (char *)buf, len, hn_msgflags(R[3]));
    }
    if (n < 0) {
        if (hn_blocked()) {
            hn_wouldblock(s, r, R[0]);
        } else {
            hn_fail(r);
        }
        return;
    }
    hn_ok(r, (int32_t)n);
}

/* recvfrom, both ABI forms: `newform` picks which sockaddr goes back. */
static void hn_recvfrom(HostNetState *s, uint32_t *R, HNReply *r,
                        bool newform)
{
    int fd = hn_fd(s, R[0]);
    struct sockaddr_in sin;
    socklen_t slen = sizeof(sin);
    g_autofree uint8_t *buf = NULL;
    uint32_t len = R[2];
    ssize_t n;

    if (fd < 0) {
        hn_errset(r, ROS_EBADF);
        return;
    }
    if (len > HN_MAX_XFER) {
        len = HN_MAX_XFER;
    }
    buf = g_malloc(len ? len : 1);
    memset(&sin, 0, sizeof(sin));
    n = recvfrom(fd, (char *)buf, len, hn_msgflags(R[3]),
                 (struct sockaddr *)&sin, &slen);
    if (n < 0) {
        if (hn_blocked()) {
            hn_wouldblock(s, r, R[0]);
        } else {
            hn_fail(r);
        }
        return;
    }
    if (n > 0 && !vmch_guest_rw(R[1], buf, (uint32_t)n, true)) {
        r->rc = HN_RC_BADADDR;
        return;
    }
    /* R4 is where the sender's address goes, R5 points at its length. */
    hn_sa_out(R[4], R[5], newform, &sin);
    hn_ok(r, (int32_t)n);
}

/*
 * Connect.
 *
 * The host socket is non-blocking, so the first call almost always says
 * EINPROGRESS.  What the guest does with that depends on what it asked
 * for: a non-blocking socket gets EINPROGRESS back and is expected to
 * watch for writability, which is how every BSD program has done this for
 * thirty years; a blocking one gets HN_RC_RETRY and the module waits.
 *
 * On the retry path the second connect() does not start anything — the
 * connection is already in flight — so it is asked about rather than
 * repeated: writable means finished, and SO_ERROR says whether it
 * finished well.  Calling connect() again would report EALREADY or
 * EISCONN and tell us nothing useful.
 */
static void hn_connect(HostNetState *s, uint32_t *R, HNReply *r)
{
    int fd = hn_fd(s, R[0]);
    struct sockaddr_in sin;

    if (fd < 0) {
        hn_errset(r, ROS_EBADF);
        return;
    }
    if (s->connecting[R[0]]) {
        int err = 0;
        socklen_t el = sizeof(err);

        if (!(hn_ready(fd, HN_W) & (HN_W | HN_X))) {
            hn_wouldblock(s, r, R[0]);       /* still on its way */
            return;
        }
        s->connecting[R[0]] = false;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &el) < 0) {
            hn_fail(r);
            return;
        }
        if (err) {
            errno = err;
            hn_fail(r);
            return;
        }
        hn_ok(r, 0);
        return;
    }
    if (!hn_sa_in(R[1], R[2], &sin)) {
        hn_errset(r, ROS_EINVAL);
        return;
    }
    if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) == 0) {
        hn_ok(r, 0);
        return;
    }
    if (errno == EINPROGRESS || errno == EALREADY || hn_blocked()) {
        s->connecting[R[0]] = true;
        if (s->nonblock[R[0]]) {
            hn_errset(r, ROS_EINPROGRESS);
        } else {
            r->rc = HN_RC_RETRY;
        }
        return;
    }
    hn_fail(r);
}

/*
 * send/recv, and the read/write pair that are the same calls with no
 * flags (Socket_Read and Socket_Write take no flags argument at all).
 */
static void hn_sendrecv(HostNetState *s, uint32_t *R, HNReply *r,
                        bool sending, bool flags)
{
    int fd = hn_fd(s, R[0]);
    g_autofree uint8_t *buf = NULL;
    uint32_t len = R[2];
    int fl = flags ? hn_msgflags(R[3]) : hn_msgflags(0);
    ssize_t n;

    if (fd < 0) {
        hn_errset(r, ROS_EBADF);
        return;
    }
    if (len > HN_MAX_XFER) {
        len = HN_MAX_XFER;
    }
    buf = g_malloc(len ? len : 1);
    if (sending) {
        if (!vmch_guest_rw(R[1], buf, len, false)) {
            r->rc = HN_RC_BADADDR;
            return;
        }
        n = send(fd, (char *)buf, len, fl);
    } else {
        n = recv(fd, (char *)buf, len, fl);
    }
    if (n < 0) {
        if (hn_blocked()) {
            hn_wouldblock(s, r, R[0]);
        } else {
            hn_fail(r);
        }
        return;
    }
    if (!sending && n > 0 && !vmch_guest_rw(R[1], buf, (uint32_t)n, true)) {
        r->rc = HN_RC_BADADDR;
        return;
    }
    hn_ok(r, (int32_t)n);
}

/*
 * readv/writev.  The iovec array is in guest memory and so are the buffers
 * it points at, so both are gathered here into one flat transfer: the host
 * stack has no interest in where the guest chose to keep its pieces, and
 * one send() beats iovcnt of them.
 */
static void hn_rwv(HostNetState *s, uint32_t *R, HNReply *r, bool sending)
{
    int fd = hn_fd(s, R[0]);
    uint32_t iovcnt = R[2];
    g_autofree uint32_t *iov = NULL;
    g_autofree uint8_t *buf = NULL;
    uint32_t total = 0, off = 0, i;
    ssize_t n;

    if (fd < 0) {
        hn_errset(r, ROS_EBADF);
        return;
    }
    if (iovcnt > HN_MAX_IOV) {
        hn_errset(r, ROS_EINVAL);
        return;
    }
    iov = g_malloc0(iovcnt * 8 + 8);          /* {base, len} per entry */
    if (iovcnt && !vmch_guest_rw(R[1], iov, iovcnt * 8, false)) {
        r->rc = HN_RC_BADADDR;
        return;
    }
    for (i = 0; i < iovcnt; i++) {
        total += iov[i * 2 + 1];
    }
    if (total > HN_MAX_XFER) {
        total = HN_MAX_XFER;
    }
    buf = g_malloc(total ? total : 1);

    if (sending) {
        for (i = 0; i < iovcnt && off < total; i++) {
            uint32_t n2 = iov[i * 2 + 1];

            if (n2 > total - off) {
                n2 = total - off;
            }
            if (n2 && !vmch_guest_rw(iov[i * 2], buf + off, n2, false)) {
                r->rc = HN_RC_BADADDR;
                return;
            }
            off += n2;
        }
        n = send(fd, (char *)buf, total, hn_msgflags(0));
    } else {
        n = recv(fd, (char *)buf, total, hn_msgflags(0));
    }
    if (n < 0) {
        if (hn_blocked()) {
            hn_wouldblock(s, r, R[0]);
        } else {
            hn_fail(r);
        }
        return;
    }
    if (!sending) {
        uint32_t left = (uint32_t)n;

        for (i = 0; i < iovcnt && left; i++) {
            uint32_t n2 = iov[i * 2 + 1];

            if (n2 > left) {
                n2 = left;
            }
            if (n2 && !vmch_guest_rw(iov[i * 2], buf + off, n2, true)) {
                r->rc = HN_RC_BADADDR;
                return;
            }
            off += n2;
            left -= n2;
        }
    }
    hn_ok(r, (int32_t)n);
}

static void hn_listen(HostNetState *s, uint32_t *R, HNReply *r)
{
    int fd = hn_fd(s, R[0]);

    if (fd < 0) {
        hn_errset(r, ROS_EBADF);
        return;
    }
    if (listen(fd, (int)R[1]) < 0) {
        hn_fail(r);
        return;
    }
    hn_ok(r, 0);
}

/*
 * Accept, both ABI forms.  The new socket gets its own slot, so the guest
 * sees a descriptor in the same numbering as everything else, and it is
 * non-blocking from the start like every other socket here — inheriting
 * the listener's blocking flag would not help, because the host cannot
 * block either way.
 */
static void hn_accept(HostNetState *s, uint32_t *R, HNReply *r, bool newform)
{
    int fd = hn_fd(s, R[0]);
    struct sockaddr_in sin;
    socklen_t sl = sizeof(sin);
    int nfd, id;

    if (fd < 0) {
        hn_errset(r, ROS_EBADF);
        return;
    }
    memset(&sin, 0, sizeof(sin));
    nfd = qemu_accept(fd, (struct sockaddr *)&sin, &sl);
    if (nfd < 0) {
        if (hn_blocked()) {
            hn_wouldblock(s, r, R[0]);
        } else {
            hn_fail(r);
        }
        return;
    }
    id = hn_alloc(s, nfd);
    if (id < 0) {
        closesocket(nfd);
        hn_errset(r, ROS_EMFILE);
        return;
    }
    hn_set_nonblock(nfd);
    hn_no_sigpipe(nfd);
    hn_sa_out(R[1], R[2], newform, &sin);
    hn_ok(r, id);
}

/*
 * sendmsg / recvmsg, in both ABI forms.
 *
 * The two msghdrs differ at the end, not the beginning: Internet 4's
 * omsghdr is six words ending in access rights, Internet 5's is seven
 * ending in control data and flags (build/h/inet4 and sys/h/socket).  The
 * first four words — name, namelen, iov, iovlen — are the same in both,
 * and they are the only ones that carry data anywhere.  Access rights and
 * control data are dropped: RISC OS has no file descriptors to pass, and
 * the library documentation says the old calls ignore the field on entry
 * anyway.
 */
static void hn_msg(HostNetState *s, uint32_t *R, HNReply *r,
                   bool sending, bool newform)
{
    int fd = hn_fd(s, R[0]);
    uint32_t mh[7];
    uint32_t words = newform ? 7 : 6;
    uint32_t name, namelen, iovp, iovcnt;
    struct sockaddr_in sin;
    socklen_t sl = sizeof(sin);
    g_autofree uint32_t *iov = NULL;
    g_autofree uint8_t *buf = NULL;
    uint32_t total = 0, off = 0, i;
    int fl;
    ssize_t n;

    if (fd < 0) {
        hn_errset(r, ROS_EBADF);
        return;
    }
    memset(mh, 0, sizeof(mh));
    if (!vmch_guest_rw(R[1], mh, words * 4, false)) {
        r->rc = HN_RC_BADADDR;
        return;
    }
    name = mh[0];
    namelen = mh[1];
    iovp = mh[2];
    iovcnt = mh[3];
    fl = hn_msgflags(R[2]);

    if (iovcnt > HN_MAX_IOV) {
        hn_errset(r, ROS_EINVAL);
        return;
    }
    iov = g_malloc0(iovcnt * 8 + 8);
    if (iovcnt && !vmch_guest_rw(iovp, iov, iovcnt * 8, false)) {
        r->rc = HN_RC_BADADDR;
        return;
    }
    for (i = 0; i < iovcnt; i++) {
        total += iov[i * 2 + 1];
    }
    if (total > HN_MAX_XFER) {
        total = HN_MAX_XFER;
    }
    buf = g_malloc(total ? total : 1);
    memset(&sin, 0, sizeof(sin));

    if (sending) {
        for (i = 0; i < iovcnt && off < total; i++) {
            uint32_t n2 = iov[i * 2 + 1];

            if (n2 > total - off) {
                n2 = total - off;
            }
            if (n2 && !vmch_guest_rw(iov[i * 2], buf + off, n2, false)) {
                r->rc = HN_RC_BADADDR;
                return;
            }
            off += n2;
        }
        if (name && namelen) {
            if (!hn_sa_in(name, namelen, &sin)) {
                hn_errset(r, ROS_EINVAL);
                return;
            }
            n = sendto(fd, (char *)buf, total, fl,
                       (struct sockaddr *)&sin, sizeof(sin));
        } else {
            n = send(fd, (char *)buf, total, fl);
        }
    } else {
        n = recvfrom(fd, (char *)buf, total, fl,
                     (struct sockaddr *)&sin, &sl);
    }

    if (n < 0) {
        if (hn_blocked()) {
            hn_wouldblock(s, r, R[0]);
        } else {
            hn_fail(r);
        }
        return;
    }

    if (!sending) {
        uint32_t left = (uint32_t)n;

        for (i = 0; i < iovcnt && left; i++) {
            uint32_t n2 = iov[i * 2 + 1];

            if (n2 > left) {
                n2 = left;
            }
            if (n2 && !vmch_guest_rw(iov[i * 2], buf + off, n2, true)) {
                r->rc = HN_RC_BADADDR;
                return;
            }
            off += n2;
            left -= n2;
        }
        if (name) {
            /* msg_namelen is a word in the header, not a pointer to one,
             * so the length goes back by rewriting that word. */
            hn_sa_out(name, 0, newform, &sin);
            hn_st32(R[1] + 4, 16);
        }
        if (newform) {
            hn_st32(R[1] + 24, 0);        /* msg_flags: nothing to report */
        }
    }
    hn_ok(r, (int32_t)n);
}

static void hn_shutdown(HostNetState *s, uint32_t *R, HNReply *r)
{
    int fd = hn_fd(s, R[0]);

    if (fd < 0) {
        hn_errset(r, ROS_EBADF);
        return;
    }
    if (shutdown(fd, (int)R[1]) < 0) {
        hn_fail(r);
        return;
    }
    hn_ok(r, 0);
}

/* getsockname / getpeername, both ABI forms. */
static void hn_getname(HostNetState *s, uint32_t *R, HNReply *r,
                       bool peer, bool newform)
{
    int fd = hn_fd(s, R[0]);
    struct sockaddr_in sin;
    socklen_t sl = sizeof(sin);
    int rc;

    if (fd < 0) {
        hn_errset(r, ROS_EBADF);
        return;
    }
    memset(&sin, 0, sizeof(sin));
    rc = peer ? getpeername(fd, (struct sockaddr *)&sin, &sl)
              : getsockname(fd, (struct sockaddr *)&sin, &sl);
    if (rc < 0) {
        hn_fail(r);
        return;
    }
    hn_sa_out(R[1], R[2], newform, &sin);
    hn_ok(r, 0);
}

/*
 * Socket options.
 *
 * The numbers agree: Winsock copied BSD, so SO_REUSEADDR is 4 and
 * SO_ERROR is 0x1007 on RISC OS, Windows and macOS alike, and SOL_SOCKET
 * is 0xffff everywhere.  They are still translated by name rather than
 * passed through, because "they happen to match today" is not a contract,
 * and because the ones we do not understand must be *accepted* rather
 * than forwarded: a program that cannot set SO_DEBUG should carry on, not
 * fail.
 */
static bool hn_opt(uint32_t level, uint32_t name, int *hlevel, int *hname)
{
    if (level == 0xffff) {                    /* SOL_SOCKET */
        *hlevel = SOL_SOCKET;
        switch (name) {
        case 0x0004: *hname = SO_REUSEADDR; return true;
        case 0x0008: *hname = SO_KEEPALIVE; return true;
        case 0x0020: *hname = SO_BROADCAST; return true;
        case 0x0080: *hname = SO_LINGER;    return true;
        case 0x0100: *hname = SO_OOBINLINE; return true;
        case 0x1001: *hname = SO_SNDBUF;    return true;
        case 0x1002: *hname = SO_RCVBUF;    return true;
        case 0x1007: *hname = SO_ERROR;     return true;
        case 0x1008: *hname = SO_TYPE;      return true;
        default: return false;
        }
    }
    if (level == 6) {                         /* IPPROTO_TCP */
        *hlevel = IPPROTO_TCP;
        switch (name) {
        case 0x01: *hname = TCP_NODELAY; return true;
        default: return false;
        }
    }
    return false;
}

static void hn_setsockopt(HostNetState *s, uint32_t *R, HNReply *r)
{
    int fd = hn_fd(s, R[0]);
    int hlevel, hname;
    uint8_t val[32];
    uint32_t len = R[4];

    if (fd < 0) {
        hn_errset(r, ROS_EBADF);
        return;
    }
    if (len > sizeof(val)) {
        len = sizeof(val);
    }
    if (len && !vmch_guest_rw(R[3], val, len, false)) {
        r->rc = HN_RC_BADADDR;
        return;
    }
    if (!hn_opt(R[1], R[2], &hlevel, &hname)) {
        hn_ok(r, 0);            /* accepted and ignored, not refused */
        return;
    }
    if (setsockopt(fd, hlevel, hname, (const char *)val, (socklen_t)len) < 0) {
        hn_fail(r);
        return;
    }
    hn_ok(r, 0);
}

static void hn_getsockopt(HostNetState *s, uint32_t *R, HNReply *r)
{
    int fd = hn_fd(s, R[0]);
    int hlevel, hname;
    uint8_t val[32];
    socklen_t sl = sizeof(val);
    uint32_t want = R[4] ? hn_ld32(R[4]) : 4;

    if (fd < 0) {
        hn_errset(r, ROS_EBADF);
        return;
    }
    if (!hn_opt(R[1], R[2], &hlevel, &hname)) {
        hn_errset(r, ROS_ENOPROTOOPT);
        return;
    }
    memset(val, 0, sizeof(val));
    if (getsockopt(fd, hlevel, hname, (char *)val, &sl) < 0) {
        hn_fail(r);
        return;
    }
    /*
     * SO_ERROR is the one that must be translated rather than copied: it
     * hands back a host errno, and the guest is about to read it as a
     * 4.4BSD one.
     */
    if (hlevel == SOL_SOCKET && hname == SO_ERROR && sl >= 4) {
        int e;
        uint32_t ros;

        memcpy(&e, val, 4);
        if (e) {
            int save = errno;
            errno = e;
            ros = hn_errno();
            errno = save;
        } else {
            ros = 0;
        }
        memcpy(val, &ros, 4);
    }
    if (want > sl) {
        want = sl;
    }
    if (want > sizeof(val)) {
        want = sizeof(val);
    }
    if (want && !vmch_guest_rw(R[3], val, want, true)) {
        r->rc = HN_RC_BADADDR;
        return;
    }
    if (R[4]) {
        hn_st32(R[4], want);
    }
    hn_ok(r, 0);
}

static void hn_close(HostNetState *s, uint32_t *R, HNReply *r)
{
    int fd = hn_fd(s, R[0]);

    if (fd < 0) {
        hn_errset(r, ROS_EBADF);
        return;
    }
    closesocket(fd);
    s->fds[R[0]] = -1;
    s->nonblock[R[0]] = false;
    s->async[R[0]] = false;
    s->woke[R[0]] = 0;
    s->connecting[R[0]] = false;
    hn_ok(r, 0);
}

/*
 * Ioctl.  FIONBIO is the one that matters, and it changes nothing on the
 * host — the socket is already non-blocking — it records what the guest
 * believes, which is what decides whether a would-block is an answer or
 * a wait.
 */
static void hn_ioctl(HostNetState *s, uint32_t *R, HNReply *r)
{
    int fd = hn_fd(s, R[0]);

    if (fd < 0) {
        hn_errset(r, ROS_EBADF);
        return;
    }
    switch (R[1]) {
    case HN_FIONBIO:
        s->nonblock[R[0]] = hn_ld32(R[2]) != 0;
        hn_ok(r, 0);
        break;
    case HN_FIONREAD:
        hn_st32(R[2], hn_readable(fd) ? 1 : 0);
        hn_ok(r, 0);
        break;

    /*
     * FIOASYNC: deliver Internet Event 19 when this socket wakes.  The
     * events themselves are Sprint 3, and this is recorded rather than
     * implemented -- but it cannot simply be refused, because the stock
     * Resolver sets it immediately after Socket_Creat and closes the
     * socket if it fails.  Refusing it is why DNS did nothing at all the
     * first time Sprint 1 was tested.
     */
    case HN_FIOASYNC:
        s->async[R[0]] = hn_ld32(R[2]) != 0;
        hn_ok(r, 0);
        break;
    case HN_FIOSETOWN:
        s->owner[R[0]] = hn_ld32(R[2]);
        hn_ok(r, 0);
        break;
    case HN_FIOGETOWN:
        hn_st32(R[2], s->owner[R[0]]);
        hn_ok(r, 0);
        break;
    default:
        hn_errset(r, ROS_EOPNOTSUPP);
        break;
    }
}

/*
 * Select, readability only (Sprint 1).
 *
 * One poll, no waiting: the timeout is the guest's business and the
 * module loops.  The output sets are written back only when something is
 * ready, so a guest that gets zero can ask again with its input sets
 * still intact — which is what makes that loop possible at all.
 */
static void hn_select(HostNetState *s, uint32_t *R, HNReply *r)
{
    uint32_t nd = R[0] > HN_MAX_SOCKETS ? HN_MAX_SOCKETS : R[0];
    uint8_t in[3][HN_FDSET_BYTES], out[3][HN_FDSET_BYTES];
    const uint32_t bit[3] = { HN_R, HN_W, HN_X };
    uint32_t i, k, count = 0;

    memset(in, 0, sizeof(in));
    memset(out, 0, sizeof(out));
    for (k = 0; k < 3; k++) {
        if (R[1 + k] && !vmch_guest_rw(R[1 + k], in[k], HN_FDSET_BYTES,
                                       false)) {
            r->rc = HN_RC_BADADDR;
            return;
        }
    }

    for (i = 0; i < nd; i++) {
        uint32_t want = 0, got;
        int fd;

        for (k = 0; k < 3; k++) {
            if (R[1 + k] && (in[k][i >> 3] & (1u << (i & 7)))) {
                want |= bit[k];
            }
        }
        if (!want) {
            continue;
        }
        fd = hn_fd(s, i);
        if (fd < 0) {
            hn_errset(r, ROS_EBADF);
            return;
        }
        got = hn_ready(fd, want);
        for (k = 0; k < 3; k++) {
            if ((want & bit[k]) && (got & bit[k])) {
                out[k][i >> 3] |= 1u << (i & 7);
                count++;      /* BSD counts each ready bit, not each fd */
            }
        }
    }

    /*
     * Only written back when something is ready.  A zero answer leaves the
     * caller's input sets untouched, which is what lets the module ask
     * again without rebuilding them — and the module has to ask again,
     * because the timeout is the caller's and the host cannot wait.
     */
    if (count) {
        for (k = 0; k < 3; k++) {
            if (R[1 + k]) {
                vmch_guest_rw(R[1 + k], out[k], HN_FDSET_BYTES, true);
            }
        }
    }
    hn_ok(r, (int32_t)count);
}

/*
 * Which sockets have woken.
 *
 * The host has no way to call into the guest, so Internet Event 19 --
 * which is how a RISC OS program learns that a socket has something on it
 * -- has to be raised by the module, and the module has to be told.  It
 * asks on a ticker; this answers.
 *
 * Edge-triggered, and that is the whole design: a readable socket stays
 * readable until somebody reads it, so reporting the level would raise the
 * same event every tick until the guest got round to it.  Only the
 * not-readable to readable transition counts.
 */
/* The reasons in one poll's answer, for the trace: "which socket woke and
 * why" is the whole question when an event-driven program goes quiet. */
static const char *hn_reasons(const uint32_t *list, uint32_t n)
{
    static char buf[128];
    static const char *name[4] = { "?", "async", "urgent", "broken" };
    uint32_t i, off = 0;

    for (i = 0; i < n && off + 24 < sizeof(buf); i++) {
        uint32_t reason = (list[i] >> 8) & 0xFF;

        off += snprintf(buf + off, sizeof(buf) - off, " fd%u=%s:%u",
                        list[i] & 0xFF,
                        name[reason < 4 ? reason : 0], list[i] >> 16);
    }
    buf[off] = 0;
    return buf;
}

static void hn_poll(HostNetState *s, uint64_t base, HNReply *r)
{
    uint32_t list[HN_POLL_MAX];
    uint32_t n = 0;
    int i;

    for (i = 0; i < HN_MAX_SOCKETS; i++) {
        short re;
        bool now;
        uint32_t reason;

        if (s->fds[i] < 0 || !s->async[i]) {
            s->woke[i] = 0;
            continue;
        }
        re = hn_revents(s->fds[i], POLLRDNORM);
        now = (re & (POLLRDNORM | POLLPRI | POLLERR | POLLHUP)) != 0;

        /*
         * Which of the three reasons this is.  The order matters: a socket
         * that has broken is broken whatever else is true of it, and out-of
         * -band data outranks ordinary data — a program watching for urgent
         * data wants to hear about it before it is told there is something
         * to read.
         */
        if (re & (POLLERR | POLLHUP)) {
            reason = HN_EV_BROKEN;
        } else if (re & POLLPRI) {
            reason = HN_EV_URGENT;
        } else {
            reason = HN_EV_ASYNC;
        }

        if (!now) {
            reason = 0;
        }
        if (reason && reason != s->woke[i] && n < HN_POLL_MAX) {
            struct sockaddr_in sin;
            socklen_t sl = sizeof(sin);
            uint32_t port = 0;

            memset(&sin, 0, sizeof(sin));
            if (getsockname(s->fds[i], (struct sockaddr *)&sin, &sl) == 0) {
                port = ntohs(sin.sin_port);
            }
            list[n++] = (port << 16) | (reason << 8) | (uint32_t)i;
        }
        s->woke[i] = (uint8_t)reason;
    }
    if (n) {
        vmch_guest_rw(base + HN_HDR_SIZE, list, n * 4, true);
    }
    {   /* Every poll, not only the useful ones: "the ticker never
         * ran" and "the ticker ran and found nothing" look identical
         * from the guest, and they have completely different causes. */
        static uint32_t polls;
        if (n || polls < 3 || (polls % 200) == 0) {
            hn_trace("hostnet: POLL #%u -> %u ready%s\n", polls, n,
                     n ? hn_reasons(list, n) : "");
        }
        polls++;
    }
    hn_ok(r, (int32_t)n);
}

static void hn_do_swi(HostNetState *s, uint32_t swi, uint32_t *R, HNReply *r)
{
    switch (swi) {
    case HN_SWI_CREAT:      hn_creat(s, R, r); break;
    case HN_SWI_BIND:       hn_bind(s, R, r); break;
    case HN_SWI_SENDTO:     hn_sendto(s, R, r); break;
    case HN_SWI_RECVFROM:   hn_recvfrom(s, R, r, false); break;
    case HN_SWI_RECVFROM_1: hn_recvfrom(s, R, r, true); break;
    case HN_SWI_CLOSE:      hn_close(s, R, r); break;
    case HN_SWI_IOCTL:      hn_ioctl(s, R, r); break;
    case HN_SWI_SELECT:     hn_select(s, R, r); break;

    /* Sprint 2: TCP. */
    case HN_SWI_CONNECT:    hn_connect(s, R, r); break;
    case HN_SWI_SEND:       hn_sendrecv(s, R, r, true,  true);  break;
    case HN_SWI_RECV:       hn_sendrecv(s, R, r, false, true);  break;
    case HN_SWI_WRITE:      hn_sendrecv(s, R, r, true,  false); break;
    case HN_SWI_READ:       hn_sendrecv(s, R, r, false, false); break;
    case HN_SWI_WRITEV:     hn_rwv(s, R, r, true);  break;
    case HN_SWI_READV:      hn_rwv(s, R, r, false); break;
    case HN_SWI_SHUTDOWN:   hn_shutdown(s, R, r); break;
    case HN_SWI_SETSOCKOPT: hn_setsockopt(s, R, r); break;
    case HN_SWI_GETSOCKOPT: hn_getsockopt(s, R, r); break;
    case HN_SWI_GETSOCKNAME:   hn_getname(s, R, r, false, false); break;
    case HN_SWI_GETSOCKNAME_1: hn_getname(s, R, r, false, true);  break;
    case HN_SWI_GETPEERNAME:   hn_getname(s, R, r, true,  false); break;
    case HN_SWI_GETPEERNAME_1: hn_getname(s, R, r, true,  true);  break;

    /* Sprint 3: listeners, and the message calls in both ABI forms. */
    case HN_SWI_LISTEN:     hn_listen(s, R, r); break;
    case HN_SWI_ACCEPT:     hn_accept(s, R, r, false); break;
    case HN_SWI_ACCEPT_1:   hn_accept(s, R, r, true);  break;
    case HN_SWI_SENDMSG:    hn_msg(s, R, r, true,  false); break;
    case HN_SWI_SENDMSG_1:  hn_msg(s, R, r, true,  true);  break;
    case HN_SWI_RECVMSG:    hn_msg(s, R, r, false, false); break;
    case HN_SWI_RECVMSG_1:  hn_msg(s, R, r, false, true);  break;

    case HN_SWI_VERSION:    hn_ok(r, HN_VERSION_VALUE); break;
    case HN_SWI_GETTSIZE:   hn_ok(r, HN_MAX_SOCKETS); break;

    /*
     * !Internet's !Run issues `Sysctl -ew net.inet.udp.checksum=1` under
     * CheckError, so this one has to succeed or the boot stops before
     * Choices:Internet.User and the machine ends up with no resolvers.
     * Nothing else consults the MIB in Sprint 1: a set is accepted and
     * dropped, a get returns nothing.
     */
    case HN_SWI_SYSCTL:
        if (R[3]) {
            hn_st32(R[3], 0);       /* oldlenp = 0: nothing returned */
        }
        hn_ok(r, 0);
        break;

    default:
        hn_errset(r, ROS_EOPNOTSUPP);
        break;
    }
}

/* ---- the doorbell ---------------------------------------------------- */

static void hn_ring(HostNetState *s, uint64_t base)
{
    uint32_t cmd = hn_ld32(base + HN_HDR_CMD);
    uint32_t regs[8];
    HNReply r = { .result = 0, .err = 0, .rc = HN_RC_OK };
    uint32_t swi;

    s->seq = hn_ld32(base + HN_HDR_SEQ);

    if (cmd == HN_CMD_PING) {
        hn_st32(base + HN_HDR_RESULT, HN_MAGIC_VALUE);
        hn_st32(base + HN_HDR_ERRNO, 0);
        hn_st32(base + HN_HDR_RC, HN_RC_OK);
        return;
    }
    if (cmd == HN_CMD_POLL) {
        HNReply pr = { .result = 0, .err = 0, .rc = HN_RC_OK };

        if (s->enabled) {
            hn_poll(s, base, &pr);
        }
        hn_st32(base + HN_HDR_RESULT, (uint32_t)pr.result);
        hn_st32(base + HN_HDR_ERRNO, 0);
        hn_st32(base + HN_HDR_RC, pr.rc);
        return;
    }
    if (cmd != HN_CMD_SWI) {
        hn_st32(base + HN_HDR_RC, HN_RC_BADCMD);
        return;
    }

    swi = hn_ld32(base + HN_HDR_SWI);
    if (swi >= HN_SWI_COUNT) {
        hn_st32(base + HN_HDR_RC, HN_RC_BADSWI);
        return;
    }
    if (!vmch_guest_rw(base + HN_HDR_REGS, regs, sizeof(regs), false)) {
        hn_st32(base + HN_HDR_RC, HN_RC_BADADDR);
        return;
    }
    if (!s->enabled) {
        hn_st32(base + HN_HDR_RC, HN_RC_NOSOCKETS);
        return;
    }

    errno = 0;
    hn_do_swi(s, swi, regs, &r);

    hn_trace("hostnet: %-14s %08x %08x %08x %08x %08x %08x -> %d"
             " errno=%u rc=%u\n",
             hn_swi_name[swi], regs[0], regs[1], regs[2], regs[3],
             regs[4], regs[5], r.result, r.err, r.rc);

    hn_st32(base + HN_HDR_RESULT, (uint32_t)r.result);
    hn_st32(base + HN_HDR_ERRNO, r.err);
    hn_st32(base + HN_HDR_RC, r.rc);
}

/*
 * Switched off, the whole window reads zero — not just the feature bits.
 *
 * This window used to hold an unimplemented device, and its reading as
 * zero was load-bearing rather than incidental: EtherGENET reads the GENET
 * revision register, which is at offset 0, and takes zero to mean an
 * unknown controller it should decline.  Answering 'HNET' there would give
 * it a revision it has never heard of on a machine that has not asked for
 * HostNet at all.
 *
 * The stock CMOS unplugs EtherGENET, so nothing reads it today — but that
 * is luck, not a guarantee, and a machine booted with a different CMOS
 * would find it.  Off should be indistinguishable from before, so it is.
 */
static uint64_t hostnet_read(void *opaque, hwaddr offset, unsigned size)
{
    HostNetState *s = HOSTNET(opaque);

    if (!s->enabled) {
        return 0;
    }
    switch (offset) {
    case HN_MAGIC:
        return HN_MAGIC_VALUE;
    case HN_VERSION:
        return HN_VERSION_VALUE;
    case HN_FEATURES:
        return HN_FEATURE_SOCKETS;
    default:
        return 0;
    }
}

static void hostnet_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    HostNetState *s = HOSTNET(opaque);

    /* Switched off, writes go nowhere: a machine that has not asked for
     * HostNet must not be able to reach it by writing to the window. */
    if (s->enabled && offset == HN_CMD) {
        hn_ring(s, value);
    }
}

static const MemoryRegionOps hostnet_ops = {
    .read = hostnet_read,
    .write = hostnet_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* ---- the device ------------------------------------------------------ */

static void hostnet_reset(DeviceState *dev)
{
    HostNetState *s = HOSTNET(dev);
    int i;

    /*
     * A reset abandons every socket the guest held, so they are closed
     * here rather than leaked: whatever owned them is gone.
     */
    for (i = 0; i < HN_MAX_SOCKETS; i++) {
        if (s->fds[i] >= 0) {
            closesocket(s->fds[i]);
        }
        s->fds[i] = -1;
        s->nonblock[i] = false;
        s->async[i] = false;
        s->woke[i] = 0;
        s->connecting[i] = false;
    }
    s->seq = 0;
}

static void hostnet_realize(DeviceState *dev, Error **errp)
{
    HostNetState *s = HOSTNET(dev);
    int i;

    for (i = 0; i < HN_MAX_SOCKETS; i++) {
        s->fds[i] = -1;
    }
    memory_region_init_io(&s->mr, OBJECT(s), &hostnet_ops, s,
                          "hostnet", HN_REGION_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mr);
}

static const Property hostnet_props[] = {
    /*
     * Off unless asked for, so a machine that has not been switched over
     * still boots its old stack.  The launchers turn it on; see
     * riscos-pi4/tools/rom.py.
     */
    DEFINE_PROP_BOOL("sockets", HostNetState, enabled, false),
};

static const VMStateDescription hostnet_vmstate = {
    .name = "hostnet",
    .version_id = 1,
    .minimum_version_id = 1,
    /*
     * Deliberately not migrating the descriptor table.  A host socket
     * cannot be carried into a saved image, so a -loadvm restore finds
     * every descriptor gone and answers EBADF.  Pretending otherwise
     * would hand the guest numbers that connect to nothing.
     */
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(seq, HostNetState),
        VMSTATE_END_OF_LIST()
    },
};

static void hostnet_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = hostnet_realize;
    device_class_set_legacy_reset(dc, hostnet_reset);
    dc->vmsd = &hostnet_vmstate;
    device_class_set_props(dc, hostnet_props);
}

static const TypeInfo hostnet_type = {
    .name = TYPE_HOSTNET,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(HostNetState),
    .class_init = hostnet_class_init,
};

static void hostnet_register(void)
{
    type_register_static(&hostnet_type);
}

type_init(hostnet_register)
