# TCG profiling — reporting on RISC OS guest instruction usage

Can we profile what the guest executes on the emulated A72?  Yes, with
no emulator changes at all: the TCG plugin interface, enabled in every
build this tree ships (the launchers' `configure --enable-plugins`;
`CONFIG_PLUGIN` verified in the build), already sees every translated
block, every instruction and every memory access.  What follows was
established by running this tree's own build against a bare-metal
AArch32 loop guest — the command lines and the sample output are real,
not projected.

---

## 1. The working invocation, and the three things that silently break it

    ./build/qemu-system-aarch64 \
        -M raspi4b -cpu cortex-a72,aarch64=off \
        -bios guest.bin -display none -serial null \
        -d plugin -D plugin.log \
        -plugin ./build/contrib/plugins/libips.dylib \
        -plugin ./build/contrib/plugins/libhotblocks.dylib \
        -plugin ./build/contrib/plugins/libhowvec.dylib

1. **`-d plugin` is not optional.**  Plugin reports go through
    `qemu_log_mask(CPU_LOG_PLUGIN, ...)` (`plugins/api.c:399`): without
    `-d plugin` — and a `-D file` to receive them — every contrib
    plugin's report is *silently discarded*.  This cost an hour to
    find and will cost another unless it is written down.
2. **macOS names them `.dylib`**, Windows and Linux `.so` — the
    launchers take whatever path they are given, so spell it for the
    host you are on.
3. **Exit cleanly, or at least politely.**  The reports are written by
    `atexit` handlers; a QMP `quit` runs them, and QEMU's own SIGTERM
    handling does too (verified: a `timeout` kill still produced the
    full report) — a `kill -9` will not.

For a bare-metal guest there is a fourth: **`-bios`, not `-kernel`.**
    A raw twenty-byte loop is rejected by the Linux boot protocol
    (`-kernel` left every core parked in the secondary stub and nothing
    else happened); `-bios` loads it at 0x80000, core 0 runs the zeros
    sled into it, and the secondaries sit in the fork's own AArch32
    spin stub at 0x300 (`hw/arm/raspi.c:188`, `write_smpboot32` —
    visible in every profile as a handful of `yield`-loop blocks, a
    useful landmark and a known overhead: three cores spinning).

## 2. What the sample run reports

A three-instruction `add/cmp/bne` loop, four seconds, one machine:

    Instruction Classes:                       (howvec)
    Class: Data Proc Reg      2,994,933,775 hits
    Class: Unclassified       5,989,867,554 hits

    pc, tcount, icount, ecount                (hotblocks)
    0x0000000000080004, 2, 3, 2,994,933,771

hotblocks names the loop's block exactly: two translations, three
instructions, three billion executions.  howvec's split is the honest
part: its classifier table is **aarch64-only**
(`contrib/plugins/howvec.c:64,149`), so on our AArch32 guest the
`add` classifies and the `cmp`/branch fall into *Unclassified*.  It
still counts everything — per-opcode, with disassembly — but the class
summary is wrong until there is an A32 table (§4).

Throughput, for calibration only: ~750M guest instructions per second
on this M4, single scheduling core, three spinning.  TCG counts
*executed instructions*, not cycles — there is no cycle model — which
is exactly "instruction usage" and exactly not "time".  Where time
matters, correlate hot blocks with what the block *does*.

## 3. What each existing plugin answers for RISC OS

| Plugin | Answers | Cost |
| --- | --- | --- |
| `hotblocks` | where execution concentrates — with `*Modules` output in hand, PC ranges attribute to modules, the RISC OS-specific payoff | inline counters, cheap |
| `ips` | instructions/second — boot-to-desktop and idle-desktop baselines, machine-to-machine sanity | negligible |
| `howvec` | instruction mix — with the A32 caveat above | cheap |
| `hotpages`, `cache` | guest memory behaviour, cache hit modelling | moderate |
| `execlog` | a full execution trace — ROM boot-path archaeology, one short window | very heavy |
| `stoptrigger` | bounded runs — stop the farm at N instructions, not T seconds | negligible |
| `drcov` | coverage of a boot or a workload, for the ROM and for our own modules | moderate |

Plus two non-plugin tools: `-perfmap` and `-jitdump` exist in this QEMU
(`system/vl.c:3148`) and map TCG code into host `perf`, for when the
question is host-side cost rather than guest-side usage; and
`-accel tcg,one-insn-per-tb=on` exists for calibration runs where
per-instruction attribution must be exact, at heavy cost.

## 4. The one thing worth building: an A32 classifier

howvec with an `aarch32_insn_classes[]` table: the A32 encoding gives
the classes nearly for free — cond field, bits 27-20, bits 7-4 select
data-processing / loads-stores / branches / coprocessor / VFP / NEON /
status access, and the table format (mask, pattern, count) is already
howvec's.  Perhaps a hundred and fifty lines of plugin, no emulator
change, and it answers the question the NEON document left open: what
share of a real RISC OS workload's instructions is SIMD, is
floating point, is branches — measured, per application, on the farm.
Until it exists, per-opcode howvec output plus a script over the
disassembly is the honest fallback.

## 5. The wiring, shipped with this document

- `run.py --plugin LIB[,ARG=V...]` — repeatable; the `-d plugin` gate
  and `plugin.log` beside the emulator come with it.
- `RISCOS_PLUGINS` in the environment for `farm.py` (report lands in
  the instance's `logs/plugin.log`) and for both shell launchers
  (`${TMPDIR}/riscos-plugin.log`).

So the farm measures, whole:

    RISCOS_PLUGINS="build/contrib/plugins/libhotblocks.dylib" \
        python riscos-pi4/tools/farm.py up alpha

## 6. Measurement ladder

1. Boot to desktop, `hotblocks` top twenty, correlated against
   `*Modules` — which modules the boot spends its instructions in.
2. Idle desktop, `ips` — the machine's resting instruction rate, the
   baseline every other number divides by.
3. A `redrawbench` session under `hotblocks` and `hotpages` — the
   heaviest repeatable desktop workload the tools already drive.
4. NEON/VFP share, once §4's classifier exists — the measurement
   `neon-acceleration.md` deferred.
5. Adversarial detail on a one-instruction-per-TB run of a short
   window, when exact attribution matters more than speed.

Not on the list: cycle counts (TCG has no cycle model), and anything
requiring emulator changes — the plugins and `-d` already reach
everything the questions need.
