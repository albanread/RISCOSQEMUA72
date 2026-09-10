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
#include "qemu/error-report.h"
#include <glib/gstdio.h>

/* ------------------------------------------------------------------ */
/* Guest RAM access: physical, little-endian, through the system AS   */

static uint32_t ld32(hwaddr a)
{
    return ldl_le_phys(&address_space_memory, a);
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

/* ------------------------------------------------------------------ */
/* Host paths                                                          */

/*
 * Translate the guest's RISC OS path into a host path under root and
 * validate it.  Returns a newly allocated host path, or NULL with *rc
 * set.  The guest path must start with '$' and consist of plain
 * components separated by '.'.
 */
static char *host_path(VMChannelState *s, const char *guest, int *rc)
{
    size_t len = strlen(guest);
    char *p, *out;

    if (!s->root) {
        *rc = VMCH_RC_NOROOT;
        return NULL;
    }
    if (len < 1 || guest[0] != '$' || len > VMCH_MAX_ARG) {
        *rc = VMCH_RC_BADPATH;
        return NULL;
    }

    p = g_strdup_printf("%s%s", s->root,
                        guest[1] ? G_DIR_SEPARATOR_S : "");
    out = p + strlen(s->root);

    /* walk the rest: each component until '.' must be a plain name */
    const char *c = guest + 1;
    while (*c == '.') {
        c++;                            /* leading dots are separators */
    }
    bool fresh = true;                  /* true: at component start */
    for (; *c; c++) {
        if (*c == '.') {
            if (fresh) {                /* ".." or empty component */
                g_free(p);
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
            *out++ = *c;
            fresh = false;
        }
    }
    if (fresh && out != p + strlen(s->root)) {
        g_free(p);                      /* trailing '.' */
        *rc = VMCH_RC_BADPATH;
        return NULL;
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

static uint32_t date_cs_for(const GStatBuf *st)
{
    /* centiseconds since 1900, the RISC OS 5-byte instant */
    return (uint32_t)((uint64_t)(st->st_mtime + 2208988800ULL) * 100);
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
            st32(base + VMCH_HDR_HANDLE, (uint32_t)h);
        }
        g_free(hp);
        g_free(path);
        break;
    }

    case VMCH_CMD_CLOSE: {
        int h = (int)ld32(base + VMCH_HDR_HANDLE);

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
        int h = (int)ld32(base + VMCH_HDR_HANDLE);
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
        int h = (int)ld32(base + VMCH_HDR_HANDLE);
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
        uint8_t resp[16];

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

            stl_le_p(resp + 0, size);
            stl_le_p(resp + 4, type);
            stl_le_p(resp + 8, attrs_for(&st));
            stl_le_p(resp + 12, date_cs_for(&st));
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
        uint8_t *buf = g_malloc(arglen ? arglen : 1);

        if (arglen > VMCH_MAX_ARG) {
            rc = VMCH_RC_BADPATH;
            g_free(buf);
            break;
        }
        block_read(base + VMCH_HDR_SIZE, buf, arglen);
        /* v0: console output is the emulator's stderr; the launcher
         * captures it.  Line-buffered by the C library already. */
        fwrite(buf, 1, arglen, stderr);
        fflush(stderr);
        g_free(buf);
        break;
    }

    case VMCH_CMD_TIME: {
        uint8_t resp[4];
        uint32_t cs = (uint32_t)((uint64_t)(time(NULL) + 2208988800ULL)
                                 * 100);

        stl_le_p(resp, cs);
        block_write(base + VMCH_HDR_SIZE, resp, sizeof(resp));
        st32(base + VMCH_HDR_ARGLEN, sizeof(resp));
        break;
    }

    default:
        rc = VMCH_RC_BADCMD;
        break;
    }

    st32(base + VMCH_HDR_RC, rc);
}

/* ------------------------------------------------------------------ */
/* MMIO                                                                */

static uint64_t vmchannel_read(void *opaque, hwaddr offset, unsigned size)
{
    VMChannelState *s = VMCHANNEL(opaque);

    switch (offset) {
    case VMCH_MAGIC:
        return VMCH_MAGIC_VALUE;
    case VMCH_VERSION:
        return VMCH_VERSION_VALUE;
    case VMCH_FEATURES:
        return VMCH_FEATURE_CONSOLE | VMCH_FEATURE_TIME
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
