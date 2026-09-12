/*
 * vmchannel — synchronous doorbell to host services, behind HostFS.
 *
 * The guest writes a request block (see include/hw/misc/vmchannel.h)
 * into its own RAM and stores the block's physical address in CMD.
 * This runs the command right here, inside the MMIO write under the
 * BQL, and puts the response in the same block: there is no queue and
 * no state between requests, which is what makes the device safe to
 * snapshot and impossible to desynchronise.  A slow host filing system
 * stalls the vCPU for the duration, exactly as a slow disc would.
 *
 * Host paths: the guest sends RISC OS paths rooted at '$' with '.'
 * separators; '$' maps to root=, '.' to the host separator.  A path is
 * refused unless every component after that translation is a plain
 * name — no '..', no drive letters, no slashes arriving from the guest.
 * That keeps every path the device touches inside root= by
 * construction.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/misc/vmchannel.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "qemu/error-report.h"
#include <glib/gstdio.h>

/* ------------------------------------------------------------------ */
/* Guest RAM access: physical, little-endian, through the system AS   */

static uint32_t ld32(hwaddr a)
{
    return ldl_le_phys(&address_space_memory, a);
}

static uint8_t ld8(hwaddr a)
{
    uint8_t v = 0;

    address_space_rw(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED,
                     &v, 1, false);
    return v;
}

static void st32(hwaddr a, uint32_t v)
{
    stl_le_phys(&address_space_memory, a, v);
}

static void block_read(hwaddr a, void *buf, uint32_t len)
{
    address_space_rw(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED,
                     buf, len, false);
}

static void block_write(hwaddr a, const void *buf, uint32_t len)
{
    address_space_rw(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED,
                     (void *)buf, len, true);
}

/*
 * Guest *logical* memory.  v1 puts the guest's own addresses on the wire
 * — the ones FileSwitch handed the module — and the host walks the MMU
 * to reach them.  cpu_memory_rw_debug() does the translation with the
 * CPU's current settings and crosses pages itself, which is exactly what
 * QEMU's semihosting uses for SYS_READ and SYS_WRITE (semihosting/
 * uaccess.c).  current_cpu is the vCPU that rang the doorbell: the
 * handler runs synchronously inside its MMIO write, under the BQL.
 *
 * It will not fault a page in — an address that is not mapped right now
 * fails rather than being invented.  That is the point: the v0 module
 * translated addresses itself and fell back to the identity mapping when
 * OS_Memory failed, which wrote file data to whatever physical address
 * the logical one happened to resemble.
 *
 * Returns true on success.
 */
static void vmch_trace(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

/*
 * Diagnostic: cpu_translate_for_debug() reports failure without saying
 * which fault.  This walks the ARMv7 short-descriptor tables by hand,
 * straight out of guest RAM via the TTBR, and prints every level so the
 * trace names the fault itself: bad L1 type, invalid L2 type, domain,
 * or AP.  Printed only on a translate failure, for that address.
 */
static void vmch_dump_walk(CPUState *cs, uint64_t addr)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    uint32_t sctlr = env->cp15.sctlr_ns;
    uint32_t ttbcr = env->cp15.tcr_el[1];
    uint32_t dacr = env->cp15.dacr_ns;
    uint32_t ttbr = env->cp15.ttbr0_el[1];
    uint32_t mode = env->uncached_cpsr & 0x1f;
    uint32_t l1, l2 = 0;
    hwaddr l1addr;
    int domain, domprot;

    vmch_trace("vmch:   walk diag va=%08llx mode=%x sctlr=%08x ttbcr=%08x "
               "dacr=%08x ttbr0=%08x\n",
               (unsigned long long)addr, mode, sctlr, ttbcr, dacr, ttbr);
    if (!(sctlr & 1)) {
        vmch_trace("vmch:   MMU off — translate cannot fail, "
                   "inconsistent\n");
        return;
    }
    if ((ttbcr & 7) && addr >= (1ULL << (ttbcr & 7))) {
        ttbr = env->cp15.ttbr1_el[1];
        vmch_trace("vmch:   using ttbr1=%08x (TTBCR.N=%u)\n",
                   ttbr, ttbcr & 7);
    }
    l1addr = (ttbr & 0xffffc000u) + ((addr >> 20) << 2);
    l1 = ld32(l1addr);
    vmch_trace("vmch:   L1@%08llx = %08x type=%u domain=%u\n",
               (unsigned long long)l1addr, l1, l1 & 3, (l1 >> 5) & 0xf);
    switch (l1 & 3) {
    case 0:
        vmch_trace("vmch:   -> L1 translation fault (unmapped)\n");
        return;
    case 2:
        domprot = (dacr >> (((l1 >> 5) & 0xf) * 2)) & 3;
        vmch_trace("vmch:   -> section, domain prot=%u — fault is "
                   "domain or AP\n", domprot);
        return;
    case 3:
        vmch_trace("vmch:   -> reserved L1 type\n");
        return;
    }
    /* type 1: coarse second-level table, 4k small pages */
    domain = (l1 >> 5) & 0xf;
    domprot = (dacr >> (domain * 2)) & 3;
    l2 = ld32((l1 & 0xfffffc00u) + (((addr >> 12) & 0xff) << 2));
    vmch_trace("vmch:   L2 = %08x type=%u domprot=%u ap=%u "
               "(domain=%d)\n",
               l2, l2 & 3, domprot, ((l2 >> 4) & 3) | ((l2 >> 7) & 4),
               domain);
    switch (l2 & 3) {
    case 0:
        vmch_trace("vmch:   -> L2 translation fault (page not in tables)\n");
        break;
    case 1:
        vmch_trace("vmch:   -> 64k large page\n");
        break;
    default:
        vmch_trace("vmch:   -> 4k small page\n");
        break;
    }
}

static bool guest_rw(uint64_t addr, void *buf, uint32_t len, bool is_write)
{
    CPUState *cpu = current_cpu ? current_cpu : first_cpu;
    uint8_t *p = buf;

    if (len == 0) {
        return true;
    }
    if (!cpu) {
        return false;
    }

    /*
     * Translate a page at a time and move the bytes through
     * address_space_memory, which is what riscos_blitter.c does to read
     * sprites out of guest virtual memory and is proven on this board.
     *
     * cpu_memory_rw_debug() is the obvious call and was tried first, but
     * it routes through cpu->cpu_ases[asidx], not the system address
     * space, and it treats any MEMTX error as a translation failure —
     * both of which this avoids.  cpu_translate_for_debug() itself does
     * no protection checks (see its contract in hw/core/cpu.h), so a
     * page the guest can reach is a page this can reach.
     */
    while (len) {
        TranslateForDebugResult tres;
        uint64_t page = addr & ~(uint64_t)0xFFF;
        uint32_t off = (uint32_t)(addr - page);
        uint32_t n = 0x1000 - off;
        hwaddr plen;
        void *host;

        if (n > len) {
            n = len;
        }
        if (!cpu_translate_for_debug(cpu, addr, &tres)) {
            vmch_trace("vmch: translate failed at va=%08llx\n",
                    (unsigned long long)addr);
            vmch_dump_walk(cpu, addr);
            return false;
        }
        plen = n;
        host = address_space_map(&address_space_memory, tres.physaddr, &plen,
                                 is_write, tres.attrs);
        if (!host || plen == 0) {
            vmch_trace("vmch: map failed va=%08llx pa=%08llx\n",
                    (unsigned long long)addr,
                    (unsigned long long)tres.physaddr);
            if (host) {
                address_space_unmap(&address_space_memory, host, plen,
                                    is_write, 0);
            }
            return false;
        }
        if (plen < n) {
            n = (uint32_t)plen;
        }
        if (is_write) {
            memcpy(host, p, n);
        } else {
            memcpy(p, host, n);
        }
        address_space_unmap(&address_space_memory, host, plen, is_write,
                            is_write ? n : 0);
        p += n;
        addr += n;
        len -= n;
    }
    return true;
}

/*
 * Move as much as will translate, a page at a time, and say how far it
 * got.  A guest buffer is not necessarily reachable end to end — nothing
 * here can fault a page in — so a partly-reachable buffer must report a
 * short count rather than quietly deliver rubbish past the boundary.
 * The chunking is host-side: it is still one doorbell.
 */
static uint32_t guest_rw_counted(uint64_t addr, uint8_t *buf, uint32_t len,
                                 bool is_write)
{
    uint32_t done = 0;

    while (done < len) {
        uint32_t chunk = 0x1000 - (uint32_t)((addr + done) & 0xFFF);

        if (chunk > len - done) {
            chunk = len - done;
        }
        if (!guest_rw(addr + done, buf + done, chunk, is_write)) {
            break;
        }
        done += chunk;
    }
    return done;
}

/* ------------------------------------------------------------------ */
/* Host paths                                                          */

/*
 * A component that names a Windows device (CON, NUL, COM1...) would be
 * redirected to the device even under root/, so they are refused like
 * any other escape attempt.
 */
static bool win32_reserved(const char *name, size_t len)
{
    static const char *dev[] = { "CON", "PRN", "AUX", "NUL",
                                 "COM1", "COM2", "COM3", "COM4", "COM5",
                                 "COM6", "COM7", "COM8", "COM9",
                                 "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
                                 "LPT6", "LPT7", "LPT8", "LPT9", NULL };
    size_t stem = 0;

    while (stem < len && name[stem] != '.') {
        stem++;
    }
    for (int i = 0; dev[i]; i++) {
        if (stem == strlen(dev[i])
            && g_ascii_strncasecmp(name, dev[i], stem) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * Translate the guest's RISC OS path into a host path under root and
 * validate it.  Returns a newly allocated host path, or NULL with *rc
 * set.  The guest path must start with '$' and consist of plain
 * components separated by '.'.
 */
static char *host_path(VMChannelState *s, const char *guest, int *rc)
{
    size_t len = strlen(guest);
    char *p, *out, *end;

    if (!s->root) {
        *rc = VMCH_RC_NOROOT;
        return NULL;
    }
    if (len < 1 || guest[0] != '$' || len > VMCH_MAX_ARG) {
        *rc = VMCH_RC_BADPATH;
        return NULL;
    }

    /*
     * The translated path is never longer than the guest path ('.' maps
     * to one separator, '$' to the root), so root + separator + len + 1
     * is a true upper bound.  The first version of this wrote the
     * components past an allocation of root+separator and corrupted the
     * heap on the first real OPEN/CAT/FILEARGS.
     */
    p = g_malloc(strlen(s->root) + 1 + len + 1);
    strcpy(p, s->root);
    out = p + strlen(s->root);
    end = p + strlen(s->root) + 1 + len;
    if (guest[1]) {
        *out++ = G_DIR_SEPARATOR;
    }

    /* walk the rest: each component until '.' must be a plain name */
    const char *c = guest + 1;
    while (*c == '.') {
        c++;                            /* leading dots are separators */
    }
    bool fresh = true;                  /* true: at component start */
    char *comp = out;                   /* start of the current component */
    for (; *c; c++) {
        if (*c == '.') {
            if (fresh || win32_reserved(comp, out - comp)) {
                g_free(p);              /* "..", empty, or a device name */
                *rc = VMCH_RC_BADPATH;
                return NULL;
            }
            *out++ = G_DIR_SEPARATOR;
            fresh = true;
        } else if (*c == '/' || *c == '\\' || *c == ':') {
            g_free(p);                  /* host separators from the guest */
            *rc = VMCH_RC_BADPATH;
            return NULL;
        } else {
            if (fresh) {
                comp = out;             /* remember where this name began */
                fresh = false;
            }
            *out++ = *c;
        }
        g_assert(out <= end);
    }
    if (!fresh && win32_reserved(comp, out - comp)) {
        g_free(p);
        *rc = VMCH_RC_BADPATH;
        return NULL;
    }
    if (fresh && out > p + strlen(s->root) + 1) {
        g_free(p);                      /* trailing '.' after a component */
        *rc = VMCH_RC_BADPATH;
        return NULL;
    }
    /* Bare "$" (out at root) and "$." (out at root + separator) both
     * name the root itself; strip a lone trailing separator so the host
     * sees the directory, not "dir/". */
    if (out > p + strlen(s->root) && out[-1] == G_DIR_SEPARATOR) {
        out--;
    }
    *out = '\0';
    return p;
}

/* Fetch the arg text (NUL-terminated) from the block. */
static char *arg_text(hwaddr base, uint32_t arglen)
{
    char *p;

    if (arglen == 0 || arglen > VMCH_MAX_ARG) {
        return NULL;
    }
    p = g_malloc0(arglen + 1);
    block_read(base + VMCH_HDR_SIZE, p, arglen);
    p[arglen] = '\0';
    return p;
}

/* ------------------------------------------------------------------ */
/* Host file plumbing (glib handles UTF-8 -> wide chars on Windows)   */

/*
 * Where the device's own files (trace, console log) land.  The Windows
 * dev box has its F: scratch; the Mac's properly-launched app runs
 * with cwd=/, so its home is ~/Library/Application Support/RISCOSQEMU
 * (the same configured directory the scripting surface writes into,
 * SCRIPTING.md section 7); anywhere else, /tmp.  VMCH_TRACE names the
 * trace file outright for a dev loop that wants it elsewhere.
 */
static char *vmch_dir(void)
{
#ifdef _WIN32
    return g_strdup("F:/RISCOSDEV/.scratch");
#elif defined(__APPLE__)
    const char *home = g_get_home_dir();

    if (!home) {
        return g_strdup("/tmp");
    }
    return g_strdup_printf("%s/Library/Application Support/RISCOSQEMU",
                           home);
#else
    return g_strdup("/tmp");
#endif
}

static int alloc_handle(VMChannelState *s)
{
    for (int i = 0; i < VMCH_MAX_OPEN; i++) {
        if (s->fds[i] == -1) {
            return i;
        }
    }
    return -1;
}

static int riscos_type_for(const char *name, const GStatBuf *st)
{
    const char *comma = strrchr(name, ',');

    if (S_ISDIR(st->st_mode)) {
        return 0;                       /* 0 = directory in RISC OS terms */
    }
    if (comma) {
        char *end;
        long t = strtol(comma + 1, &end, 16);
        if (end != comma + 1 && *end == '\0' && t >= 0 && t < 0x1000) {
            return (int)t;              /* "name,ff8" style suffix */
        }
    }
    return 0xFFF;
}

static uint32_t attrs_for(const GStatBuf *st)
{
    uint32_t a = 3;                     /* owner read + write */

    if (!(st->st_mode & S_IWUSR)) {
        a &= ~(uint32_t)2;
    }
    return a;
}

static uint64_t date_cs_for(const GStatBuf *st)
{
    /* centiseconds since 1900, the RISC OS 5-byte instant: must stay
     * 64-bit, the value is ~3.9e11 for 2026 dates */
    return (uint64_t)(st->st_mtime + 2208988800ULL) * 100;
}

/* Development trace to a file: stderr proved unreliable under the
 * launchers, so every doorbell access lands here instead. */
static void vmch_trace(const char *fmt, ...)
{
    static FILE *f;
    va_list ap;

    if (!f) {
        const char *env = getenv("VMCH_TRACE");
        char *dir = vmch_dir();
        char *path;

        g_mkdir_with_parents(dir, 0755);
        path = g_strdup_printf("%s%cvmch-trace.txt", dir,
                               G_DIR_SEPARATOR);
        g_free(dir);
        f = (env && *env) ? fopen(env, "a") : NULL;
        if (!f) {
            f = fopen(path, "a");
        }
        g_free(path);
        if (!f) {
            static bool warned;
            if (!warned) {
                warned = true;
                fprintf(stderr, "vmch: cannot open trace file\n");
            }
            return;
        }
    }
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);
}

/* ------------------------------------------------------------------ */
/* Command execution.  base is the request block's guest address.      */

static void vmchannel_do(VMChannelState *s, hwaddr base)
{
    uint32_t cmd = ld32(base + VMCH_HDR_CMD);
    uint32_t arglen = ld32(base + VMCH_HDR_ARGLEN);
    uint32_t rc = VMCH_RC_OK;

    st32(base + VMCH_HDR_RC, VMCH_RC_BADCMD);   /* default: overwritten */

    switch (cmd) {
    case VMCH_CMD_PING: {
        /* Echo: arg bytes are read and written back, and the scratch
         * word at +32 carries the magic so the smoke test can prove
         * both DMA directions without depending on the arg. */
        uint8_t buf[VMCH_MAX_ARG];
        if (arglen > VMCH_MAX_ARG) {
            rc = VMCH_RC_BADPATH;
            break;
        }
        if (arglen) {
            block_read(base + VMCH_HDR_SIZE, buf, arglen);
            block_write(base + VMCH_HDR_SIZE, buf, arglen);
        }
        st32(base + VMCH_HDR_ARG, VMCH_MAGIC_VALUE);
        rc = VMCH_RC_OK;
        break;
    }

    case VMCH_CMD_OPEN: {
        char *path = arg_text(base, arglen);
        char *hp;
        int flags = (int)ld32(base + VMCH_HDR_HANDLE);
        int h, oflags = 0;

        if (!path) {
            rc = VMCH_RC_BADPATH;
            break;
        }
        hp = host_path(s, path, &rc);
        if (!hp) {
            g_free(path);
            break;
        }
        h = alloc_handle(s);
        if (h < 0) {
            rc = VMCH_RC_HANDLES;
            g_free(hp);
            g_free(path);
            break;
        }
        if (flags & VMCH_OPEN_WRITE) {
            oflags |= O_WRONLY | O_BINARY;
            if (flags & VMCH_OPEN_READ) {
                oflags = O_RDWR | O_BINARY;
            }
            if (flags & VMCH_OPEN_CREATE) {
                oflags |= O_CREAT;
            }
            if (flags & VMCH_OPEN_TRUNC) {
                oflags |= O_TRUNC;
            }
        } else {
            oflags = O_RDONLY | O_BINARY;
        }
        s->fds[h] = g_open(hp, oflags, 0644);
        if (s->fds[h] < 0) {
            rc = (errno == ENOENT) ? VMCH_RC_NOTFOUND : VMCH_RC_ACCESS;
        } else {
            /* handles are 1-based over the wire: 0 means "no file" to
             * a RISC OS FSEntry_Open caller */
            st32(base + VMCH_HDR_HANDLE, (uint32_t)(h + 1));
        }
        g_free(hp);
        g_free(path);
        break;
    }

    case VMCH_CMD_SETSIZE: {
        int h = (int)ld32(base + VMCH_HDR_HANDLE) - 1;
        uint32_t ne = ld32(base + VMCH_HDR_ARG + 0);
        int r = -1;

        if (h < 0 || h >= VMCH_MAX_OPEN || s->fds[h] == -1) {
            rc = VMCH_RC_ACCESS;
            break;
        }
#ifdef _WIN32
        r = _chsize_s(s->fds[h], (long long)ne);
#else
        r = ftruncate(s->fds[h], (off_t)ne);
#endif
        if (r != 0) {
            rc = VMCH_RC_IOERR;
        }
        break;
    }

    case VMCH_CMD_CLOSE: {
        int h = (int)ld32(base + VMCH_HDR_HANDLE) - 1;

        if (h < 0 || h >= VMCH_MAX_OPEN || s->fds[h] == -1) {
            rc = VMCH_RC_ACCESS;
        } else {
            close(s->fds[h]);
            s->fds[h] = -1;
        }
        break;
    }

    case VMCH_CMD_READ:
    case VMCH_CMD_WRITE: {
        int h = (int)ld32(base + VMCH_HDR_HANDLE) - 1;
        hwaddr bufaddr = (hwaddr)(uint32_t)ld32(base + VMCH_HDR_ARG + 0);
        uint32_t len = ld32(base + VMCH_HDR_ARG + 4);
        void *tmp;
        ssize_t n = -1;

        if (h < 0 || h >= VMCH_MAX_OPEN || s->fds[h] == -1) {
            rc = VMCH_RC_ACCESS;
            break;
        }
        if (len > 16 * 1024 * 1024) {
            rc = VMCH_RC_IOERR;
            break;
        }
        tmp = g_malloc(len ? len : 1);
        if (cmd == VMCH_CMD_READ) {
            n = read(s->fds[h], tmp, len);
            if (n > 0) {
                block_write(bufaddr, tmp, (uint32_t)n);
            }
        } else {
            block_read(bufaddr, tmp, len);
            n = write(s->fds[h], tmp, len);
        }
        g_free(tmp);
        if (n < 0) {
            rc = VMCH_RC_IOERR;
        } else {
            st32(base + VMCH_HDR_HANDLE, (uint32_t)n);
        }
        break;
    }

    case VMCH_CMD_SEEK: {
        int h = (int)ld32(base + VMCH_HDR_HANDLE) - 1;
        int64_t off = (int64_t)(int32_t)ld32(base + VMCH_HDR_ARG + 0);
        int whence = (int)ld32(base + VMCH_HDR_ARG + 4);
        static const int w[] = { SEEK_SET, SEEK_CUR, SEEK_END };
        off_t r;

        if (h < 0 || h >= VMCH_MAX_OPEN || s->fds[h] == -1 ||
            whence < 0 || whence > 2) {
            rc = VMCH_RC_ACCESS;
            break;
        }
        r = lseek(s->fds[h], (off_t)off, w[whence]);
        if (r < 0) {
            rc = VMCH_RC_IOERR;
        } else {
            st32(base + VMCH_HDR_HANDLE, (uint32_t)r);
        }
        break;
    }

    case VMCH_CMD_FILEARGS: {
        char *path = arg_text(base, arglen);
        char *hp;
        GStatBuf st;
        uint8_t resp[20];

        if (!path) {
            rc = VMCH_RC_BADPATH;
            break;
        }
        hp = host_path(s, path, &rc);
        if (!hp) {
            g_free(path);
            break;
        }
        if (g_stat(hp, &st) != 0) {
            rc = (errno == ENOENT) ? VMCH_RC_NOTFOUND : VMCH_RC_ACCESS;
        } else {
            uint32_t size = (uint32_t)st.st_size;
            uint32_t type = S_ISDIR(st.st_mode) ? 0
                           : (uint32_t)riscos_type_for(path, &st);
            uint64_t cs = date_cs_for(&st);

            stl_le_p(resp + 0, size);
            stl_le_p(resp + 4, type);
            stl_le_p(resp + 8, attrs_for(&st));
            stl_le_p(resp + 12, (uint32_t)cs);
            stl_le_p(resp + 16, (uint32_t)(cs >> 32));
            block_write(base + VMCH_HDR_SIZE, resp, sizeof(resp));
            st32(base + VMCH_HDR_ARGLEN, sizeof(resp));
        }
        g_free(hp);
        g_free(path);
        break;
    }

    case VMCH_CMD_CAT: {
        char *path = arg_text(base, arglen);
        char *hp;
        GDir *dir;
        const char *name;
        GError *gerr = NULL;
        uint8_t *resp = g_malloc(VMCH_MAX_ARG);
        uint32_t used = 0, limit = VMCH_MAX_ARG;

        if (!path) {
            rc = VMCH_RC_BADPATH;
            break;
        }
        hp = host_path(s, path, &rc);
        if (!hp) {
            g_free(path);
            g_free(resp);
            break;
        }
        dir = g_dir_open(hp, 0, &gerr);
        if (!dir) {
            rc = (errno == ENOENT || gerr && gerr->code == G_FILE_ERROR_NOENT)
                 ? VMCH_RC_NOTFOUND : VMCH_RC_NOTDIR;
            if (gerr) {
                g_error_free(gerr);
            }
        } else {
            while ((name = g_dir_read_name(dir)) != NULL) {
                char *one = g_build_filename(hp, name, NULL);
                GStatBuf st;
                uint32_t type = 0xFFF, size = 0, attrs = 3;

                if (g_stat(one, &st) == 0) {
                    type = S_ISDIR(st.st_mode) ? 0
                          : (uint32_t)riscos_type_for(name, &st);
                    size = (uint32_t)st.st_size;
                    attrs = attrs_for(&st);
                }
                g_free(one);
                if (strlen(name) >= VMCH_MAX_NAME) {
                    continue;
                }
                if (used + 64 > limit) {
                    rc = VMCH_RC_FULL;      /* more entries than fit */
                    break;
                }
                memset(resp + used, 0, 64);
                memcpy(resp + used, name, strlen(name));
                stl_le_p(resp + used + 48, type);
                stl_le_p(resp + used + 52, size);
                stl_le_p(resp + used + 56, attrs);
                used += 64;
            }
            g_dir_close(dir);
            if (used) {
                block_write(base + VMCH_HDR_SIZE, resp, used);
            }
            st32(base + VMCH_HDR_ARGLEN, used);
        }
        g_free(hp);
        g_free(path);
        g_free(resp);
        break;
    }

    case VMCH_CMD_CREATE: {
        char *path = arg_text(base, arglen);
        char *hp;
        int fd;

        if (!path) {
            rc = VMCH_RC_BADPATH;
            break;
        }
        hp = host_path(s, path, &rc);
        if (!hp) {
            g_free(path);
            break;
        }
        fd = g_open(hp, O_WRONLY | O_CREAT | O_BINARY | O_TRUNC, 0644);
        if (fd < 0) {
            rc = (errno == ENOENT) ? VMCH_RC_NOTFOUND : VMCH_RC_ACCESS;
        } else {
            close(fd);
        }
        g_free(hp);
        g_free(path);
        break;
    }

    case VMCH_CMD_DELETE: {
        char *path = arg_text(base, arglen);
        char *hp;
        GStatBuf st;

        if (!path) {
            rc = VMCH_RC_BADPATH;
            break;
        }
        hp = host_path(s, path, &rc);
        if (!hp) {
            g_free(path);
            break;
        }
        if (g_stat(hp, &st) != 0) {
            rc = VMCH_RC_NOTFOUND;
        } else if (S_ISDIR(st.st_mode)) {
            rc = g_rmdir(hp) == 0 ? VMCH_RC_OK : VMCH_RC_NOTDIR;
        } else {
            rc = g_unlink(hp) == 0 ? VMCH_RC_OK : VMCH_RC_ACCESS;
        }
        g_free(hp);
        g_free(path);
        break;
    }

    case VMCH_CMD_RENAME: {
        char *both = arg_text(base, arglen);
        char *split, *hp1, *hp2;

        if (!both) {
            rc = VMCH_RC_BADPATH;
            break;
        }
        split = strchr(both, '\n');
        if (!split) {
            rc = VMCH_RC_BADPATH;
        } else {
            *split = '\0';
            hp1 = host_path(s, both, &rc);
            if (hp1) {
                hp2 = host_path(s, split + 1, &rc);
                if (hp2) {
                    rc = g_rename(hp1, hp2) == 0 ? VMCH_RC_OK
                                                       : VMCH_RC_ACCESS;
                    g_free(hp2);
                }
                g_free(hp1);
            }
        }
        g_free(both);
        break;
    }

    case VMCH_CMD_CONSOLE: {
        uint8_t *buf;

        if (arglen > VMCH_MAX_ARG) {     /* bound check BEFORE the
                                          * allocation: arglen is a raw
                                          * guest value up to 4 GiB */
            rc = VMCH_RC_BADPATH;
            break;
        }
        buf = g_malloc(arglen ? arglen : 1);
        block_read(base + VMCH_HDR_SIZE, buf, arglen);
        /*
         * A log file in the device's home directory: stderr is no use,
         * the launchers send it to DEVNULL, and a bare name would be
         * lost where the Mac app's cwd is /.
         */
        {
            char *dir = vmch_dir();
            char *path;
            FILE *log;

            g_mkdir_with_parents(dir, 0755);
            path = g_strdup_printf("%s%cvmchannel-console.txt", dir,
                                   G_DIR_SEPARATOR);
            g_free(dir);
            log = fopen(path, "ab");
            g_free(path);
            if (log) {
                fwrite(buf, 1, arglen, log);
                fclose(log);
            }
        }
        g_free(buf);
        break;
    }

    case VMCH_CMD_TIME: {
        uint8_t resp[8];
        uint64_t cs = (uint64_t)(time(NULL) + 2208988800ULL) * 100;

        stl_le_p(resp + 0, (uint32_t)cs);
        stl_le_p(resp + 4, (uint32_t)(cs >> 32));
        block_write(base + VMCH_HDR_SIZE, resp, sizeof(resp));
        st32(base + VMCH_HDR_ARGLEN, sizeof(resp));
        break;
    }

    case VMCH_CMD_FS_GETBYTES:
    case VMCH_CMD_FS_PUTBYTES: {
        /* FSEntry_GetBytes / _PutBytes on a buffered file, whole, in one
         * round trip: R1 handle, R2 guest logical buffer, R3 count, R4
         * file offset.  No exit registers (PRM 2-544).  The offset rides
         * in the request, so there is no seek and no per-page chunking —
         * the v0 module cost two doorbells per 4 KiB for want of both. */
        bool is_put = (cmd == VMCH_CMD_FS_PUTBYTES);
        uint32_t r1 = ld32(base + VMCH_HDR_REGS + 4);
        uint64_t addr = (uint64_t)ld32(base + VMCH_HDR_REGS + 8);
        uint32_t len = ld32(base + VMCH_HDR_REGS + 12);
        uint64_t off = (uint64_t)ld32(base + VMCH_HDR_REGS + 16);
        int h = (int)r1 - 1;
        g_autofree uint8_t *tmp = NULL;
        ssize_t n;

        if (!s->root) {
            rc = VMCH_RC_NOROOT;
            break;
        }
        if (h < 0 || h >= VMCH_MAX_OPEN || s->fds[h] == -1) {
            rc = VMCH_RC_ACCESS;
            break;
        }
        if (len > 16 * 1024 * 1024) {
            rc = VMCH_RC_IOERR;
            break;
        }
        tmp = g_malloc(len ? len : 1);

        if (is_put) {
            uint32_t got = guest_rw_counted(addr, tmp, len, false);

            if (got != len) {
                st32(base + VMCH_HDR_REGS + 12, got);
                rc = VMCH_RC_BADADDR;
                break;
            }
            n = pwrite(s->fds[h], tmp, len, (off_t)off);
            if (n < 0) {
                rc = VMCH_RC_IOERR;
            } else if ((uint32_t)n != len) {
                rc = VMCH_RC_FULL;      /* short write: out of space */
            }
        } else {
            n = pread(s->fds[h], tmp, len, (off_t)off);
            if (n < 0) {
                rc = VMCH_RC_IOERR;
                break;
            }
            /* A short read is the normal end of a file: the count is a
             * multiple of the buffer size, so the last block runs past
             * the extent.  Zero the tail rather than leave the guest's
             * buffer holding whatever was there before. */
            if ((uint32_t)n < len) {
                memset(tmp + n, 0, len - (uint32_t)n);
            }
            {
                uint32_t put = guest_rw_counted(addr, tmp, len, true);

                st32(base + VMCH_HDR_REGS + 12, put);
                if (put != len) {
                    rc = VMCH_RC_BADADDR;
                }
            }
        }
        break;
    }

    default:
        rc = VMCH_RC_BADCMD;
        break;
    }

    /* Development trace: every doorbell request, one line, with the
     * first block words so layout disputes can be settled from the
     * log alone. */
    {
        vmch_trace("vmch: cmd=%u seq=%u rc=%u hnd=%08x arglen=%u",
                cmd, ld32(base + VMCH_HDR_SEQ), rc,
                ld32(base + VMCH_HDR_HANDLE), arglen);
        if (cmd >= 0x100) {
            vmch_trace(" R1=%08x R2=%08x R3=%08x R4=%08x",
                    ld32(base + VMCH_HDR_REGS + 4),
                    ld32(base + VMCH_HDR_REGS + 8),
                    ld32(base + VMCH_HDR_REGS + 12),
                    ld32(base + VMCH_HDR_REGS + 16));
        }
        if (cmd == VMCH_CMD_OPEN || cmd == VMCH_CMD_CREATE ||
            cmd == VMCH_CMD_DELETE || cmd == VMCH_CMD_CAT ||
            cmd == VMCH_CMD_FILEARGS) {
            uint32_t i;
            vmch_trace(" path=");
            for (i = 0; i < arglen && i < 64; i++) {
                int c = ld8(base + VMCH_HDR_SIZE + i);
                vmch_trace("%c", (c >= 32 && c < 127) ? c : '.');
            }
        }
        vmch_trace("\n");
    }

    st32(base + VMCH_HDR_RC, rc);
}

/* ------------------------------------------------------------------ */
/* MMIO                                                                */

static uint64_t vmchannel_read_inner(VMChannelState *s, hwaddr offset,
                                     unsigned size);

static uint64_t vmchannel_read(void *opaque, hwaddr offset, unsigned size)
{
    VMChannelState *s = VMCHANNEL(opaque);
    uint64_t v = vmchannel_read_inner(s, offset, size);

    vmch_trace("vmch RD off=%llx sz=%u -> %08llx\n",
            (unsigned long long)offset, size, (unsigned long long)v);
    return v;
}

static uint64_t vmchannel_read_inner(VMChannelState *s, hwaddr offset,
                                     unsigned size)
{

    switch (offset) {
    case VMCH_MAGIC:
        return VMCH_MAGIC_VALUE;
    case VMCH_VERSION:
        return VMCH_VERSION_VALUE;
    case VMCH_FEATURES:
        return VMCH_FEATURE_CONSOLE | VMCH_FEATURE_TIME
             | VMCH_FEATURE_FSENTRY | VMCH_FEATURE_VIRTADDR
             | (s->root ? VMCH_FEATURE_FS : 0);
    case VMCH_STATUS:
        return 1;                       /* synchronous: always done */
    default:
        return 0;
    }
}

static void vmchannel_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    VMChannelState *s = VMCHANNEL(opaque);

    vmch_trace("vmch WR off=%llx sz=%u val=%08llx\n",
            (unsigned long long)offset, size, (unsigned long long)value);

    if (offset == VMCH_CMD && size == 4) {
        vmchannel_do(s, (hwaddr)(uint32_t)value & ~(hwaddr)0xf);
    }
    /* every other register is read-only */
}

static const MemoryRegionOps vmchannel_ops = {
    .read = vmchannel_read,
    .write = vmchannel_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void vmchannel_realize(DeviceState *dev, Error **errp)
{
    VMChannelState *s = VMCHANNEL(dev);

    if (s->root && !g_file_test(s->root, G_FILE_TEST_IS_DIR)) {
        error_setg(errp, "vmchannel: root is not a directory: %s", s->root);
        return;
    }
    for (int i = 0; i < VMCH_MAX_OPEN; i++) {
        s->fds[i] = -1;
    }
    memory_region_init_io(&s->mr, OBJECT(s), &vmchannel_ops, s,
                          "vmchannel", VMCH_REGION_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mr);
}

static const Property vmchannel_props[] = {
    DEFINE_PROP_STRING("root", VMChannelState, root),
};

static void vmchannel_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = vmchannel_realize;
    device_class_set_props(dc, vmchannel_props);
    /* No vmstate: registers only, nothing persists between requests. */
    dc->vmsd = NULL;
    dc->desc = "HostFS doorbell (see riscos-pi4/FSDESIGN.md)";
    dc->user_creatable = false;
}

static const TypeInfo vmchannel_type = {
    .name = TYPE_VMCHANNEL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VMChannelState),
    .class_init = vmchannel_class_init,
};

static void vmchannel_register(void)
{
    type_register_static(&vmchannel_type);
}

type_init(vmchannel_register);
