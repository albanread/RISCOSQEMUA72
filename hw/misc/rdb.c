/*
 * rdb — the rdb.* monitor commands of DEBUGDESIGN.md, sprint 1.
 *
 * The debugging surface for RISC OS applications, backend only: the
 * window (sprint 2) renders these, an agent drives them today over
 * QMP's human-monitor-command.  Everything here rides primitives the
 * gdbstub already proved:
 *
 *   stop/cont   vm_stop/vm_resume, the runstate's own
 *   step        cpu_single_step + resume: TCG raises EXCP_DEBUG after
 *               one instruction, cpu_handle_guest_debug stops the
 *               machine with no debugger attached (accel/tcg/cpu-exec.c,
 *               system/cpus.c) — the exact stop a breakpoint makes
 *   bp/wp       cpu_breakpoint_insert/cpu_watchpoint_insert, the same
 *               TB-invalidation breakpoints gdbstub sets; invisible to
 *               the guest, no memory patching
 *   regs        cpu_dump_state, the monitor's own "info registers" path
 *   mem/dis     vmch_guest_rw — the walk; with the machine stopped the
 *               guest is quiescent, so the pages are as mapped
 *
 * Machine-wide semantics, labelled: RISC OS is single-scheduled, and
 * DEBUGDESIGN.md §2 records the application-space trap — breakpoints
 * arm against addresses, not tasks, until the guest agent exists.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "monitor/monitor.h"
#include "qobject/qdict.h"
#include "monitor/hmp.h"
#include "system/runstate.h"
#include "hw/core/cpu.h"
#include "hw/misc/vmchannel.h"
#include "exec/breakpoint.h"
#include "exec/watchpoint.h"
#include "exec/gdbstub.h"
#include "system/hw_accel.h"
#include "qemu/error-report.h"

#ifdef CONFIG_CAPSTONE
#include <capstone.h>
#endif

/* One CPU is the debugging target: core 0, the one RISC OS schedules.
 * DEBUGDESIGN.md §2 again -- the secondaries are parked. */
static CPUState *rdb_cpu(void)
{
    return first_cpu;
}

void hmp_rdb_stop(Monitor *mon, const QDict *qdict)
{
    vm_stop(RUN_STATE_DEBUG);
    monitor_printf(mon, "rdb: machine stopped\n");
}

void hmp_rdb_cont(Monitor *mon, const QDict *qdict)
{
    CPUState *cs = rdb_cpu();

    if (runstate_check(RUN_STATE_RUNNING)) {
        monitor_printf(mon, "rdb: already running\n");
        return;
    }
    cpu_single_step(cs, 0);        /* clear any step before running free */
    vm_resume(RUN_STATE_RUNNING);
    monitor_printf(mon, "rdb: running\n");
}

void hmp_rdb_step(Monitor *mon, const QDict *qdict)
{
    CPUState *cs = rdb_cpu();

    if (runstate_check(RUN_STATE_RUNNING)) {
        monitor_printf(mon, "rdb: stop first\n");
        return;
    }
    /* One instruction: TCG exits with EXCP_DEBUG after it and the
     * machine stops itself.  The command returns immediately; the stop
     * is asynchronous, the next rdb-regs shows where it landed. */
    cpu_single_step(cs, SSTEP_ENABLE);
    /* Running, not debug-resumed: vm_resume only starts the machine
     * for a live state, and the step's own EXCP_DEBUG is what stops
     * it again -- paused (debug), one instruction later. */
    vm_resume(RUN_STATE_RUNNING);
    monitor_printf(mon, "rdb: stepping\n");
}

void hmp_rdb_regs(Monitor *mon, const QDict *qdict)
{
    /* The monitor's own "info registers" dump: all banks, CPSR/SPSR,
     * decoded -- no second register formatter to drift from it. */
    cpu_dump_state(rdb_cpu(), NULL, CPU_DUMP_FPU | CPU_DUMP_VPU);
}

/* ------------------------------------------------------------------ */
/* Memory and disassembly, through the walk                            */

void hmp_rdb_mem(Monitor *mon, const QDict *qdict)
{
    uint64_t addr = qdict_get_int(qdict, "addr");
    uint32_t len = qdict_get_try_int(qdict, "len", 64);
    uint8_t buf[256];
    uint32_t n = len < sizeof(buf) ? len : sizeof(buf), i;

    if (!vmch_guest_rw(addr, buf, n, false)) {
        monitor_printf(mon, "rdb: %08llx does not translate\n",
                       (unsigned long long)addr);
        return;
    }
    for (i = 0; i < n; i += 16) {
        uint32_t row = n - i < 16 ? n - i : 16, j;

        monitor_printf(mon, "%08llx:", (unsigned long long)(addr + i));
        for (j = 0; j < 16; j++) {
            if (j % 4 == 0) {
                monitor_printf(mon, " ");
            }
            monitor_printf(mon, j < row ? "%02x" : "  ", buf[i + j]);
        }
        monitor_printf(mon, "  ");
        for (j = 0; j < row; j++) {
            monitor_printf(mon, "%c", buf[i + j] >= 32 && buf[i + j] < 127
                                  ? buf[i + j] : '.');
        }
        monitor_printf(mon, "\n");
    }
}

void hmp_rdb_memw(Monitor *mon, const QDict *qdict)
{
    uint64_t addr = qdict_get_int(qdict, "addr");
    uint32_t val = qdict_get_int(qdict, "val");
    uint32_t old = 0;

    if (!vmch_guest_rw(addr, &old, 4, false)) {
        monitor_printf(mon, "rdb: %08llx does not translate\n",
                       (unsigned long long)addr);
        return;
    }
    if (!vmch_guest_rw(addr, &val, 4, true)) {
        monitor_printf(mon, "rdb: write refused at %08llx\n",
                       (unsigned long long)addr);
        return;
    }
    monitor_printf(mon, "rdb: %08llx: %08x -> %08x\n",
                   (unsigned long long)addr, old, val);
}

void hmp_rdb_fill(Monitor *mon, const QDict *qdict)
{
    uint64_t addr = qdict_get_int(qdict, "addr");
    uint32_t len = qdict_get_int(qdict, "len");
    uint8_t val = qdict_get_int(qdict, "val");
    uint8_t buf[256];

    if (len > 4096) {
        monitor_printf(mon, "rdb: fill capped at 4096\n");
        len = 4096;
    }
    memset(buf, val, sizeof(buf));
    while (len) {
        uint32_t n = len < sizeof(buf) ? len : sizeof(buf);

        if (!vmch_guest_rw(addr, buf, n, true)) {
            monitor_printf(mon, "rdb: write refused at %08llx\n",
                           (unsigned long long)addr);
            return;
        }
        addr += n;
        len -= n;
    }
    monitor_printf(mon, "rdb: filled\n");
}

void hmp_rdb_dis(Monitor *mon, const QDict *qdict)
{
    uint64_t addr = qdict_get_int(qdict, "addr");
    uint32_t count = qdict_get_try_int(qdict, "count", 8);
    uint8_t code[4 * 32];
    uint32_t n = count < 32 ? count : 32;

    if (!vmch_guest_rw(addr, code, n * 4, false)) {
        monitor_printf(mon, "rdb: %08llx does not translate\n",
                       (unsigned long long)addr);
        return;
    }
#ifdef CONFIG_CAPSTONE
    {
        csh handle;
        cs_insn *insns;
        size_t done;

        if (cs_open(CS_ARCH_ARM, CS_MODE_ARM, &handle) != CS_ERR_OK) {
            monitor_printf(mon, "rdb: capstone refused to open\n");
            return;
        }
        done = cs_disasm(handle, code, n * 4, addr, n, &insns);
        if (done == 0) {
            monitor_printf(mon, "rdb: disassembly failed at %08llx\n",
                           (unsigned long long)addr);
        } else {
            size_t i;

            for (i = 0; i < done; i++) {
                uint32_t word;
                memcpy(&word, code + (insns[i].address - addr), 4);
                monitor_printf(mon, "%08llx: %08x  %s %s\n",
                               (unsigned long long)insns[i].address, word,
                               insns[i].mnemonic, insns[i].op_str);
            }
            cs_free(insns, done);
        }
        cs_close(&handle);
    }
#else
    monitor_printf(mon, "rdb: built without capstone\n");
#endif
}

/* ------------------------------------------------------------------ */
/* Registers: write side.  The read side is cpu_dump_state; the write
 * side needs the target's register file, which is per-target. */

void hmp_rdb_regw(Monitor *mon, const QDict *qdict)
{
    int idx = qdict_get_int(qdict, "idx");
    uint32_t val = qdict_get_int(qdict, "val");

    if (idx < 0 || idx > 15) {
        monitor_printf(mon, "rdb: register 0-15\n");
        return;
    }
    /* Through the gdbstub's generic register interface: register
     * numbers are the target's, and for ARM r0-r15 are exactly 0-15.
     * No target header, so this compiles in the generic build. */
    {
        CPUState *cs = rdb_cpu();
        GByteArray *buf = g_byte_array_new();
        uint32_t old = 0;

        cpu_synchronize_state(cs);
        if (gdb_read_register(cs, buf, idx) <= 0 || buf->len != 4) {
            monitor_printf(mon, "rdb: register %d not readable\n", idx);
        } else {
            memcpy(&old, buf->data, 4);
            if (gdb_write_register(cs, (uint8_t *)&val, idx) <= 0) {
                monitor_printf(mon, "rdb: register %d not writable\n", idx);
            } else {
                monitor_printf(mon, "rdb: r%d: %08x -> %08x\n", idx,
                               old, val);
            }
        }
        g_byte_array_unref(buf);
    }
}

/* ------------------------------------------------------------------ */
/* Breakpoints and watchpoints                                         */

#define RDB_MAX_BP 32

static struct {
    uint64_t addr;
    uint32_t len;
    bool watch;
    void *ref;
} rdb_bps[RDB_MAX_BP];

static int rdb_slot(uint64_t addr, bool watch)
{
    int i;

    for (i = 0; i < RDB_MAX_BP; i++) {
        if (rdb_bps[i].ref && rdb_bps[i].addr == addr
            && rdb_bps[i].watch == watch) {
            return i;
        }
    }
    return -1;
}

void hmp_rdb_bp(Monitor *mon, const QDict *qdict)
{
    uint64_t addr = qdict_get_int(qdict, "addr");
    CPUState *cs = rdb_cpu();
    CPUBreakpoint *bp = NULL;
    int i;

    if (rdb_slot(addr, false) >= 0) {
        monitor_printf(mon, "rdb: already breakpointed at %08llx\n",
                       (unsigned long long)addr);
        return;
    }
    for (i = 0; i < RDB_MAX_BP; i++) {
        if (!rdb_bps[i].ref) {
            break;
        }
    }
    if (i == RDB_MAX_BP) {
        monitor_printf(mon, "rdb: breakpoint table full\n");
        return;
    }
    cpu_breakpoint_insert(cs, addr, BP_GDB, &bp);
    rdb_bps[i].addr = addr;
    rdb_bps[i].watch = false;
    rdb_bps[i].ref = bp;
    monitor_printf(mon, "rdb: breakpoint at %08llx\n",
                   (unsigned long long)addr);
}

void hmp_rdb_bpc(Monitor *mon, const QDict *qdict)
{
    uint64_t addr = qdict_get_int(qdict, "addr");
    CPUState *cs = rdb_cpu();
    int i = rdb_slot(addr, false);

    if (i < 0) {
        monitor_printf(mon, "rdb: no breakpoint at %08llx\n",
                       (unsigned long long)addr);
        return;
    }
    cpu_breakpoint_remove(cs, addr, BP_GDB);
    memset(&rdb_bps[i], 0, sizeof(rdb_bps[i]));
    monitor_printf(mon, "rdb: breakpoint cleared at %08llx\n",
                   (unsigned long long)addr);
}

void hmp_rdb_wp(Monitor *mon, const QDict *qdict)
{
    uint64_t addr = qdict_get_int(qdict, "addr");
    uint32_t len = qdict_get_try_int(qdict, "len", 4);
    const char *kind = qdict_get_try_str(qdict, "kind");
    CPUState *cs = rdb_cpu();
    CPUWatchpoint *wp = NULL;
    int flags = BP_MEM_ACCESS, i;

    if (kind && strcmp(kind, "r") == 0) {
        flags = BP_MEM_READ;
    } else if (kind && strcmp(kind, "w") == 0) {
        flags = BP_MEM_WRITE;
    }
    if (rdb_slot(addr, true) >= 0) {
        monitor_printf(mon, "rdb: already watchpointed at %08llx\n",
                       (unsigned long long)addr);
        return;
    }
    for (i = 0; i < RDB_MAX_BP; i++) {
        if (!rdb_bps[i].ref) {
            break;
        }
    }
    if (i == RDB_MAX_BP) {
        monitor_printf(mon, "rdb: breakpoint table full\n");
        return;
    }
    if (cpu_watchpoint_insert(cs, addr, len, flags, &wp)) {
        monitor_printf(mon, "rdb: watchpoint refused at %08llx\n",
                       (unsigned long long)addr);
        return;
    }
    rdb_bps[i].addr = addr;
    rdb_bps[i].len = len;
    rdb_bps[i].watch = true;
    rdb_bps[i].ref = wp;
    monitor_printf(mon, "rdb: watchpoint at %08llx len %u\n",
                   (unsigned long long)addr, len);
}

void hmp_rdb_list(Monitor *mon, const QDict *qdict)
{
    int i;

    for (i = 0; i < RDB_MAX_BP; i++) {
        if (rdb_bps[i].ref) {
            monitor_printf(mon, "%08llx  %s\n",
                           (unsigned long long)rdb_bps[i].addr,
                           rdb_bps[i].watch ? "watchpoint" : "breakpoint");
        }
    }
}
