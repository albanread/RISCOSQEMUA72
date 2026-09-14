/*
 * HostNet — the guest's sockets, served by the host.
 *
 * Sprint 0: the transport, and every socket SWI answered "not supported
 * yet".  That sounds like a stub and is not: it exercises the whole path
 * — module, veneer, register frame, doorbell, MMU walk, result, RISC OS
 * error — for all thirty-five calls, so Sprint 1 only has to fill in
 * cases.  See ROS_PRIVATE design/HOSTNET.md §3 for the contract this
 * implements and HOSTNET-SPRINTS.md for what lands when.
 *
 * Why the host must never block here: this runs synchronously inside the
 * vCPU's MMIO write, under the BQL.  A blocking connect() or recv() would
 * freeze the display, the monitor and the machine.  Every host socket is
 * non-blocking and every readiness question is a zero-timeout poll; a
 * guest that wanted to block gets EWOULDBLOCK and spins its own wait
 * loop, which is exactly what it does today (tsleep() in the Internet
 * module's lib/c/unixenv spins on UpCall 6 and Portable_Idle — there is
 * no scheduler to sleep on).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/hostnet.h"
#include "hw/misc/vmchannel.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

/* ---- the request block ---------------------------------------------- */

/*
 * The block itself is at a guest logical address, like everything else in
 * a request, so it is reached the same way.  ld/st of one word keeps the
 * call sites readable; a failed access leaves the value zero, which the
 * caller notices through the rc it ends up writing.
 */
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

/* ---- commands -------------------------------------------------------- */

static void hn_do_ping(HostNetState *s, uint64_t base)
{
    hn_st32(base + HN_HDR_RESULT, HN_MAGIC_VALUE);
    hn_st32(base + HN_HDR_RC, HN_RC_OK);
}

/*
 * One socket SWI.  R0..R7 are in the block at HN_HDR_REGS, exactly as the
 * application passed them — the Internet module casts _kernel_swi_regs
 * straight onto each BSD *_args struct, so R0..Rn are the C arguments in
 * order (riscos/c/socket_swi).  The result goes back in HN_HDR_RESULT and
 * an ordinary networking failure in HN_HDR_ERRNO; rc is for the transport
 * alone.
 */
static void hn_do_swi(HostNetState *s, uint64_t base)
{
    uint32_t swi = hn_ld32(base + HN_HDR_SWI);
    uint32_t regs[8];

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

    switch (swi) {
    /*
     * Socket_Version answers from here rather than the guest, so that
     * "which HostNet am I talking to" is a question about the host, which
     * is the half that will keep changing.
     */
    case HN_SWI_VERSION:
        hn_st32(base + HN_HDR_RESULT, HN_VERSION_VALUE);
        hn_st32(base + HN_HDR_ERRNO, 0);
        break;

    /*
     * Socket_Gettsize is how a program asks how many descriptors exist,
     * and it must agree with the fd_set the guest builds for Select.
     * Answering it in Sprint 0 costs nothing and keeps that invariant in
     * one place from the start.
     */
    case HN_SWI_GETTSIZE:
        hn_st32(base + HN_HDR_RESULT, HN_MAX_SOCKETS);
        hn_st32(base + HN_HDR_ERRNO, 0);
        break;

    /*
     * Everything else: EOPNOTSUPP (45 in 4.4BSD, which is what RISC OS
     * uses).  The guest turns it into error &20E00+45, message
     * "OPNOTSUPP", V set — a real RISC OS error a real program can
     * handle, which is the whole point of Sprint 0's test: NetSurf must
     * report a failed connection, not hang and not throw an error box.
     */
    default:
        hn_st32(base + HN_HDR_RESULT, (uint32_t)-1);
        hn_st32(base + HN_HDR_ERRNO, 45);
        break;
    }
    hn_st32(base + HN_HDR_RC, HN_RC_OK);
}

/* ---- the doorbell ---------------------------------------------------- */

static void hn_ring(HostNetState *s, uint64_t base)
{
    uint32_t cmd = hn_ld32(base + HN_HDR_CMD);

    s->seq = hn_ld32(base + HN_HDR_SEQ);
    hn_st32(base + HN_HDR_ERRNO, 0);

    switch (cmd) {
    case HN_CMD_PING:
        hn_do_ping(s, base);
        break;
    case HN_CMD_SWI:
        hn_do_swi(s, base);
        break;
    default:
        hn_st32(base + HN_HDR_RC, HN_RC_BADCMD);
        break;
    }
}

static uint64_t hostnet_read(void *opaque, hwaddr offset, unsigned size)
{
    HostNetState *s = HOSTNET(opaque);

    switch (offset) {
    case HN_MAGIC:
        return HN_MAGIC_VALUE;
    case HN_VERSION:
        return HN_VERSION_VALUE;
    case HN_FEATURES:
        return s->enabled ? HN_FEATURE_SOCKETS : 0;
    default:
        return 0;
    }
}

static void hostnet_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    HostNetState *s = HOSTNET(opaque);

    if (offset == HN_CMD) {
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
     * A reset abandons every socket the guest held.  There is nothing to
     * close yet in Sprint 0; from Sprint 1 this is where they go, and it
     * matters — a machine reset must not leak host sockets.
     */
    for (i = 0; i < HN_MAX_SOCKETS; i++) {
        s->fds[i] = -1;
    }
    s->seq = 0;
}

static void hostnet_realize(DeviceState *dev, Error **errp)
{
    HostNetState *s = HOSTNET(dev);

    memory_region_init_io(&s->mr, OBJECT(s), &hostnet_ops, s,
                          "hostnet", HN_REGION_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mr);
    hostnet_reset(dev);
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
