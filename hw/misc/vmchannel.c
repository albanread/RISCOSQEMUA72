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
#include <utime.h>
#ifndef _WIN32
#include <sys/statvfs.h>
#include <sys/xattr.h>
#endif

/*
 * errno as it stood when the answer was decided, not when the trace line
 * is written.  Later calls in the same request set errno too, so reading
 * it at the end reported a different failure from the one that produced
 * the answer -- a CAT refused for permissions was logged as errno=2, from
 * a lookup that ran afterwards (ROS_PRIVATE#9).
 */
static int vmch_errno;

static inline uint32_t vmch_answer(uint32_t code)
{
    if (code != VMCH_RC_OK && code != VMCH_RC_NOTFOUND) {
        vmch_errno = errno;
    }
    return code;
}

/*
 * Set when host_name_of() had to remap a character this host cannot store
 * (ROS_PRIVATE issue #6): the trace flags such a request, and the first one
 * warns.  It replaces the EINVAL-gated check of #9, because the names that
 * matter most produce no errno at all -- an untyped NUL becomes the null
 * device and a trailing dot or space is silently dropped, both without a
 * failure -- and this fires exactly when the escaping did something.
 */
static bool vmch_remapped;

/*
 * mingw has no pread/pwrite.  The doorbell runs each op synchronously
 * under the BQL, one at a time, so seek-transfer-seek-back is race-free
 * where the POSIX names would be the natural spelling.
 */
#ifdef _WIN32
static ssize_t vmch_pread(int fd, void *buf, size_t len, off_t off)
{
    off_t save = lseek(fd, 0, SEEK_CUR);
    ssize_t n = -1;

    if (save >= 0 && lseek(fd, off, SEEK_SET) >= 0) {
        n = read(fd, buf, (unsigned int)len);
        lseek(fd, save, SEEK_SET);
    }
    return n;
}

static ssize_t vmch_pwrite(int fd, const void *buf, size_t len, off_t off)
{
    off_t save = lseek(fd, 0, SEEK_CUR);
    ssize_t n = -1;

    if (save >= 0 && lseek(fd, off, SEEK_SET) >= 0) {
        n = write(fd, buf, (unsigned int)len);
        lseek(fd, save, SEEK_SET);
    }
    return n;
}

#define pread(fd, buf, len, off)  vmch_pread((fd), (buf), (len), (off))
#define pwrite(fd, buf, len, off) vmch_pwrite((fd), (buf), (len), (off))
#endif

/* The most of a GetBytes or PutBytes the host buffers at once; a longer
 * transfer goes through in pieces of this size, still one doorbell. */
#define VMCH_XFER_PIECE (16u * 1024 * 1024)

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

bool vmch_guest_rw(uint64_t addr, void *buf, uint32_t len, bool is_write)
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
        if (!vmch_guest_rw(addr + done, buf + done, chunk, is_write)) {
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
 * Names cross between two character sets.  RISC OS filenames are 8-bit
 * Acorn Latin-1; host names are UTF-8, and macOS refuses a name that is not
 * (mkdir fails with EILSEQ), as glib on Windows cannot convert one.  So
 * every guest name is translated on the way in and every host name on the
 * way out, as well as the two filename conventions every RISC OS filing
 * system on a foreign disc keeps:
 *
 *   RISC OS  host
 *   /        .     a dot is a separator in RISC OS
 *   &A0      space a RISC OS name cannot hold a space, so a hard space
 *                  stands in for one ("Beginners Guide Wimp" on the DDE
 *                  card is spelled with them)
 *   &80-&9F  the Acorn additions: € Ŵ ŵ Ŷ ŷ … ™ ‰ • ‘ ’ ‹ › “ ” „ – — − Œ œ
 *                  † ‡ ﬁ ﬂ (Fonts/Encodings/Latin1); the five ornaments and
 *                  two unassigned codes keep their C1 code points, so they
 *                  still round trip
 *   &A1-&FF  U+00A1-U+00FF, which is ISO 8859-1
 *
 * A host name holding what Latin-1 cannot — CJK, emoji — is shown with '_'
 * in its place.  It can still be opened: lookups compare the guest's name
 * with each host name mapped the same way, so the mapping need not be
 * reversible, only consistent.  Host names are compared in NFC, the form a
 * Mac may or may not have used when it created them.
 */
static const gunichar acorn_latin1_80[32] = {
    0x20AC, 0x0174, 0x0175, 0x0083, 0x0084, 0x0176, 0x0177, 0x0087,
    0x0088, 0x0089, 0x008A, 0x008B, 0x2026, 0x2122, 0x2030, 0x2022,
    0x2018, 0x2019, 0x2039, 0x203A, 0x201C, 0x201D, 0x201E, 0x2013,
    0x2014, 0x2212, 0x0152, 0x0153, 0x2020, 0x2021, 0xFB01, 0xFB02,
};

/*
 * A leafname the guest can hold but a host filing system cannot store,
 * mapped so both hosts can (ROS_PRIVATE issue #6).  Windows forbids
 * < > " | ? * in a name, a trailing '.' or ' ', and the device names
 * (NUL, CON, COM1...); macOS forbids none of these, so a real card tree
 * copies on a Mac and stops partway on Windows -- StrongED ships a
 * directory called TRUE>>>1, and '>' is one Windows refuses.
 *
 * Each such character becomes 0xF000 + c in the Unicode private-use area,
 * the WSL/Cygwin spelling of the convention every SMB stack uses for this.
 * The private-use area is the right home because a code point there cannot
 * occur in an 8-bit Acorn Latin-1 name, so the map is whole and reversible
 * with no escape character (undone in guest_name_of by the same range).
 *
 * Two of these are worse than a refusal, both silent and both measured on
 * NTFS: an untyped NUL opens as the null device and swallows the file, and
 * a trailing '.' or ' ' is dropped -- so RISC OS foo/ (host foo. after the
 * swap) and foo collide into one host file.  So the map is unconditional,
 * not gated on the host complaining.  A typed reserved name (NUL,ff9) is an
 * ordinary file on the host and need not be escaped, but host_name_of does
 * not know the type here and escaping it anyway only costs a placeholder
 * glyph in a file manager, so it is escaped like the rest.
 *
 * Applied on the leaf, after the dot/slash swap; the guest never sees it.
 * '/' \\ ':' never reach here -- the swap consumes '/', host_path refuses
 * the other two -- and a control byte cannot cross the wire.
 */
static char *escape_host_leaf(const char *ideal)
{
    size_t len = strlen(ideal);
    size_t i = 0, end = len;
    size_t comma_suffix = (size_t)-1;
    gunichar trail = 0;
    GString *out = g_string_sized_new(len + 8);

    if (end > 0 && (ideal[end - 1] == '.' || ideal[end - 1] == ' ')) {
        trail = (unsigned char)ideal[end - 1];      /* Windows drops these */
        end--;
    }
    /*
     * A leaf that itself ends in `,` and three hex digits collides with the
     * host's typed-file convention: guest_leaf_of() reads `a,ffb` back as
     * `a` of type &FFB, so a file the guest actually named "a,ffb" could be
     * written but never reopened by that name (issue #17).  Escape that one
     * comma into the private-use area like any other unstorable character;
     * guest_name_of() restores it, and the real type suffix host_leaf_for()
     * appends afterwards is untouched (it is added past this point).
     */
    if (end >= 4 && ideal[end - 4] == ','
        && g_ascii_isxdigit(ideal[end - 3])
        && g_ascii_isxdigit(ideal[end - 2])
        && g_ascii_isxdigit(ideal[end - 1])) {
        comma_suffix = end - 4;
    }
    if (win32_reserved(ideal, end)) {
        g_string_append_unichar(out, 0xF000 + (unsigned char)ideal[0]);
        i = 1;                                       /* break the device name */
    }
    for (; i < end; i++) {
        unsigned char c = (unsigned char)ideal[i];

        if (i == comma_suffix
            || c == '<' || c == '>' || c == '"' || c == '|'
            || c == '?' || c == '*') {
            g_string_append_unichar(out, 0xF000 + c);
        } else {
            g_string_append_c(out, (char)c);         /* ASCII or UTF-8 byte */
        }
    }
    if (trail) {
        g_string_append_unichar(out, 0xF000 + trail);
    }
    if (out->len != len || memcmp(out->str, ideal, len) != 0) {
        vmch_remapped = true;
    }
    return g_string_free(out, FALSE);
}

/* A RISC OS leafname, as the host spells it. */
static char *host_name_of(const char *guest)
{
    GString *out = g_string_sized_new(strlen(guest) + 8);
    g_autofree char *ideal = NULL;

    for (const unsigned char *p = (const unsigned char *)guest; *p; p++) {
        if (*p == '/') {
            g_string_append_c(out, '.');
        } else if (*p == 0xA0) {
            g_string_append_c(out, ' ');
        } else if (*p < 0x80) {
            g_string_append_c(out, (char)*p);
        } else {
            g_string_append_unichar(out, *p < 0xA0 ? acorn_latin1_80[*p - 0x80]
                                                   : (gunichar)*p);
        }
    }
    ideal = g_string_free(out, FALSE);
    return escape_host_leaf(ideal);
}

/* The first n bytes of a host leafname, as RISC OS spells them. */
static char *guest_name_of(const char *host, size_t n)
{
    g_autofree char *raw = g_strndup(host, n);
    g_autofree char *nfc = g_utf8_validate(raw, -1, NULL)
                           ? g_utf8_normalize(raw, -1, G_NORMALIZE_NFC) : NULL;
    const char *p = nfc ? nfc : raw;
    GString *out = g_string_sized_new(n);

    while (*p) {
        gunichar u;
        unsigned char b = '_';

        if (!nfc) {                     /* not UTF-8 at all: byte by byte */
            u = (unsigned char)*p++;
            if (u >= 0x80) {
                u = 0xFFFD;
            }
        } else {
            u = g_utf8_get_char(p);
            p = g_utf8_next_char(p);
        }
        if (u >= 0xF000 && u <= 0xF0FF) {
            u -= 0xF000;                /* undo escape_host_leaf (issue #6) */
        }
        if (u == '.') {
            b = '/';
        } else if (u == ' ' || u == 0xA0) {
            b = 0xA0;
        } else if (u > ' ' && u < 0x7F) {
            b = (unsigned char)u;
        } else if (u >= 0xA1 && u <= 0xFF) {
            b = (unsigned char)u;
        } else {
            for (int k = 0; k < 32; k++) {
                if (acorn_latin1_80[k] == u) {
                    b = (unsigned char)(0x80 + k);
                    break;
                }
            }
        }
        g_string_append_c(out, (char)b);
    }
    return g_string_free(out, FALSE);
}

/*
 * Translate the guest's RISC OS path into a host path under root and
 * validate it.  Returns a newly allocated host path, or NULL with *rc
 * set.  The guest path must start with '$' and consist of plain
 * components separated by '.'.
 */
static char *guest_leaf_of(const char *host_leaf, bool is_dir,
                           uint32_t *type);

/*
 * The real host leafname inside `dir` that RISC OS would call `want`.
 *
 * The fast path is one stat: `readme/txt` is `readme.txt` on the host, so
 * translating slashes back to dots usually hits.  Only when that misses
 * does it read the directory and forward-map every entry, which is what
 * finds `notes` inside `notes,ffb` — the suffix is consumed by the
 * presented name, so it cannot be recovered by rewriting the wanted one.
 * That scan is also where case-insensitivity happens: RISC OS is
 * case-blind, and a case-sensitive host would otherwise hide files.
 *
 * Returns an allocated host path.  A name that matches nothing comes back
 * as the literal translation, so CREATE can make it.
 */
static char *resolve_leaf(const char *dir, const char *want)
{
    char *naive = host_name_of(want);
    char *cand;
    GStatBuf st;
    GDir *d;
    const char *name;
    char *found = NULL;

    cand = g_build_filename(dir, naive, NULL);
    if (g_stat(cand, &st) == 0) {
        g_free(naive);
        return cand;
    }
    g_free(cand);

    d = g_dir_open(dir, 0, NULL);
    if (d) {
        while ((name = g_dir_read_name(d)) != NULL) {
            char *one = g_build_filename(dir, name, NULL);
            GStatBuf est;
            bool isdir = g_stat(one, &est) == 0 && S_ISDIR(est.st_mode);
            uint32_t type;
            char *leaf = guest_leaf_of(name, isdir, &type);

            if (g_ascii_strcasecmp(leaf, want) == 0) {
                g_free(leaf);
                found = one;
                break;
            }
            g_free(leaf);
            g_free(one);
        }
        g_dir_close(d);
    }
    if (found) {
        g_free(naive);
        return found;
    }
    cand = g_build_filename(dir, naive, NULL);   /* not there: for CREATE */
    g_free(naive);
    return cand;
}

/*
 * True if `path` resolves to somewhere inside `root`.  Symlinks are
 * resolved by realpath.  A path whose final component does not exist yet --
 * what a create, mkdir or rename target looks like -- has no realpath of
 * its own, so the deepest ancestor that does exist is checked instead: a
 * parent directory that is a symlink out of the share is then caught, which
 * a realpath of the final object alone missed (issue #20).  `root` itself
 * always exists, so the walk upward terminates.
 */
/*
 * realpath() for the containment check, which has to be written twice.
 *
 * Windows has no realpath().  The shim in scope maps it to _fullpath(),
 * and that returns NULL here even for a directory that plainly exists --
 * so within_root() refused everything and HostFS answered "bad path" to
 * every request, including the share root.  _fullpath() would not be
 * enough even when it does work: it only tidies the string, and does not
 * resolve a junction or a symlink.  Resolving those is the whole point of
 * the check (issue #20), so the Windows side has to be real.
 *
 * GetFinalPathNameByHandleW resolves both, and needs an open handle, so
 * the object must exist; a leaf that does not is already covered by the
 * caller stepping up to its parent.  FILE_FLAG_BACKUP_SEMANTICS is what
 * lets a directory be opened at all.  The "\\?\" prefix it returns is
 * left in place: both sides of the comparison come through here, so they
 * carry it alike.
 */
static char *vmch_realpath(const char *path)
{
#ifdef _WIN32
    wchar_t buf[32768];
    wchar_t *wide = g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
    HANDLE h;
    DWORD n;

    if (!wide) {
        return NULL;
    }
    h = CreateFileW(wide, 0,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    g_free(wide);
    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    n = GetFinalPathNameByHandleW(h, buf, G_N_ELEMENTS(buf),
                                  FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (n == 0 || n >= G_N_ELEMENTS(buf)) {
        return NULL;
    }
    return g_utf16_to_utf8(buf, -1, NULL, NULL, NULL);
#else
    char *r = realpath(path, NULL);
    char *out;

    if (!r) {
        return NULL;
    }
    out = g_strdup(r);            /* one allocator for both hosts */
    free(r);
    return out;
#endif
}

static bool within_root(const char *path, const char *root)
{
    char *rootreal = vmch_realpath(root);
    char *cur = g_strdup(path);
    bool ok = false;

    if (rootreal) {
        size_t rl = strlen(rootreal);

        for (;;) {
            char *real = vmch_realpath(cur);
            char *slash;

            if (real) {
                ok = strncmp(real, rootreal, rl) == 0
                     && (real[rl] == '\0' || real[rl] == G_DIR_SEPARATOR);
                g_free(real);
                break;
            }
            /* Not there: step up and vouch for the parent instead. */
            slash = strrchr(cur, G_DIR_SEPARATOR);
            if (!slash || slash == cur) {
                break;              /* nothing left to check: refuse */
            }
            *slash = '\0';
        }
    }
    g_free(rootreal);
    g_free(cur);
    return ok;
}

static char *host_path(VMChannelState *s, const char *guest, int *rc)
{
    size_t len = strlen(guest);
    char *cur;
    const char *c;

    if (!s->root) {
        *rc = vmch_answer(VMCH_RC_NOROOT);
        return NULL;
    }

    /*
     * Once FSEntry_Func 11 answers with a disc name, FileSwitch puts that
     * name into every canonical path: ":HostFS.$.oldname", not
     * "$.oldname".  Doc/SimpleFS says so — paths arrive with "disc name
     * ... and path from $" — but it only starts happening when the disc
     * name call works, so fixing Func 11 broke every path until this
     * accepted the prefix.  There is one disc, so the name is skipped; with
     * several shares it is what would choose between them.  A RISC OS disc
     * name cannot contain a dot, so the first dot ends it.
     */
    if (len > 0 && guest[0] == ':') {
        const char *dot = strchr(guest, '.');

        if (!dot) {
            *rc = vmch_answer(VMCH_RC_BADPATH);
            return NULL;
        }
        guest = dot + 1;
        len = strlen(guest);
    }

    if (len < 1 || guest[0] != '$' || len > VMCH_MAX_ARG) {
        *rc = vmch_answer(VMCH_RC_BADPATH);
        return NULL;
    }

    cur = g_strdup(s->root);
    c = guest + 1;
    while (*c == '.') {
        c++;                            /* leading dots are separators */
    }

    while (*c) {
        const char *e = c;
        size_t n;
        char *comp, *next;

        while (*e && *e != '.') {
            /* '/' is legal here: it is how RISC OS spells a dot in a
             * foreign leafname.  Host separators are not. */
            if (*e == '\\' || *e == ':') {
                g_free(cur);
                *rc = vmch_answer(VMCH_RC_BADPATH);
                return NULL;
            }
            e++;
        }
        n = (size_t)(e - c);
        if (n == 0) {
            g_free(cur);                /* ".." or an empty component */
            *rc = vmch_answer(VMCH_RC_BADPATH);
            return NULL;
        }
        /* A device name is no longer refused: host_name_of (via
         * resolve_leaf) escapes it, like any name the host could not hold,
         * so it is stored rather than turned away (issue #6). */
        comp = g_strndup(c, n);
        next = resolve_leaf(cur, comp);
        g_free(comp);
        g_free(cur);
        cur = next;

        c = e;
        while (*c == '.') {
            c++;
        }
    }

    /*
     * Whatever the components said, the answer must still be inside the
     * share.  within_root() resolves symlinks and, for a leaf that does not
     * exist yet (a create, mkdir or rename target), checks the deepest
     * ancestor that does -- so a parent directory that is a symlink out of
     * the share is refused before anything is written through it, not only
     * a symlink that is read through (issue #20).  The share root is the
     * only security perimeter, so create, mkdir and rename must honour it
     * just as OPEN and OS_File 10 already did.
     */
    if (!within_root(cur, s->root)) {
        g_free(cur);
        *rc = vmch_answer(VMCH_RC_BADPATH);
        return NULL;
    }
    return cur;
}

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

/*
 * Extension -> filetype.  The table is data: VMCH_TYPEMAP names a file of
 * "ext<TAB>type<TAB>name<TAB>description" lines, generated from ROOL's
 * allocation list by riscos-pi4/tools/mktypemap.py.  The handful below is
 * only a fallback for when no file is given.
 *
 * No entry may map to an executable type (&FFA Module, &FF8 Absolute,
 * &FFC Utility, &FEB Obey, &FFB BASIC, &FFE Command): an extension is a
 * guess, and a guess must not tell the desktop that a host file is code.
 * Only an explicit ,xxx suffix may say that.  The generator enforces the
 * same rule.
 */
static const struct { const char *ext; uint32_t type; } typemap_builtin[] = {
    { "txt", 0xFFF }, { "c", 0xFFF }, { "h", 0xFFF }, { "s", 0xFFF },
    { "cpp", 0xFFF }, { "py", 0xFFF }, { "md", 0xFFF }, { "json", 0xFFF },
    { "log", 0xFFF }, { "csv", 0xFFF }, { "xml", 0xF80 }, { "html", 0xFAF },
    { "htm", 0xFAF }, { "png", 0xB60 }, { "jpg", 0xC85 }, { "jpeg", 0xC85 },
    { "gif", 0x695 }, { "pdf", 0xADF }, { "zip", 0xA91 }, { "gz", 0xF89 },
    { "tar", 0xC46 }, { "wav", 0xFB1 }, { "mod", 0xCB6 },
};

static GHashTable *typemap;             /* lowercased ext -> type + 1 */
static GHashTable *typemap_rev;         /* type + 1 -> canonical ext */

static void typemap_load(void)
{
    const char *file = getenv("VMCH_TYPEMAP");
    size_t i;

    if (typemap) {
        return;
    }
    typemap = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    /* The reverse direction is one-to-many — &FFF alone owns txt, c, h,
     * py and a dozen more — so the FIRST entry for a type wins and
     * becomes the extension used when RISC OS writes a file of that type.
     * Reorder the typemap file to change it. */
    typemap_rev = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                        NULL, g_free);
    for (i = 0; i < ARRAY_SIZE(typemap_builtin); i++) {
        g_hash_table_insert(typemap, g_strdup(typemap_builtin[i].ext),
                            GUINT_TO_POINTER(typemap_builtin[i].type + 1));
        if (!g_hash_table_contains(typemap_rev,
                GUINT_TO_POINTER(typemap_builtin[i].type + 1))) {
            g_hash_table_insert(typemap_rev,
                                GUINT_TO_POINTER(typemap_builtin[i].type + 1),
                                g_strdup(typemap_builtin[i].ext));
        }
    }
    if (file && *file) {
        char *text = NULL;
        if (g_file_get_contents(file, &text, NULL, NULL)) {
            char **lines = g_strsplit(text, "\n", -1);
            unsigned n = 0;
            for (i = 0; lines[i]; i++) {
                char **f;
                unsigned long t;
                char *endp;
                if (lines[i][0] == '#' || lines[i][0] == '\0') {
                    continue;
                }
                f = g_strsplit(lines[i], "\t", -1);
                if (f[0] && f[1]) {
                    t = strtoul(f[1], &endp, 16);
                    if (*endp == '\0' && t < 0x1000) {
                        g_hash_table_insert(typemap, g_ascii_strdown(f[0], -1),
                                            GUINT_TO_POINTER((guint)t + 1));
                        if (!g_hash_table_contains(typemap_rev,
                                GUINT_TO_POINTER((guint)t + 1))) {
                            g_hash_table_insert(typemap_rev,
                                    GUINT_TO_POINTER((guint)t + 1),
                                    g_ascii_strdown(f[0], -1));
                        }
                        n++;
                    }
                }
                g_strfreev(f);
            }
            g_strfreev(lines);
            g_free(text);
            vmch_trace("vmch: typemap %s: %u entries\n", file, n);
        } else {
            warn_report("vmchannel: cannot read VMCH_TYPEMAP file %s", file);
        }
    }
}

static uint32_t type_for_ext(const char *ext)
{
    char *low;
    gpointer v;

    if (!ext || !*ext) {
        return 0xFFF;
    }
    typemap_load();
    low = g_ascii_strdown(ext, -1);
    v = g_hash_table_lookup(typemap, low);
    g_free(low);
    return v ? (uint32_t)(GPOINTER_TO_UINT(v) - 1) : 0xFFF;
}

/*
 * A host leafname as RISC OS should see it, and its filetype.
 *
 *   notes,ffb   -> "notes",      &FFB   the RISC OS typed-file convention;
 *                                       the suffix is consumed, not shown
 *   readme.txt  -> "readme/txt", &FFF   DOSFS's convention: a dot in a
 *                                       foreign name becomes a slash, which
 *                                       is what makes it nameable at all
 *   hello       -> "hello",      &FFF
 *
 * Returns a newly allocated leafname; *type gets the filetype, or 0 for a
 * directory.
 */
static char *guest_leaf_of(const char *host_leaf, bool is_dir, uint32_t *type)
{
    const char *comma = strrchr(host_leaf, ',');
    const char *dot;
    char *out;

    if (comma && strlen(comma + 1) == 3) {
        char *end;
        long t = strtol(comma + 1, &end, 16);

        if (end == comma + 4 && t >= 0 && t < 0x1000) {
            *type = is_dir ? 0 : (uint32_t)t;
            /* The suffix is consumed, but what is left can still hold a
             * dot — `readme.txt,ff9` — and a dot is still unrepresentable.
             * Returning it untranslated listed the file as "readme.txt",
             * visible and unopenable: the sprint-3 bug again, for every
             * typed name with a dot in it. */
            return guest_name_of(host_leaf, (size_t)(comma - host_leaf));
        }
    }

    out = guest_name_of(host_leaf, strlen(host_leaf));
    if (is_dir) {
        *type = 0;
        return out;
    }
    dot = strrchr(host_leaf, '.');
    *type = (dot && dot != host_leaf) ? type_for_ext(dot + 1) : 0xFFF;
    return out;
}

/*
 * One catalogue entry, collected so a whole directory can be sorted before
 * it is paged to the guest (VMCH_CMD_CAT).  The name is the guest leaf --
 * what the user sees and what FileCore would sort by.
 */
typedef struct {
    char    *leaf;
    uint32_t type, size, attrs, load, exec;
} caten;

static void caten_free(gpointer p)
{
    caten *e = p;

    g_free(e->leaf);
    g_free(e);
}

/* Case-insensitive on the guest leafname, the order FileCore keeps and a
 * deterministic function of the directory, so the guest's page index means
 * the same on every CAT call for it.
 *
 * FileCore folds to UPPER case, not lower: the characters between 'Z' (&5A)
 * and 'a' (&61) -- '[ \ ] ^ _ `' -- sort after the letters, not before, so a
 * name like `_under` lists last.  g_ascii_strcasecmp folds to lower, which
 * ordered it first and disagreed with a real disc (issue #21); fold each
 * byte to upper by hand to match. */
static gint caten_cmp(gconstpointer a, gconstpointer b)
{
    const caten *x = *(const caten * const *)a;
    const caten *y = *(const caten * const *)b;
    const unsigned char *p = (const unsigned char *)x->leaf;
    const unsigned char *q = (const unsigned char *)y->leaf;

    for (; *p && *q; p++, q++) {
        int cp = g_ascii_toupper(*p);
        int cq = g_ascii_toupper(*q);

        if (cp != cq) {
            return cp - cq;
        }
    }
    return (int)*p - (int)*q;
}

/* A host leafname without its `,xxx` type suffix, if it has one. */
static char *host_base_of(const char *host_leaf)
{
    const char *comma = strrchr(host_leaf, ',');

    if (comma && strlen(comma + 1) == 3 && g_ascii_isxdigit(comma[1])
        && g_ascii_isxdigit(comma[2]) && g_ascii_isxdigit(comma[3])) {
        return g_strndup(host_leaf, (size_t)(comma - host_leaf));
    }
    return g_strdup(host_leaf);
}

/*
 * The host filename for a file of a given type, from its host-form base name
 * (no type suffix; host_base_of() makes one) — the `naming=smart` policy of
 * FSDESIGN-V1 Sec 6.3.  Every caller has a host path already, which is why
 * this takes the host's spelling rather than the guest's.
 *
 *   "hello/c",  &FFF  ->  hello.c       the slash was a dot all along
 *   "notes",    &FFF  ->  notes         Text is the default: nothing to say
 *   "shot/png", &B60  ->  shot.png      the extension already says PNG
 *   "shot",     &B60  ->  shot,b60      nothing says PNG, so the suffix does
 *   "logo",     &FF9  ->  logo,ff9
 *   "data/png", &FFF  ->  data.png,fff  the extension says the wrong thing
 *
 * A type is only encoded when it has to be.  Text gets no decoration at
 * all, because most files are text and a share full of ",fff" would be
 * unusable from the Mac side; and a name that already carries the right
 * extension is left to speak for itself, so a file copied in as
 * `hello.c` copies out as `hello.c` rather than `hello.c,fff`.
 */
static char *host_leaf_for(const char *host_base, uint32_t type, bool is_dir)
{
    const char *dot = strrchr(host_base, '.');
    uint32_t implied = (dot && dot != host_base) ? type_for_ext(dot + 1)
                                                 : 0xFFF;

    /*
     * A directory carries no type, so its host name is its base name and
     * nothing more.  This used to be spotted as `type == 0`, but &000 is a
     * real, allocated filetype, not "no type": a file of type &000 was
     * written bare and read back as the default &FFF (issue #13).  Only an
     * actual directory -- told apart by its own flag now -- skips the suffix
     * on account of being typeless.
     */
    if (is_dir) {
        return g_strdup(host_base);
    }

    /*
     * The name is only decorated when its own spelling would say otherwise,
     * and then only with `,xxx`: whatever this returns must read back, via
     * guest_leaf_of(), as the same name with the same type.
     *
     * It used to append the table's extension instead, so a RISC OS file
     * "SDIM0019" of type JPEG was created as SDIM0019.jpg — which reads back
     * as "SDIM0019/jpg".  *Copy creates a file and then opens it by the name
     * it asked for, so every copied file with a type in the table failed
     * with "file not found".
     */
    if (type == implied) {
        return g_strdup(host_base);     /* the name already says it */
    }
    return g_strdup_printf("%s,%03x", host_base, type);
}

/* The type a RISC OS load word carries, or 0xFFFFFFFF if the word is a
 * real load address rather than a type stamp (PRM 2-542: a typed file has
 * 0xFFF in the top twelve bits). */
static uint32_t type_of_load(uint32_t load)
{
    return ((load >> 20) == 0xFFF) ? ((load >> 8) & 0xFFF) : 0xFFFFFFFFu;
}

/* The RISC OS load/exec pair for a typed, dated file (PRM 2-542): the
 * type and the top byte of the 5-byte instant in load, the rest in exec.
 * Computed here so the module never has to know what a load address is —
 * its own version special-cased &FFF to all-ones, which is why every
 * file in a *Ex listing was untyped and undated. */
static void load_exec_for(uint32_t type, uint64_t cs,
                          uint32_t *load, uint32_t *exec)
{
    *load = 0xFFF00000u | ((type & 0xFFFu) << 8) | (uint32_t)((cs >> 32) & 0xFF);
    *exec = (uint32_t)cs;
}

static int riscos_type_for(const char *name, const GStatBuf *st)
{
    uint32_t type;
    char *leaf = guest_leaf_of(name, S_ISDIR(st->st_mode), &type);

    g_free(leaf);
    return (int)type;
}

/*
 * The RISC OS attribute byte, stored whole for anything the host mode
 * cannot express -- the locked bit especially (issue #12; FSDESIGN-V1 §7).
 * The design's portable default is a .riscos-meta sidecar (§6.3/§13); this
 * uses an extended attribute instead -- the documented cheaper alternative,
 * native on the Mac farm and carried across rename() and unlink() by the
 * host for free -- kept behind these helpers so the store can be swapped in
 * one place.  Windows, which has no getxattr, falls back to the mode.
 *
 * Only the bits chmod cannot hold need it, so an object with default
 * attributes (and every file copied in from the host) has no xattr and its
 * attributes are derived from the mode.
 */
#define VMCH_ATTR_NAME_LINUX "user.riscos.attr"

static bool meta_get_attr(const char *hp, uint32_t *attr)
{
    uint8_t b;
    ssize_t n = -1;

#if defined(__APPLE__)
    n = getxattr(hp, "riscos.attr", &b, 1, 0, 0);
#elif !defined(_WIN32)
    n = getxattr(hp, VMCH_ATTR_NAME_LINUX, &b, 1);
#endif
    if (n == 1) {
        *attr = b;
        return true;
    }
    return false;
}

static void meta_set_attr(const char *hp, uint32_t attr)
{
    uint8_t b = (uint8_t)(attr & 0x3F);

#if defined(__APPLE__)
    (void)setxattr(hp, "riscos.attr", &b, 1, 0, 0);
#elif !defined(_WIN32)
    (void)setxattr(hp, VMCH_ATTR_NAME_LINUX, &b, 1, 0);
#else
    (void)hp; (void)b;
#endif
}

/* Locked (bit 3) is never derivable from the mode, so a file with no stored
 * attribute byte is not locked. */
static bool meta_is_locked(const char *hp)
{
    uint32_t a;

    return meta_get_attr(hp, &a) && (a & 8);
}

static uint32_t attrs_for(const GStatBuf *st, const char *hp)
{
    uint32_t a;

    /*
     * A stored byte is authoritative: it round-trips every bit RISC OS set,
     * including the locked and public-access bits the host mode drops
     * (issue #12 -- read used to be hard-coded on and everything else
     * lost).  With none stored, derive what the mode does carry.
     */
    if (hp && meta_get_attr(hp, &a)) {
        return a;
    }
    a = 0;
    if (st->st_mode & S_IRUSR) {
        a |= 0x01;                      /* owner read */
    }
    if (st->st_mode & S_IWUSR) {
        a |= 0x02;                      /* owner write */
    }
    /*
     * The public-access and locked bits default off for a file with no
     * stored byte -- RISC OS's own default for a fresh file (&03), and what
     * this reported before.  A file given public access or a lock has a
     * stored byte, taken above, that says so.  (Not derived from the host's
     * group/other mode bits, which g_open leaves set at 0644 and which RISC
     * OS never meant.)
     */
    return a;
}

/* True if `hp` is one of the files the guest currently has open: its
 * resolved path matches an open slot's.  Used to refuse a delete, rename or
 * second write-open of an open object (issue #19). */
static bool path_is_open(VMChannelState *s, const char *hp, bool write_only)
{
    char *real = realpath(hp, NULL);
    bool open = false;
    int i;

    if (!real) {
        return false;                   /* cannot exist, so cannot be open */
    }
    for (i = 0; i < VMCH_MAX_OPEN; i++) {
        if (s->open_paths[i] && strcmp(s->open_paths[i], real) == 0
            && (!write_only || s->open_write[i])) {
            open = true;
            break;
        }
    }
    free(real);
    return open;
}

static uint64_t date_cs_for(const GStatBuf *st)
{
    /*
     * Centiseconds since 1900, the RISC OS 5-byte instant: must stay
     * 64-bit, the value is ~3.9e11 for 2026 dates.
     *
     * UTC, as the host's mtime is, with no offset either way.  A RISC OS
     * 5-byte time is UTC (PRM: Territory_ConvertDateAndTime "converts a 5
     * byte UTC time"); the system clock is UTC and FileSwitch stamps files
     * from it; the Territory applies the time zone when a date is shown.
     *
     * This used to add the host's UTC offset, treating a stamp as local
     * wall-clock time, which read right on a desktop whose clock was
     * nobody's and whose zone was unset.  Once HostFS 2.03 set the clock
     * from the host (VMCH_CMD_TIME, UTC) it was an hour wrong each way in
     * summer time: an object cc wrote at 12:23 UTC reached the host as
     * 12:23 BST, and amu saw it as older than a source edited since.
     *
     * st_mtime is signed and can be negative -- a date between 1900 and
     * 1970, which RISC OS keeps (issue #14).  The offset must be added as
     * signed, or a negative mtime casts to a huge unsigned value; the sum
     * is >= 0 for any instant at or after 1900, so it is safe to make it
     * unsigned then.  Before 1900 is unrepresentable, so clamp to 1900.
     */
    int64_t secs = (int64_t)st->st_mtime + 2208988800LL;

    if (secs < 0) {
        secs = 0;
    }
    return (uint64_t)secs * 100;
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

/* A name this host could not store was remapped into the private-use area
 * by host_name_of (issue #6).  Reported when the mapping actually fired --
 * not on EINVAL, which the two worst cases (an untyped NUL, a trailing dot
 * or space) never raise -- so it catches all of them and no false ones.
 * The path shows the true RISC OS name; the point is that its host spelling
 * differs.  Warned once a run: a *Copy meets such a name for a whole subtree. */
static void vmch_name_remapped(hwaddr base, uint32_t arglen)
{
    static bool warned;
    char name[VMCH_MAX_ARG + 1];
    uint32_t i, n = 0;

    vmch_trace(" [name remapped for this host]");
    if (warned) {
        return;
    }
    warned = true;
    for (i = 0; i < arglen && n < sizeof(name) - 1; i++) {
        name[n++] = (char)ld8(base + VMCH_HDR_SIZE + i);
    }
    name[n] = '\0';
    warn_report("vmchannel: '%s' holds a name this host cannot store "
                "(< > \" | ? *, a trailing '.' or space, or a device name); "
                "it is kept in the Unicode private-use area, so a file "
                "manager shows placeholder glyphs but RISC OS sees the real "
                "name. macOS stores all of these as-is.", name);
}

/* ------------------------------------------------------------------ */
/* Command execution.  base is the request block's guest address.      */

static void vmchannel_do(VMChannelState *s, hwaddr base)
{
    gint64 started = g_get_monotonic_time();    /* for the trace */

    /* Cleared so the trace's errno= means "what the host last refused
     * during THIS request", not a leftover from an earlier one.  libc
     * only ever sets errno, never clears it on success. */
    errno = 0;
    vmch_errno = 0;
    vmch_remapped = false;
    uint32_t cmd = ld32(base + VMCH_HDR_CMD);
    uint32_t arglen = ld32(base + VMCH_HDR_ARGLEN);
    uint32_t rc = vmch_answer(VMCH_RC_OK);

    st32(base + VMCH_HDR_RC, VMCH_RC_BADCMD);   /* default: overwritten */

    switch (cmd) {
    case VMCH_CMD_PING: {
        /* Echo: arg bytes are read and written back, and the scratch
         * word at +32 carries the magic so the smoke test can prove
         * both DMA directions without depending on the arg. */
        uint8_t buf[VMCH_MAX_ARG];
        if (arglen > VMCH_MAX_ARG) {
            rc = vmch_answer(VMCH_RC_BADPATH);
            break;
        }
        if (arglen) {
            block_read(base + VMCH_HDR_SIZE, buf, arglen);
            block_write(base + VMCH_HDR_SIZE, buf, arglen);
        }
        st32(base + VMCH_HDR_ARG, VMCH_MAGIC_VALUE);
        rc = vmch_answer(VMCH_RC_OK);
        break;
    }

    case VMCH_CMD_OPEN: {
        char *path = arg_text(base, arglen);
        char *hp;
        int flags = (int)ld32(base + VMCH_HDR_HANDLE);
        int h, oflags = 0;

        if (!path) {
            rc = vmch_answer(VMCH_RC_BADPATH);
            break;
        }
        hp = host_path(s, path, &rc);
        if (!hp) {
            g_free(path);
            break;
        }
        h = alloc_handle(s);
        if (h < 0) {
            rc = vmch_answer(VMCH_RC_HANDLES);
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
        {
            bool want_write = (flags & VMCH_OPEN_WRITE) != 0;

            /* FileCore refuses to open a locked file for update or output
             * (issue #12).  It also refuses a conflicting open of one
             * already open (issue #19): a write-open (OPENUP/OPENOUT)
             * conflicts with any open handle; a read-open (OPENIN)
             * conflicts only with a handle open for update, so two readers
             * are fine but OPENUP-then-OPENIN is not.  A new file, not yet
             * on disc, is neither locked nor open, so an OPENOUT that
             * creates still goes through. */
            if (want_write && meta_is_locked(hp)) {
                rc = vmch_answer(VMCH_RC_LOCKED);
                g_free(hp);
                g_free(path);
                break;
            }
            if (path_is_open(s, hp, !want_write)) {
                rc = vmch_answer(VMCH_RC_OPEN);
                g_free(hp);
                g_free(path);
                break;
            }
            s->fds[h] = g_open(hp, oflags, 0644);
            if (s->fds[h] < 0) {
                rc = vmch_answer((errno == ENOENT) ? VMCH_RC_NOTFOUND
                                                   : VMCH_RC_ACCESS);
            } else {
                /* Remember the resolved path and the mode so a delete,
                 * rename or conflicting open can be refused while it is
                 * open (issue #19). */
                char *real = realpath(hp, NULL);

                s->open_paths[h] = g_strdup(real ? real : hp);
                s->open_write[h] = want_write;
                free(real);
                /* handles are 1-based over the wire: 0 means "no file" to
                 * a RISC OS FSEntry_Open caller */
                st32(base + VMCH_HDR_HANDLE, (uint32_t)(h + 1));
            }
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
            rc = vmch_answer(VMCH_RC_ACCESS);
            break;
        }
#ifdef _WIN32
        r = _chsize_s(s->fds[h], (long long)ne);
#else
        r = ftruncate(s->fds[h], (off_t)ne);
#endif
        if (r != 0) {
            rc = vmch_answer(VMCH_RC_IOERR);
        }
        break;
    }

    case VMCH_CMD_CLOSE: {
        int h = (int)ld32(base + VMCH_HDR_HANDLE) - 1;

        if (h < 0 || h >= VMCH_MAX_OPEN || s->fds[h] == -1) {
            rc = vmch_answer(VMCH_RC_ACCESS);
        } else {
            close(s->fds[h]);
            s->fds[h] = -1;
            g_free(s->open_paths[h]);       /* no longer open (issue #19) */
            s->open_paths[h] = NULL;
            s->open_write[h] = false;
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
            rc = vmch_answer(VMCH_RC_ACCESS);
            break;
        }
        if (len > 16 * 1024 * 1024) {
            rc = vmch_answer(VMCH_RC_IOERR);
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
            rc = vmch_answer(VMCH_RC_IOERR);
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
            rc = vmch_answer(VMCH_RC_ACCESS);
            break;
        }
        r = lseek(s->fds[h], (off_t)off, w[whence]);
        if (r < 0) {
            rc = vmch_answer(VMCH_RC_IOERR);
        } else {
            st32(base + VMCH_HDR_HANDLE, (uint32_t)r);
        }
        break;
    }

    case VMCH_CMD_FILEARGS: {
        char *path = arg_text(base, arglen);
        char *hp;
        GStatBuf st;
        uint8_t resp[28];

        if (!path) {
            rc = vmch_answer(VMCH_RC_BADPATH);
            break;
        }
        hp = host_path(s, path, &rc);
        if (!hp) {
            g_free(path);
            break;
        }
        if (g_stat(hp, &st) != 0) {
            rc = vmch_answer((errno == ENOENT) ? VMCH_RC_NOTFOUND : VMCH_RC_ACCESS);
        } else {
            uint32_t size = (uint32_t)st.st_size;
            uint32_t type = (uint32_t)riscos_type_for(hp, &st);
            uint64_t cs = date_cs_for(&st);
            uint32_t load, exec;

            load_exec_for(type, cs, &load, &exec);
            stl_le_p(resp + 0, size);
            /* A directory is signalled out of band, not as filetype 0, so a
             * real type-&000 file is not read back as a directory (#13).
             * load/exec above still use the real type. */
            stl_le_p(resp + 4, S_ISDIR(st.st_mode) ? VMCH_TYPE_DIR : type);
            stl_le_p(resp + 8, attrs_for(&st, hp));
            stl_le_p(resp + 12, (uint32_t)cs);
            stl_le_p(resp + 16, (uint32_t)(cs >> 32));
            stl_le_p(resp + 20, load);
            stl_le_p(resp + 24, exec);
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
            rc = vmch_answer(VMCH_RC_BADPATH);
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
            /* Why, from glib's own code rather than errno, which Windows
             * does not always set.  Everything but "not found" used to be
             * NOTDIR, which the guest reported as "unsupported operation"
             * whether the directory was a file, in use or forbidden
             * (issue #6). */
            int why = gerr ? gerr->code : -1;

            rc = vmch_answer((why == G_FILE_ERROR_NOENT || (!gerr && errno == ENOENT))
                     ? VMCH_RC_NOTFOUND
               : why == G_FILE_ERROR_NOTDIR ? VMCH_RC_NOTDIR
               : (why == G_FILE_ERROR_ACCES || why == G_FILE_ERROR_PERM)
                     ? VMCH_RC_ACCESS
               : VMCH_RC_IOERR);
            if (gerr) {
                g_error_free(gerr);
            }
        } else {
            /*
             * The whole directory, sorted, one page at a time.  HANDLE in
             * carries the continuation index -- how many sorted entries to
             * skip -- and HANDLE out says whether any remain (issue #8: a
             * directory over 63 entries, one page, used to stop here with
             * "directory too big").  The order is the guest leafname folded
             * case-insensitively, as FileCore keeps a catalogue, so a share
             * lists like a real disc and the page index is stable from call
             * to call (issue #4).
             */
            uint32_t start = ld32(base + VMCH_HDR_HANDLE);
            GPtrArray *ents = g_ptr_array_new_with_free_func(caten_free);
            uint32_t total, i, packed = 0;

            while ((name = g_dir_read_name(dir)) != NULL) {
                char *one = g_build_filename(hp, name, NULL);
                GStatBuf st = { 0 };
                uint32_t type = 0xFFF;
                bool isdir = false;
                caten *e;
                char *leaf;

                e = g_new0(caten, 1);
                if (g_stat(one, &st) == 0) {
                    isdir = S_ISDIR(st.st_mode);
                    e->size = (uint32_t)st.st_size;
                    e->attrs = attrs_for(&st, one);
                } else {
                    e->attrs = 3;
                }
                g_free(one);
                /* The guest is shown the mapped name, not the host one:
                 * `notes,ffb` is `notes` of type &FFB, `readme.txt` is
                 * `readme/txt`.  Listing the raw name was why a file
                 * could be catalogued and then not opened. */
                leaf = guest_leaf_of(name, isdir, &type);
                if (strlen(leaf) >= VMCH_MAX_NAME) {
                    g_free(leaf);
                    g_free(e);
                    continue;
                }
                e->leaf = leaf;
                /* A directory is signalled out of band, not as filetype 0,
                 * so a real type-&000 file is not listed as a directory
                 * (#13).  load/exec below still use the real type. */
                e->type = isdir ? VMCH_TYPE_DIR : type;
                /* Dated in the entry: a catalogue that carried only the
                 * type made every file in a *Ex listing show 01-Jan-1900,
                 * the only other date source being a FILEARGS per name. */
                load_exec_for(type, date_cs_for(&st), &e->load, &e->exec);
                g_ptr_array_add(ents, e);
            }
            g_dir_close(dir);
            g_ptr_array_sort(ents, caten_cmp);

            total = ents->len;
            for (i = start; i < total; i++) {
                caten *e = g_ptr_array_index(ents, i);

                if (used + VMCH_CAT_ENTRY > limit) {
                    break;                          /* this page is full */
                }
                /* Metadata first, then the name (issue #16): so a leafname
                 * up to VMCH_MAX_NAME-1 rides in the entry without moving
                 * the fixed fields.  memset NUL-pads the whole entry, so
                 * the name is NUL-terminated. */
                memset(resp + used, 0, VMCH_CAT_ENTRY);
                stl_le_p(resp + used + 0,  e->load);
                stl_le_p(resp + used + 4,  e->exec);
                stl_le_p(resp + used + 8,  e->type);
                stl_le_p(resp + used + 12, e->size);
                stl_le_p(resp + used + 16, e->attrs);
                memcpy(resp + used + 20, e->leaf, strlen(e->leaf));
                used += VMCH_CAT_ENTRY;
                packed++;
            }
            /* HANDLE out: any entries left after this page (the guest pages
             * on until it is 0), not an error. */
            st32(base + VMCH_HDR_HANDLE, (start + packed < total) ? 1 : 0);
            /* The entry count, so the guest need not divide arglen by the
             * (non-power-of-two) entry size -- a runtime divide the ROM
             * module cannot link (issue #16). */
            st32(base + VMCH_HDR_ARG, packed);
            if (used) {
                block_write(base + VMCH_HDR_SIZE, resp, used);
            }
            st32(base + VMCH_HDR_ARGLEN, used);
            g_ptr_array_free(ents, TRUE);
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
            rc = vmch_answer(VMCH_RC_BADPATH);
            break;
        }
        hp = host_path(s, path, &rc);
        if (!hp) {
            g_free(path);
            break;
        }
        fd = g_open(hp, O_WRONLY | O_CREAT | O_BINARY | O_TRUNC, 0644);
        if (fd < 0) {
            rc = vmch_answer((errno == ENOENT) ? VMCH_RC_NOTFOUND : VMCH_RC_ACCESS);
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
            rc = vmch_answer(VMCH_RC_BADPATH);
            break;
        }
        hp = host_path(s, path, &rc);
        if (!hp) {
            g_free(path);
            break;
        }
        if (g_stat(hp, &st) != 0) {
            rc = vmch_answer(VMCH_RC_NOTFOUND);
        } else if (meta_is_locked(hp)) {
            rc = vmch_answer(VMCH_RC_LOCKED);   /* locked: refuse (issue #12) */
        } else if (path_is_open(s, hp, false)) {
            rc = vmch_answer(VMCH_RC_OPEN);     /* open: refuse (issue #19) */
        } else if (S_ISDIR(st.st_mode)) {
            /* Every failure here used to be NOTDIR, which the guest could
             * only call "unsupported operation" — for the commonest one,
             * a directory that still holds something. */
            if (g_rmdir(hp) == 0) {
                rc = vmch_answer(VMCH_RC_OK);
            } else if (errno == ENOTEMPTY || errno == EEXIST) {
                rc = vmch_answer(VMCH_RC_NOTEMPTY);
            } else if (errno == EACCES || errno == EPERM || errno == EBUSY) {
                rc = vmch_answer(VMCH_RC_ACCESS);
            } else {
                rc = vmch_answer(VMCH_RC_IOERR);
            }
        } else {
            rc = vmch_answer(g_unlink(hp) == 0 ? VMCH_RC_OK : VMCH_RC_ACCESS);
        }
        g_free(hp);
        g_free(path);
        break;
    }

    case VMCH_CMD_RENAME: {
        char *both = arg_text(base, arglen);
        char *split, *hp1, *hp2;

        if (!both) {
            rc = vmch_answer(VMCH_RC_BADPATH);
            break;
        }
        split = strchr(both, '\n');
        if (!split) {
            rc = vmch_answer(VMCH_RC_BADPATH);
        } else {
            *split = '\0';
            hp1 = host_path(s, both, &rc);
            if (hp1) {
                hp2 = host_path(s, split + 1, &rc);
                if (hp2 && meta_is_locked(hp1)) {
                    rc = vmch_answer(VMCH_RC_LOCKED);   /* issue #12 */
                    g_free(hp2);
                    hp2 = NULL;
                } else if (hp2 && path_is_open(s, hp1, false)) {
                    rc = vmch_answer(VMCH_RC_OPEN);     /* issue #19 */
                    g_free(hp2);
                    hp2 = NULL;
                }
                if (hp2) {
                    GStatBuf st;

                    /* The type rides the host name, so a rename has to
                     * carry it across: renaming `logo,ff9` (which RISC OS
                     * calls "logo") to "icon" must land as `icon,ff9`, not
                     * as an untyped `icon`.  A rename() also carries the
                     * stored attribute xattr with the file, so a renamed
                     * object keeps its locked/public bits. */
                    if (g_stat(hp1, &st) == 0) {
                        uint32_t type = (uint32_t)riscos_type_for(hp1, &st);
                        g_autofree char *dir = g_path_get_dirname(hp2);
                        g_autofree char *leaf = g_path_get_basename(hp2);
                        g_autofree char *stem = host_base_of(leaf);
                        g_autofree char *want = host_leaf_for(stem, type,
                                                    S_ISDIR(st.st_mode));

                        if (strcmp(want, leaf) != 0) {
                            char *typed = g_build_filename(dir, want, NULL);
                            g_free(hp2);
                            hp2 = typed;
                        }
                    }
                    rc = vmch_answer(g_rename(hp1, hp2) == 0 ? VMCH_RC_OK
                                                 : VMCH_RC_ACCESS);
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
            rc = vmch_answer(VMCH_RC_BADPATH);
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

    case VMCH_CMD_FS_FUNC: {
        /*
         * FSEntry_Func reasons the host can answer better than the module:
         * the free space on the volume holding the share.
         *
         * 30, read free space (PRM 2-584, OS_FSControl 49): R0 free, R1
         * biggest object creatable, R2 disc size.  *Free calls it, and so
         * does something in !Boot, so a booting HostFS needs a plausible
         * answer rather than an error.  It is 32-bit, and a modern volume
         * overflows it, so the values saturate at 4 GiB rather than
         * wrapping to a small number, which would make a full disc look
         * nearly empty or the reverse.
         *
         * 35, read free space in 64 bits (OS_FSControl 55; FileSwitch
         * hdr/LowFSI fsfunc_ReadFreeSpace64): R0/R1 free, low and high
         * words; R2 biggest object; R3/R4 disc size.  The Free module's
         * window asks this first.  The biggest object stays saturated: a
         * RISC OS file's length is 32 bits whatever the disc.
         */
        uint32_t reason = ld32(base + VMCH_HDR_REGS + 0);
        uint64_t free_b = 0, total_b = 0;

        if (!s->root) {
            rc = vmch_answer(VMCH_RC_NOROOT);
            break;
        }
        if (reason != 30 && reason != 35) {
            rc = vmch_answer(VMCH_RC_BADCMD);
            break;
        }
#ifndef _WIN32
        {
            struct statvfs sv;

            if (statvfs(s->root, &sv) != 0) {
                rc = vmch_answer(VMCH_RC_IOERR);
                break;
            }
            free_b = (uint64_t)sv.f_bavail * sv.f_frsize;
            total_b = (uint64_t)sv.f_blocks * sv.f_frsize;
        }
#else
        {
            ULARGE_INTEGER avail, total, freeb;

            if (!GetDiskFreeSpaceExA(s->root, &avail, &total, &freeb)) {
                rc = vmch_answer(VMCH_RC_IOERR);
                break;
            }
            free_b = avail.QuadPart;
            total_b = total.QuadPart;
        }
#endif
        if (reason == 35) {
            st32(base + VMCH_HDR_REGS + 0, (uint32_t)free_b);
            st32(base + VMCH_HDR_REGS + 4, (uint32_t)(free_b >> 32));
            st32(base + VMCH_HDR_REGS + 8, free_b > 0xFFFFFFFFull
                                            ? 0xFFFFFFFFu : (uint32_t)free_b);
            st32(base + VMCH_HDR_REGS + 12, (uint32_t)total_b);
            st32(base + VMCH_HDR_REGS + 16, (uint32_t)(total_b >> 32));
            break;
        }
        st32(base + VMCH_HDR_REGS + 0, free_b > 0xFFFFFFFFull
                                        ? 0xFFFFFFFFu : (uint32_t)free_b);
        st32(base + VMCH_HDR_REGS + 4, free_b > 0xFFFFFFFFull
                                        ? 0xFFFFFFFFu : (uint32_t)free_b);
        st32(base + VMCH_HDR_REGS + 8, total_b > 0xFFFFFFFFull
                                        ? 0xFFFFFFFFu : (uint32_t)total_b);
        break;
    }

    case VMCH_CMD_FS_FILE: {
        /*
         * FSEntry_File, the reasons that *write* metadata: 1 write
         * catalogue info, 2 write load, 3 write exec, 4 write attributes,
         * 7 create with a type, and 8 create a directory.  The path comes
         * inline; R0 is the reason, R2 load, R3 exec, and R5 attributes —
         * except for reason 7, where R5 is the end address of the data
         * (PRM 2-541) and is not an attribute word at all.
         *
         * Not yet handled: PRM gives 1..4 a *wildcarded* name.  A single
         * named file works; `*SetType foo* FF9` does not expand here.
         *
         * These used to be answered by the module with "no-op, return
         * success", so *SetType appeared to work and did nothing.  A
         * silent lie is worse than an error, and there is no reason for
         * one: the type rides the host filename, so setting it is a
         * rename, and the datestamp is a utimes().
         */
        g_autofree char *path = arg_text(base, arglen);
        uint32_t reason = ld32(base + VMCH_HDR_REGS + 0);
        uint32_t load = ld32(base + VMCH_HDR_REGS + 8);
        uint32_t exec = ld32(base + VMCH_HDR_REGS + 12);
        uint32_t attr = ld32(base + VMCH_HDR_REGS + 20);
        g_autofree char *hp = NULL;
        uint32_t type;

        if (!s->root) {
            rc = vmch_answer(VMCH_RC_NOROOT);
            break;
        }
        if (!path) {
            rc = vmch_answer(VMCH_RC_BADPATH);
            break;
        }
        hp = host_path(s, path, &rc);
        if (!hp) {
            break;
        }

        if (reason == 7) {              /* create empty, of a given type */
            int fd;
            uint32_t start = ld32(base + VMCH_HDR_REGS + 16);   /* R4 */
            uint32_t end = attr;        /* R5 here is the end address, not an
                                         * attribute word (see the note) */

            /* Saving over a locked file is refused, as it is for delete and
             * rename (issue #12).  hp is the resolved existing object here,
             * before it is renamed to carry the new type. */
            if (meta_is_locked(hp)) {
                rc = vmch_answer(VMCH_RC_LOCKED);
                break;
            }
            if (path_is_open(s, hp, false)) {
                rc = vmch_answer(VMCH_RC_OPEN);
                break;
            }

            type = type_of_load(load);
            if (type != 0xFFFFFFFFu) {
                /* Name it for its type now, rather than creating it and
                 * renaming a moment later. */
                g_autofree char *dir = g_path_get_dirname(hp);
                g_autofree char *leaf = g_path_get_basename(hp);
                g_autofree char *stem = host_base_of(leaf);
                g_autofree char *want = host_leaf_for(stem, type, false);
                g_autofree char *dest = g_build_filename(dir, want, NULL);

                /* The name resolved to an existing host file spelt for
                 * another type (say `f,ff9`), but this type wants a
                 * different host spelling (`f,ffd`).  Creating the new one
                 * would leave the old beside it -- two host files for the
                 * one RISC OS leaf, and *Load could pick the stale one
                 * (issue #18).  Remove the old spelling first, so one RISC
                 * OS name is always one host file. */
                if (strcmp(dest, hp) != 0
                    && g_file_test(hp, G_FILE_TEST_EXISTS)
                    && !g_file_test(hp, G_FILE_TEST_IS_DIR)) {
                    g_unlink(hp);
                }
                g_free(hp);
                hp = g_steal_pointer(&dest);
            }
            fd = g_open(hp, O_WRONLY | O_CREAT | O_BINARY | O_TRUNC, 0644);
            if (fd < 0) {
                rc = vmch_answer((errno == ENOENT) ? VMCH_RC_NOTFOUND : VMCH_RC_ACCESS);
            } else {
                /* PRM 2-541: reason 7 creates a file whose extent is
                 * R5-R4.  *Create <len> and OS_File 11 ask for a length
                 * this way; the file was left empty, so *Create &800 gave 0
                 * bytes rather than 2048 (issue #15).  The guest forwards R4
                 * (file_on_host) so the length is exact. */
                uint32_t want_len = (end >= start) ? (end - start) : 0;

                if (want_len && ftruncate(fd, (off_t)want_len) != 0) {
                    rc = vmch_answer(VMCH_RC_IOERR);
                }
                close(fd);
            }
            break;
        }

        if (reason == 8) {
            /* Create directory (PRM 2-560): *CDir, and the Filer's New
             * directory.  One that already exists is left as it is, which
             * the PRM allows; a file in the way is an error.  Directories
             * carry no type or date here, so R2 and R3 need no action. */
            GStatBuf st;

            if (g_mkdir(hp, 0755) == 0) {
                break;
            }
            if (errno == EEXIST && g_stat(hp, &st) == 0 && S_ISDIR(st.st_mode)) {
                break;
            }
            rc = vmch_answer((errno == ENOENT) ? VMCH_RC_NOTFOUND
               : (errno == EEXIST) ? VMCH_RC_ACCESS : VMCH_RC_IOERR);
            break;
        }

        /* 1..4: the object must exist */
        {
            GStatBuf st;

            if (g_stat(hp, &st) != 0) {
                rc = vmch_answer((errno == ENOENT) ? VMCH_RC_NOTFOUND : VMCH_RC_ACCESS);
                break;
            }

            /*
             * A directory holds none of it.  Its host name carries no type,
             * its mtime is the host's to keep, and RISC OS's "locked" on a
             * directory forbids deleting it, not adding to it — which is
             * what taking away write permission would do instead.  This used
             * to fall through: *Copy writes the source directory's load and
             * exec onto the new one (&FFFFFDxx, type Data), and every copied
             * directory was renamed "name,ffd".  RISC OS still saw the right
             * names, so only the host side showed it.
             */
            if (S_ISDIR(st.st_mode)) {
                break;
            }

            if (reason == 1 || reason == 4) {
                /*
                 * The whole RISC OS attribute byte is kept host-side so
                 * every bit round-trips -- the locked and public-access
                 * bits the mode cannot carry included (issue #12); the
                 * stored byte, not the mode, is what attrs_for reports.
                 *
                 * The owner (the emulator) keeps read+write on the host
                 * whatever RISC OS says, so it can always update the file
                 * and its stored byte -- setxattr on an owner-read-only
                 * file is refused, which would strand a *Access that
                 * unlocks.  "locked" and owner "read only" are enforced
                 * explicitly (meta_is_locked), not through the host mode.
                 * The public bits are reflected, for the host side's sake.
                 */
                mode_t m = st.st_mode & ~(mode_t)(S_IRUSR | S_IWUSR |
                             S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
                m |= S_IRUSR | S_IWUSR;
                if (attr & 0x10) {                   /* public read */
                    m |= S_IROTH | S_IRGRP;
                }
                if (attr & 0x20) {                   /* public write */
                    m |= S_IWOTH | S_IWGRP;
                }
                if (g_chmod(hp, m) != 0) {
                    rc = vmch_answer(VMCH_RC_ACCESS);
                    break;
                }
                /* After the chmod: the file is owner-writable now, so the
                 * stored byte cannot be refused. */
                meta_set_attr(hp, attr);
            }

            if (reason == 1 || reason == 2 || reason == 3) {
                uint64_t cs;

                /*
                 * PRM 2-536..2-538: reason 1 carries load AND exec, but
                 * reason 2 carries only R2 (load) and reason 3 only R3
                 * (exec) — the other register is whatever the caller left
                 * there.  Take the missing half from the file as it
                 * stands, or a WriteLoad would stamp a date built from
                 * junk.
                 */
                if (reason != 1) {
                    uint32_t cur_type = (uint32_t)riscos_type_for(hp, &st);
                    uint32_t cur_load, cur_exec;

                    load_exec_for(cur_type, date_cs_for(&st),
                                  &cur_load, &cur_exec);
                    if (reason == 2) {
                        exec = cur_exec;
                    } else {
                        load = cur_load;
                    }
                }

                type = type_of_load(load);
                if (reason != 3 && type != 0xFFFFFFFFu) {
                    g_autofree char *dir = g_path_get_dirname(hp);
                    g_autofree char *leaf = g_path_get_basename(hp);
                    g_autofree char *stem = host_base_of(leaf);
                    g_autofree char *want = host_leaf_for(stem, type, false);

                    if (strcmp(want, leaf) != 0) {
                        g_autofree char *dest = g_build_filename(dir, want,
                                                                 NULL);
                        /* Setting a type renames the host file.  That is
                         * what encoding the type in the name means, and
                         * it surprises anyone watching the directory, so
                         * it is traced. */
                        if (g_rename(hp, dest) == 0) {
                            vmch_trace("vmch: settype %s -> %s (&%03x)\n",
                                       leaf, want, type);
                            g_free(hp);
                            hp = g_steal_pointer(&dest);
                        } else {
                            rc = vmch_answer(VMCH_RC_ACCESS);
                            break;
                        }
                    }
                }

                /* The 5-byte instant back to a host time: UTC to UTC, the
                 * inverse of date_cs_for.  Only when it looks like one: an
                 * untyped file's load/exec are real addresses and must not
                 * be read as a date. */
                if (type != 0xFFFFFFFFu) {
                    cs = ((uint64_t)(load & 0xFF) << 32) | exec;
                    /*
                     * A negative time_t is a date before 1970: legal RISC
                     * OS (its epoch is 1900) and kept by FileCore, but the
                     * old `> 1970` guard silently dropped every such stamp,
                     * so a 1965 date read back as "now" (issue #14).  Only
                     * an all-zero instant means "no datestamp"; leave that
                     * alone and apply everything else, signed.
                     */
                    if (cs != 0) {
                        struct utimbuf ut;

                        ut.actime = (time_t)((int64_t)(cs / 100)
                                             - 2208988800LL);
                        ut.modtime = ut.actime;
                        (void)g_utime(hp, &ut);
                    }
                }
            }
        }
        break;
    }

    case VMCH_CMD_FS_GETBYTES:
    case VMCH_CMD_FS_PUTBYTES: {
        /* FSEntry_GetBytes / _PutBytes on a buffered file, whole, in one
         * round trip: R1 handle, R2 guest logical buffer, R3 count, R4
         * file offset.  No exit registers (PRM 2-544).  The offset rides
         * in the request, so there is no seek and no per-page chunking —
         * the v0 module cost two doorbells per 4 KiB for want of both.
         *
         * What is bounded is the host's buffer, not the transfer: a count
         * past VMCH_XFER_PIECE goes through in pieces of that size, still
         * in the one round trip.  It used to be refused with an I/O error
         * past 16 MiB, which the guest took for an unmapped buffer, and
         * FileSwitch hands over a count that size whenever the caller's
         * buffer is that big (issue #6). */
        bool is_put = (cmd == VMCH_CMD_FS_PUTBYTES);
        uint32_t r1 = ld32(base + VMCH_HDR_REGS + 4);
        uint64_t addr = (uint64_t)ld32(base + VMCH_HDR_REGS + 8);
        uint32_t len = ld32(base + VMCH_HDR_REGS + 12);
        uint64_t off = (uint64_t)ld32(base + VMCH_HDR_REGS + 16);
        int h = (int)r1 - 1;
        g_autofree uint8_t *tmp = NULL;
        uint32_t done = 0;

        if (!s->root) {
            rc = vmch_answer(VMCH_RC_NOROOT);
            break;
        }
        if (h < 0 || h >= VMCH_MAX_OPEN || s->fds[h] == -1) {
            rc = vmch_answer(VMCH_RC_ACCESS);
            break;
        }
        tmp = g_malloc(MAX(MIN(len, VMCH_XFER_PIECE), 1));

        while (done < len) {
            uint32_t part = MIN(len - done, VMCH_XFER_PIECE);
            ssize_t n;

            if (is_put) {
                uint32_t got = guest_rw_counted(addr + done, tmp, part, false);

                if (got != part) {
                    /* R3: how far the translation got, over the whole
                     * transfer — the guest faults the rest in and retries */
                    st32(base + VMCH_HDR_REGS + 12, done + got);
                    rc = vmch_answer(VMCH_RC_BADADDR);
                    break;
                }
                n = pwrite(s->fds[h], tmp, part, (off_t)(off + done));
                if (n < 0) {
                    rc = vmch_answer(VMCH_RC_IOERR);
                    break;
                }
                if ((uint32_t)n != part) {
                    rc = vmch_answer(VMCH_RC_FULL);  /* short write: out of space */
                    break;
                }
            } else {
                uint32_t put;

                n = pread(s->fds[h], tmp, part, (off_t)(off + done));
                if (n < 0) {
                    rc = vmch_answer(VMCH_RC_IOERR);
                    break;
                }
                /* A short read is the normal end of a file: the count is a
                 * multiple of the buffer size, so the last block runs past
                 * the extent.  Zero the tail rather than leave the guest's
                 * buffer holding whatever was there before. */
                if ((uint32_t)n < part) {
                    memset(tmp + n, 0, part - (uint32_t)n);
                }
                put = guest_rw_counted(addr + done, tmp, part, true);
                if (put != part) {
                    done += put;
                    rc = vmch_answer(VMCH_RC_BADADDR);
                    break;
                }
            }
            done += part;
        }
        if (!is_put && rc != VMCH_RC_IOERR) {
            st32(base + VMCH_HDR_REGS + 12, done);  /* bytes delivered */
        }
        break;
    }

    case VMCH_CMD_FS_ARGS: {
        /*
         * FSEntry_Args reasons the host does.  Only 8 so far, write zeroes
         * (PRM 2-551): R1 handle, R2 file offset, R3 count.  FileSwitch
         * calls it on a buffered filing system to grow a file past its end
         * (s/StreamBits, ZeroFileFromPosition); both numbers are multiples
         * of the buffer size, and the file offset rides in the request as
         * it does for PutBytes.
         */
        static const uint8_t zeros[65536];
        uint32_t reason = ld32(base + VMCH_HDR_REGS + 0);
        int h = (int)ld32(base + VMCH_HDR_REGS + 4) - 1;
        uint64_t off = ld32(base + VMCH_HDR_REGS + 8);
        uint32_t count = ld32(base + VMCH_HDR_REGS + 12);

        if (!s->root) {
            rc = vmch_answer(VMCH_RC_NOROOT);
            break;
        }
        if (reason != 8) {
            rc = vmch_answer(VMCH_RC_BADCMD);
            break;
        }
        if (h < 0 || h >= VMCH_MAX_OPEN || s->fds[h] == -1) {
            rc = vmch_answer(VMCH_RC_ACCESS);
            break;
        }
        while (count != 0) {
            size_t n = MIN(count, sizeof(zeros));
            ssize_t w = pwrite(s->fds[h], zeros, n, (off_t)off);

            if (w < 0) {
                rc = vmch_answer(VMCH_RC_IOERR);
                break;
            }
            if ((size_t)w != n) {
                rc = vmch_answer(VMCH_RC_FULL);
                break;
            }
            off += n;
            count -= (uint32_t)n;
        }
        break;
    }

    default:
        rc = vmch_answer(VMCH_RC_BADCMD);
        break;
    }

    /* Development trace: every doorbell request, one line, with the
     * first block words so layout disputes can be settled from the
     * log alone. */
    {
        /* us= is the host time spent on the request, which is what a
         * boot's filing system calls cost beside its CPU (FSDESIGN §13 B2) */
        vmch_trace("vmch: cmd=%u seq=%u rc=%u hnd=%08x arglen=%u us=%lld",
                cmd, ld32(base + VMCH_HDR_SEQ), rc,
                ld32(base + VMCH_HDR_HANDLE), arglen,
                (long long)(g_get_monotonic_time() - started));
        if (cmd >= 0x100) {
            vmch_trace(" R1=%08x R2=%08x R3=%08x R4=%08x",
                    ld32(base + VMCH_HDR_REGS + 4),
                    ld32(base + VMCH_HDR_REGS + 8),
                    ld32(base + VMCH_HDR_REGS + 12),
                    ld32(base + VMCH_HDR_REGS + 16));
        }
        /* Every host refusal says what the host actually said.  Without
         * this an unstorable name reads as an I/O error: HostFS 2.01's
         * message names the request and the code ("host I/O error
         * (FS_FILE 8: cmd 261, rc 10)"), and 2.00's said only "unsupported
         * operation", but neither can say errno, and EINVAL rather than
         * EIO is the whole diagnosis. */
        if (rc != VMCH_RC_OK && rc != VMCH_RC_NOTFOUND && vmch_errno != 0) {
            vmch_trace(" errno=%d(%s)", vmch_errno, strerror(vmch_errno));
        }
        if (cmd == VMCH_CMD_OPEN || cmd == VMCH_CMD_CREATE ||
            cmd == VMCH_CMD_DELETE || cmd == VMCH_CMD_CAT ||
            cmd == VMCH_CMD_FILEARGS || cmd == VMCH_CMD_RENAME) {
            uint32_t i;
            vmch_trace(" path=");
            /* The whole path, not a 64-byte cut: the leaf is where a name
             * problem shows, and a 64-byte cut hid StrongED's TRUE>>>1
             * three characters short of the answer (issue #6). */
            for (i = 0; i < arglen && i < VMCH_MAX_ARG; i++) {
                int c = ld8(base + VMCH_HDR_SIZE + i);
                vmch_trace("%c", (c >= 32 && c < 127) ? c : '.');
            }
            if (vmch_remapped) {
                vmch_name_remapped(base, arglen);
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
        s->open_paths[i] = NULL;
        s->open_write[i] = false;
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
