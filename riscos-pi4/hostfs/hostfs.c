/* hostfs.c — the HostFS module body: a filing system over the vmchannel
 * doorbell (riscos-pi4/FSDESIGN.md).
 *
 * Every filing-system operation becomes one vmchannel request: the block
 * is built in this module's statics (claimed RMA, identity-mapped like all
 * RAM on RISC OS 5), its physical address is poked into the doorbell, and
 * the device answers in place.  The eight FSEntry veneers in
 * hostfs_entries.s marshal RISC OS's register-and-V-flag contract into
 * the C handlers here, each returning 0 or an error-block pointer.
 *
 * What v0 implements is what the desktop exercises: open/read/write/
 * close/args, OS_File read-cat/load/save/create/delete, and the two
 * fsfunc directory reads (*Cat and the Filer both arrive as
 * fsfunc_ReadDirEntriesInfo, five info words then the name).  gbpb is
 * left at zero so FileSwitch synthesises data transfers from get/put.
 */

typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned long long uint64_t;

/* ---- the doorbell (0xfd400000, the low-peripheral window) ---------- */

#define VMCH_BASE       0xFD400000u
#define VMCH_MAGIC      0x00
#define VMCH_FEATURES   0x08
#define VMCH_CMD        0x0C
#define VMCH_MAGIC_VALUE 0x48434D56u
#define VMCH_FEATURE_FS 0x1

/* request block */
#define H_CMD   0
#define H_SEQ   4
#define H_RC    8
#define H_HND   12
#define H_ARG   16
#define H_ARGLEN 20
#define H_SIZE  64

#define C_PING     0
#define C_OPEN     1
#define C_CLOSE    2
#define C_READ     3
#define C_WRITE    4
#define C_SEEK     5
#define C_FILEARGS 6
#define C_CAT      7
#define C_CREATE   8
#define C_DELETE   9
#define C_RENAME   10
#define C_TIME     17

#define RC_OK        0
#define RC_NOROOT    1
#define RC_NOTFOUND  2
#define RC_ACCESS    3
#define RC_BADPATH   4
#define RC_FULL      5
#define RC_HANDLES   6
#define RC_NOTDIR    7
#define RC_ISDIR     8

#define O_READ   0x1
#define O_WRITE  0x2
#define O_CREATE 0x4
#define O_TRUNC  0x8

#define VMCH_MAX_ARG 8192

/* ---- RISC OS bits -------------------------------------------------- */

typedef struct {
    int32_t errnum;
    char errmess[48];
} _kernel_oserror;

/* not a const pointer: an initialised static cannot hold an absolute
 * address under RWPI, so the doorbell is reached through a macro */
#define vmch_reg(idx) (*(volatile uint32_t *)(VMCH_BASE + (idx) * 4))

static uint32_t req[16 + VMCH_MAX_ARG / 4] __attribute__((aligned(16)));
static uint32_t seq;

/* Error blocks: statics are fine, FileSwitch copies the message at once */
static _kernel_oserror err_badcmd = { 0x1E4, "HostFS: bad command" };
static _kernel_oserror err_notfound = { 0x1D6, "HostFS: file not found" };
static _kernel_oserror err_access = { 0xBD, "HostFS: access denied" };
static _kernel_oserror err_badpath = { 0x1E4, "HostFS: bad path" };
static _kernel_oserror err_full = { 0x101, "HostFS: directory too big" };
static _kernel_oserror err_nodev = { 0x1E4, "HostFS: no doorbell device" };
static _kernel_oserror err_nofs = { 0x1E4, "HostFS: device has no root" };
static _kernel_oserror err_unsupported = { 0x1E8, "HostFS: unsupported operation" };

static _kernel_oserror *rc_to_error(uint32_t rc)
{
    switch (rc) {
    case RC_OK: return 0;
    case RC_NOTFOUND: return &err_notfound;
    case RC_ACCESS: case RC_HANDLES: return &err_access;
    case RC_BADPATH: return &err_badpath;
    case RC_FULL: return &err_full;
    case RC_NOROOT: return &err_nofs;
    default: return &err_unsupported;
    }
}

/* ---- SWIs, X-bit, V read back in the same asm block ---------------- */

static uint32_t vmch_req_close(uint32_t h);

/* OS_Module 18 by title: out R1 = module base, R3 = private word
 * pointer — the two things OS_FSControl 12 wants. */
static uint32_t os_module_18(const char *title, uint32_t *base,
                             uint32_t *privword)
{
    register uint32_t r0 __asm("r0") = 18;
    register uint32_t r1 __asm("r1") = (uint32_t)title;
    register uint32_t r3 __asm("r3");
    register uint32_t v __asm("r2");
    __asm volatile("swi 0x2001E\n\tmrs r2, cpsr"
                   : "+r"(r0), "+r"(r1), "=r"(v), "=r"(r3)
                   :
                   : "r12", "lr", "cc", "memory");
    if (v & (1u << 28)) {
        return 1;
    }
    *base = r1;
    *privword = r3;
    return 0;
}

static uint32_t os_fscontrol(uint32_t reason, uint32_t r1, uint32_t r2,
                             uint32_t r3)
{
    register uint32_t r0 __asm("r0") = reason;
    register uint32_t ar1 __asm("r1") = r1;
    register uint32_t ar2 __asm("r2") = r2;
    register uint32_t ar3 __asm("r3") = r3;
    register uint32_t v __asm("r4");
    __asm volatile("swi 0x20029\n\tmrs r4, cpsr"
                   : "+r"(r0), "+r"(ar1), "+r"(ar2), "+r"(ar3), "=r"(v)
                   :
                   : "r12", "lr", "cc", "memory");
    if (v & (1u << 28)) {
        return 1;                   /* error block in r0 */
    }
    return 0;
}

static uint32_t os_validate(void *lo, void *hi)
{
    register uint32_t r0 __asm("r0") = (uint32_t)lo;
    register uint32_t r1 __asm("r1") = (uint32_t)hi;
    register uint32_t v __asm("r2");
    __asm volatile("swi 0x2003A\n\tmrs r2, cpsr"
                   : "+r"(r0), "+r"(r1), "=r"(v)
                   :
                   : "r3", "r12", "lr", "cc", "memory");
    return (v & (1u << 28)) ? 1 : 0;   /* 0 = all valid */
}

/* ---- the transport -------------------------------------------------- */

static uint32_t vmch_go(void)
{
    req[H_SEQ] = ++seq;
    vmch_reg(VMCH_CMD / 4) = (uint32_t)req;
    return req[H_RC];
}

static uint32_t vmch_arglen(void)
{
    return req[H_ARGLEN];
}

/* path: NUL-terminated RISC OS path into the arg area */
static void put_path(const char *path)
{
    char *arg = (char *)&req[H_SIZE / 4];
    uint32_t i = 0;

    while (path[i] && i < VMCH_MAX_ARG - 1) {
        arg[i] = path[i];
        i++;
    }
    arg[i] = 0;
    req[H_ARGLEN] = i;
}

/* one catalogue entry as cached from CAT */
struct catent {
    uint32_t load, exec, len, attr, type;
    char name[48];
};
static struct catent catents[128];

/* load/exec for a typed, dated file: the 5-byte centisecond instant is
 * 40 bits — load's low byte carries bits 39..32, exec carries 31..0 */
static void stamp_load_exec(uint32_t type, uint64_t cs,
                            uint32_t *load, uint32_t *exec)
{
    if (type == 0xFFF || cs == 0) {
        *load = *exec = (type == 0xFFF) ? 0xFFFFFFFFu : 0;
        return;
    }
    *load = 0xFFF00000u | (type << 8) | (uint32_t)(cs >> 32);
    *exec = (uint32_t)cs;
}

static uint32_t info_of(const char *path, uint32_t *size, uint32_t *type,
                        uint32_t *attr, uint64_t *cs)
{
    req[H_CMD] = C_FILEARGS;
    req[H_HND] = 0;
    put_path(path);
    uint32_t rc = vmch_go();
    if (rc != RC_OK) {
        return rc;
    }
    uint32_t *resp = &req[H_SIZE / 4];
    *size = resp[0];
    *type = resp[1];
    *attr = resp[2];
    *cs = (uint64_t)resp[3] | ((uint64_t)resp[4] << 32);
    return RC_OK;
}

/* object type in the OS_File sense: 1 file, 2 directory, 0 none */
static uint32_t objtype_of(uint32_t rc, uint32_t type)
{
    if (rc == RC_NOTFOUND) {
        return 0;
    }
    if (rc != RC_OK) {
        return 0;
    }
    return type == 0 ? 2 : 1;
}

/* ---- C handlers: r[0]..r[9] are the caller's registers ------------- */

int hostfs_c_open(uint32_t *r, void *ws)
{
    (void)ws;
    uint32_t mode = r[0];
    const char *name = (const char *)r[1];

    req[H_CMD] = C_OPEN;
    uint32_t flags = O_READ;
    if (mode == 1) {
        flags = O_WRITE | O_CREATE | O_TRUNC;
    } else if (mode == 2) {
        flags = O_WRITE | O_READ;
    } else if (mode == 3) {
        flags = O_READ | O_WRITE;      /* no creation */
    }
    req[H_HND] = flags;
    put_path(name);
    uint32_t rc = vmch_go();
    if (rc != RC_OK) {
        /* OS_Find 0x40: must exist; 0x80: create.  FileSwitch already
         * translated, so plain not-found is an error. */
        return (int)rc_to_error(rc);
    }
    uint32_t h = req[H_HND];

    uint32_t size, type, attr;
    uint64_t cs;
    rc = info_of(name, &size, &type, &attr, &cs);
    if (rc == RC_NOTFOUND) {
        size = 0;                      /* newly created file */
        type = 0xFFF;
    } else if (rc != RC_OK) {
        vmch_req_close(h);
        return (int)rc_to_error(rc);
    }

    r[0] = (1u << 31) | (1u << 30);    /* write+read permission */
    r[1] = h;
    r[2] = 0;                          /* unbuffered: every get/put
                                        * carries the file address */
    r[3] = (type == 0) ? 0 : size;
    r[4] = 0;
    return 0;
}

static uint32_t vmch_req_close(uint32_t h)
{
    req[H_CMD] = C_CLOSE;
    req[H_HND] = h;
    req[H_ARGLEN] = 0;
    return vmch_go();
}

int hostfs_c_get(uint32_t *r, void *ws)
{
    (void)ws;
    uint32_t h = r[1];
    uint32_t *mem = (uint32_t *)r[2];
    uint32_t n = r[3];
    uint32_t ptr = r[4];

    /* seek to the file address, then read */
    req[H_CMD] = C_SEEK;
    req[H_HND] = h;
    req[H_ARGLEN] = 0;
    req[H_ARG + 0] = ptr;
    req[H_ARG + 1] = 0;                /* SEEK_SET */
    if (vmch_go() != RC_OK) {
        return (int)&err_access;
    }

    req[H_CMD] = C_READ;
    req[H_HND] = h;
    req[H_ARGLEN] = 0;
    req[H_ARG + 0] = (uint32_t)mem;
    req[H_ARG + 1] = n;
    uint32_t rc = vmch_go();
    if (rc != RC_OK) {
        return (int)rc_to_error(rc);
    }
    r[3] = req[H_HND];                 /* bytes actually transferred */
    return 0;
}

int hostfs_c_put(uint32_t *r, void *ws)
{
    (void)ws;
    uint32_t h = r[1];
    uint32_t *mem = (uint32_t *)r[2];
    uint32_t n = r[3];
    uint32_t ptr = r[4];

    req[H_CMD] = C_SEEK;
    req[H_HND] = h;
    req[H_ARGLEN] = 0;
    req[H_ARG + 0] = ptr;
    req[H_ARG + 1] = 0;
    if (vmch_go() != RC_OK) {
        return (int)&err_access;
    }

    req[H_CMD] = C_WRITE;
    req[H_HND] = h;
    req[H_ARGLEN] = 0;
    req[H_ARG + 0] = (uint32_t)mem;
    req[H_ARG + 1] = n;
    uint32_t rc = vmch_go();
    if (rc != RC_OK) {
        return (int)rc_to_error(rc);
    }
    r[3] = req[H_HND];
    return 0;
}

int hostfs_c_args(uint32_t *r, void *ws)
{
    (void)ws;
    switch (r[0]) {
    case 0:                            /* read PTR: not tracked here */
        r[2] = 0;
        return 0;
    case 1:                            /* set PTR: carried per get/put */
        return 0;
    case 2:                           /* read EXT: FileSwitch takes the
                                        * extent from Open for unbuffered
                                        * streams and does not ask */
        r[2] = 0;
        return 0;
    case 3:                            /* set EXT: unsupported */
        return (int)&err_unsupported;
    case 255:                          /* flush */
        return 0;
    default:
        return (int)&err_unsupported;
    }
}

int hostfs_c_close(uint32_t *r, void *ws)
{
    (void)ws;
    uint32_t rc = vmch_req_close(r[1]);
    return (rc == RC_OK) ? 0 : (int)rc_to_error(rc);
}

int hostfs_c_file(uint32_t *r, void *ws)
{
    (void)ws;
    uint32_t reason = r[0];
    const char *name = (const char *)r[1];

    switch (reason) {
    case 5:                           /* read catalogue info */
    case 9: {                         /* ...without length */
        uint32_t size, type, attr;
    uint64_t cs;
        uint32_t rc = info_of(name, &size, &type, &attr, &cs);
        uint32_t obj = objtype_of(rc, type);
        r[0] = obj;
        if (!obj) {
            return 0;
        }
        uint32_t load, exec;
        if (obj == 2) {
            load = exec = 0;
            size = 0;
            attr = 3;
        } else {
            stamp_load_exec(type, cs, &load, &exec);
        }
        r[2] = load;
        r[3] = exec;
        r[4] = size;
        r[5] = attr;
        return 0;
    }

    case 0xFF: {                       /* load into memory at r2 */
        uint32_t size, type, attr;
    uint64_t cs;
        uint32_t rc = info_of(name, &size, &type, &attr, &cs);
        if (rc == RC_NOTFOUND) {
            r[0] = 0;
            return 0;
        }
        if (rc != RC_OK) {
            return (int)rc_to_error(rc);
        }
        if (type == 0) {
            r[0] = 2;
            return (int)&err_unsupported;
        }
        req[H_CMD] = C_OPEN;
        req[H_HND] = O_READ;
        put_path(name);
        rc = vmch_go();
        if (rc != RC_OK) {
            return (int)rc_to_error(rc);
        }
        uint32_t h = req[H_HND];

        req[H_CMD] = C_READ;
        req[H_HND] = h;
        req[H_ARGLEN] = 0;
        req[H_ARG + 0] = r[2];
        req[H_ARG + 1] = size;
        rc = vmch_go();
        vmch_req_close(h);
        if (rc != RC_OK) {
            return (int)rc_to_error(rc);
        }
        uint32_t load, exec;
        stamp_load_exec(type, cs, &load, &exec);
        r[0] = 1;
        r[2] = load;
        r[3] = exec;
        r[4] = size;
        r[5] = attr;
        return 0;
    }

    case 0: {                          /* save: data r4..r5 */
        uint32_t len = r[5] - r[4];
        req[H_CMD] = C_CREATE;
        req[H_HND] = 0;
        put_path(name);
        uint32_t rc = vmch_go();
        if (rc != RC_OK) {
            return (int)rc_to_error(rc);
        }
        req[H_CMD] = C_OPEN;
        req[H_HND] = O_WRITE | O_TRUNC;
        put_path(name);
        rc = vmch_go();
        if (rc != RC_OK) {
            return (int)rc_to_error(rc);
        }
        uint32_t h = req[H_HND];
        req[H_CMD] = C_WRITE;
        req[H_HND] = h;
        req[H_ARGLEN] = 0;
        req[H_ARG + 0] = r[4];
        req[H_ARG + 1] = len;
        rc = vmch_go();
        vmch_req_close(h);
        if (rc != RC_OK) {
            return (int)rc_to_error(rc);
        }
        uint32_t size, type, attr;
    uint64_t cs;
        rc = info_of(name, &size, &type, &attr, &cs);
        uint32_t load = 0xFFFFFFFFu, exec = 0xFFFFFFFFu;
        if (rc == RC_OK) {
            stamp_load_exec(type, cs, &load, &exec);
        }
        r[0] = 1;
        r[2] = load;
        r[3] = exec;
        r[4] = len;
        r[5] = 3;
        return 0;
    }

    case 7: {                          /* create empty, given size */
        req[H_CMD] = C_CREATE;
        req[H_HND] = 0;
        put_path(name);
        uint32_t rc = vmch_go();
        if (rc != RC_OK) {
            return (int)rc_to_error(rc);
        }
        r[0] = 1;
        r[2] = r[3] = 0xFFFFFFFFu;
        r[4] = 0;
        r[5] = 3;
        return 0;
    }

    case 6: {                          /* delete */
        req[H_CMD] = C_DELETE;
        req[H_HND] = 0;
        put_path(name);
        uint32_t rc = vmch_go();
        if (rc == RC_NOTFOUND) {
            r[0] = 0;
            return 0;
        }
        if (rc != RC_OK) {
            return (int)rc_to_error(rc);
        }
        r[0] = 1;
        return 0;
    }

    default:
        return (int)&err_unsupported;
    }
}

int hostfs_c_func(uint32_t *r, void *ws)
{
    (void)ws;
    switch (r[0]) {
    case 0: case 1:                    /* *Dir / *Lib: FileSwitch stores
                                        * the prefix; nothing to do */
        return 0;
    case 16:                           /* shutdown */
    case 17:                           /* print banner */
        return 0;

    case 14:                           /* read names */
    case 15: {                         /* read names + info */
        uint32_t off = r[4];           /* continuation index */
        uint32_t want = r[3];          /* entries wanted */
        uint8_t *buf = (uint8_t *)r[2];
        uint32_t buflen = r[5];

        if (r[0] == 14) {
            return (int)&err_unsupported;   /* let FileSwitch not need it */
        }

        /* catalogue the whole directory into the cache */
        req[H_CMD] = C_CAT;
        req[H_HND] = 0;
        put_path((const char *)r[1]);
        uint32_t rc = vmch_go();
        if (rc != RC_OK) {
            return (int)rc_to_error(rc);
        }
        uint32_t nbytes = vmch_arglen();
        uint32_t n = nbytes / 64;
        if (n > 128) {
            n = 128;
        }
        uint8_t *src = (uint8_t *)&req[H_SIZE / 4];
        for (uint32_t i = 0; i < n; i++) {
            struct catent *e = &catents[i];
            uint8_t *en = src + i * 64;
            e->type = ((uint32_t)en[48]) | ((uint32_t)en[49] << 8)
                    | ((uint32_t)en[50] << 16) | ((uint32_t)en[51] << 24);
            e->len = ((uint32_t)en[52]) | ((uint32_t)en[53] << 8)
                   | ((uint32_t)en[54] << 16) | ((uint32_t)en[55] << 24);
            e->attr = ((uint32_t)en[56]) | ((uint32_t)en[57] << 8)
                    | ((uint32_t)en[58] << 16) | ((uint32_t)en[59] << 24);
            /* date: unknown here; the entry's own date would need a
             * FILEARGS per name — v0 leaves files undated-typed */
            for (int k = 0; k < 47; k++) {
                e->name[k] = en[k];
                if (!en[k]) {
                    break;
                }
            }
            e->name[47] = 0;
        }

        /* fill the caller's buffer from the continuation offset */
        uint32_t done = 0;
        while (off < n && done < want) {
            struct catent *e = &catents[off];
            uint32_t namelen = 0;
            while (e->name[namelen]) {
                namelen++;
            }
            uint32_t entlen = 20 + ((namelen + 1 + 3) & ~3u);
            if (entlen > buflen) {
                break;
            }
            uint32_t load, exec;
            if (e->type == 0) {
                load = exec = 0;
                e->len = 0;
                e->attr = 3;
            } else {
                stamp_load_exec(e->type, 0, &load, &exec);
            }
            uint32_t *w = (uint32_t *)buf;
            w[0] = load;
            w[1] = exec;
            w[2] = e->len;
            w[3] = e->attr;
            w[4] = e->type;
            for (uint32_t k = 0; k < ((namelen + 1 + 3) & ~3u); k++) {
                buf[20 + k] = k <= namelen ? e->name[k] : 0;
            }
            buf += entlen;
            buflen -= entlen;
            off++;
            done++;
        }
        r[3] = done;
        r[4] = (off >= n) ? 0xFFFFFFFFu : off;
        return 0;
    }

    default:
        return (int)&err_unsupported;
    }
}

/* ---- module lifecycle ---------------------------------------------- */

static uint32_t registered;

extern uint32_t hostfs_fsinfo_offset(void);   /* hostfs_entries.s */

int hostfs_init(void *ws)
{
    (void)ws;

    /* Both ends must be addressable: the doorbell in the low-peripheral
     * window, the request block in (identity-mapped) RAM. */
    if (os_validate((void *)VMCH_BASE, (void *)(VMCH_BASE + 16)) != 0) {
        return 0;                    /* no doorbell: stay dormant */
    }
    if (os_validate(req, (char *)req + sizeof(req)) != 0) {
        return 0;
    }
    if (vmch_reg(VMCH_MAGIC / 4) != VMCH_MAGIC_VALUE) {
        return 0;                    /* something else lives there */
    }
    if (!(vmch_reg(VMCH_FEATURES / 4) & VMCH_FEATURE_FS)) {
        return 0;                    /* present but no root: dormant */
    }

    uint32_t base = 0, privword = 0;
    if (os_module_18("HostFS", &base, &privword) != 0 || base == 0) {
        return (int)&err_nodev;
    }

    if (os_fscontrol(12, base, hostfs_fsinfo_offset(), privword) != 0) {
        return (int)&err_unsupported;
    }
    registered = 1;
    return 0;
}

int hostfs_final(unsigned fatal, void *ws)
{
    (void)fatal;
    (void)ws;
    if (registered) {
        /* FSControl 16 (RemoveFS), by name */
        os_fscontrol(16, (uint32_t) "HostFS#", 0, 0);
    }
    return 0;
}

/* The private word address, captured by the init veneer in
 * hostfs_head.s: */
__asm("\n.global hostfs_privword\nhostfs_privword: .word 0\n");

int hostfs_command_ping(const char *tail, int argc, void *ws)
{
    (void)tail;
    (void)argc;
    (void)ws;
    req[H_CMD] = C_PING;
    req[H_HND] = 0;
    req[H_ARGLEN] = 4;
    ((uint8_t *)&req[H_SIZE / 4])[0] = 'P';
    ((uint8_t *)&req[H_SIZE / 4])[1] = 'N';
    ((uint8_t *)&req[H_SIZE / 4])[2] = 'G';
    ((uint8_t *)&req[H_SIZE / 4])[3] = '!';
    if (vmch_go() != RC_OK || vmch_arglen() != 4) {
        return (int)&err_nodev;
    }
    return 0;
}
