# A Raspberry Pi 4 personality for QEMU — investigation, design, sprints

Status: **Sprint 2 done — RISC OS 5.30 boots to its supervisor prompt on an
emulated Cortex-A72 in 32-bit mode, with a framebuffer on screen.** Nine fixes
on a QEMU branch. See `first-boot.png`. Everything in
§1–§3 was run on this machine (i7-12700, Windows 11) on 2026-09-09 and is
reproducible from the commands given. §4–§9 is the design; §10 is the log.

Counterpart to `rpcemu/A72-MODE-PLAN.md`, which extends the RPCEmu *interpreter*
to decode A72 instructions on an emulated RiscPC. This document is about the
other half of the problem: emulating the *machine*, so RISC OS 5 itself runs on
a Cortex-A72 in 32-bit mode rather than on a StrongARM pretending.

## 0. Summary

- **A Cortex-A72 in AArch32 already works on stock QEMU 11.1.0.**
  `qemu-system-aarch64 -M raspi4b -cpu cortex-a72,aarch64=off` boots the real
  ROOL Pi ROM in ARM state on the real target CPU, with the BCM2711 peripheral
  base at `0xFE000000`. This contradicts the common claim that `raspi4b` is
  64-bit only, and it means **the CPU side of a Pi 4 sandbox costs nothing**.
- **RISC OS 5.30 hangs early in the HAL**, in `HAL_BCM2835`'s USB power-up: it
  posts to VideoCore mailbox **channel 0** (power management) and spins on
  `MAIL0_STATUS` forever, because QEMU wires only channels 1 (framebuffer) and
  8 (property). Verified from the trace and the disassembly (§2.2).
- **With that fixed, RISC OS boots into its own kernel** — MMU on, ROM mapped at
  `0xFC000000`. Three more blockers lie behind it, each found by measurement and
  each now fixed: the secondary-core boot stub, byte access to the I2C FIFO, and
  an empty DDC bus. See §2.3 and the sprint log in §10.
- **Speed is not the problem.** QEMU TCG measured **~2000 MIPS** on a
  register-only loop and **~500 MIPS** on a load/store loop that crosses pages,
  against **205 MIPS** for the RPCEmu interpreter you debug on today and
  **~920 MIPS** for the RPCEmu amd64 dynarec (§3). QEMU is in the same league as
  the dynarec and several times the interpreter, on the *correct ISA*.
- **Therefore: do not write a new emulator.** Write the Pi 4 personality for
  QEMU. Sprints in §7. A hand-written interpreter+JIT of our own is §8, kept as
  the fallback it should be, with the conditions that would justify it.

## 1. How to reproduce everything here

QEMU 11.1.0 (`v11.1.0-12130-ge470268ff4`) from qemu.weilnetz.de, SHA-512 verified
against the published checksum, installed to
`C:\Users\alban\AppData\Local\Programs\qemu` (no admin needed, NSIS `/S /D=`).

ROM: ROOL "RPi ROM stable" 5.30, `BCM2835.5.30.zip` (5,662,584 bytes) from
<https://www.riscosopen.org/content/downloads/raspberry-pi>, containing
`RISCOS.IMG` (5 MiB) plus the Broadcom firmware. The beta 5.31 ROM
(`BCM2835Dev.5.31.zip`) was fetched too. **One ROM covers BCM2835/2836/2837/2838**
— Pi 1 through Pi 4 — the SoC is detected at runtime. That is why a Pi 2 machine
and a Pi 4 machine run the same binary and diverge only in which HAL paths they
take.

    qemu-system-aarch64 -M raspi4b -cpu cortex-a72,aarch64=off \
        -kernel RISCOS.IMG -display none -serial null \
        -qmp tcp:127.0.0.1:4455,server,nowait \
        -d unimp,guest_errors -trace enable=bcm2835_mbox* -D q.log

Driving it: QMP over TCP. `human-monitor-command` gives `info registers`,
`x/16i <addr>` and `xp/16wx <addr>`; `screendump` writes a PNG (pass
`format:"png"` — the default is still PPM).

## 2. What actually happens

### 2.1 The CPU: `-cpu` is not ignored on raspi machines

`-M raspi4b` hard-codes `cortex-a72` in the SoC, so it is widely believed that
`-cpu` does nothing there. Empirically the *property* still lands (QEMU records
`-cpu` features as globals for that CPU type, applied at instance-init):

| Command | First instructions executed |
| --- | --- |
| `-M raspi4b -kernel RISCOS.IMG` | AArch64 at `0x80000` — the ROM's ARM32 vector table decoded as A64 garbage, then `udf`. Dead. |
| `-M raspi4b -cpu cortex-a72,aarch64=off -kernel RISCOS.IMG` | **AArch32 from `0x0` into the ROM.** Reaches the HAL. |
| `-M raspi3b -kernel RISCOS.IMG` | AArch64 at `0x80000`. Dead. |
| `-M raspi2b -kernel RISCOS.IMG` (qemu-system-arm) | AArch32, ROM loaded at `0x10000`, reaches the HAL. |

The bare-metal benchmark of §3 confirms the BCM2711 peripheral base
(`0xFE000000`) is live in the 32-bit raspi4b case: its PL011 at `0xFE201000` and
system timer at `0xFE003004` both work.

### 2.2 Blocker #1 — mailbox channel 0, the USB power-up

Both machines stop in the same place. `info registers` is stable across 40 s:

    raspi2b:  R03=3f00b800  R04=3f000000  R13=00010ff0  R15=00016764  svc32
    raspi4b:  R03=fe00b800  R04=fc000000  R13=00010ff0  R15=00016764  svc32

`R3` is the mailbox at peripheral base + `0xB800` — and note it differs between
the two runs. The *same ROM binary* picked the BCM2836 base (`0x3F000000`) on
raspi2b and the BCM2711 one (`0xFE000000`) on raspi4b, so the SoC detection
works and we really are exercising the Pi 4 code paths. The code at the PC:

    0x00016760:  str  r2, [r3, #0xa0]      @ MAIL1_WRITE
    0x00016764:  ldr  r0, [r3, #0x98]      @ MAIL0_STATUS
    0x00016768:  tst  r0, #0x40000000      @ EMPTY?
    0x0001676c:  bne  #0x16764             @ spin until a reply arrives

and the caller, at ROM offset `0x40cc`:

    0x000140c4:  mov  r1, #0
    0x000140c8:  mov  r0, #0x80            @ channel 0, value 8 = power on USB HCD
    0x000140cc:  bl   #0x16684

The mailbox trace shows the property channel working fine — a full tag list
(`0x10003` MAC, `0x10004` serial, `0x10005` ARM memory, `0x10006` VC memory,
`0x10001` board model, `0x10002` board revision, `0x60001` DMA channels,
`0x40002`, `0x30002` get clock, `0x38002` set clock) is answered and read back —
and then:

    bcm2835_mbox_write mbox write sz:4 addr:0xa0 data:0x80
    bcm2835_mbox_read  mbox read  sz:4 addr:0x98 data:0x40000000   (×3.9M and counting)

Nobody answers channel 0. RISC OS's messaging code has no timeout, so it spins
forever. Linux never hits this because it powers devices with property tag
`0x28001` instead of the legacy channel-0 interface.

This is a ~30-line fix in QEMU: a channel-0 peer that echoes the request back.

### 2.3 The blocker chain behind it

Each of these was found the same way: run, trace every MMIO access, histogram
by device and address, and look at what the guest touched last. All four are
fixed on the `riscos-pi4` branch (§10).

| # | What stopped it | How it showed up | Fix |
| --- | --- | --- | --- |
| 1 | **Mailbox channel 0 has no peer.** RISC OS powers up the USB host controller through the legacy channel-0 interface; QEMU wires only channels 1 and 8. | `PC=0x00016764` forever; 3.9M reads of `MAIL0_STATUS` in 12 s | new `bcm2835-mbox-power` device |
| 2 | **The secondary-core boot stub is AArch64.** `setup_boot()` picks it by SoC generation, so a 32-bit guest on raspi3b/raspi4b gets a 64-bit spin stub, executed as ARM garbage. RISC OS starts its secondaries (write entry to each core's mailbox 3) and waits on its own mailbox 1. | 298,606 of the last 300,000 MMIO events were `read cpu0 0xff8000c4` | AArch32 stub keyed off `ARM_FEATURE_AARCH64`, with the BCM2711 local base |
| 3 | **`bcm2835_i2c` rejects byte access.** The BSC FIFO is a byte port and the HAL writes it with `STRB`; a 4-byte-only `MemoryRegionOps` turns that into a data abort. | last instruction before `0xffff0010` (the abort vector) was `strb r1, [r4, #0x10]` | `valid` and `impl` widened to 1..4 bytes |
| 4 | **Nothing on the I2C buses.** RISC OS muxes GPIO 0-3 to ALT0 and reads the monitor's EDID from slave `0x50`; an empty bus NACKs. | status register stuck at `0x157`, `S.ERR` set | `i2c-ddc` on BSC0 (`0x157` → `0x57`) |

Blocker 2 is the interesting one, because it is not RISC OS-specific at all:
**any** 32-bit kernel booted on `raspi3b` or `raspi4b` gets a 64-bit secondary
stub. It is a plain QEMU bug that nobody hit because nobody boots 32-bit guests
on those machines.

Where it stands after all four: the guest reaches display setup, completes the
EDID transfer without error, and then spins on the BSC status register. `S`
reads `0x57` — `TA` and `DONE` both set — while the HAL's `transfer_read_write`
loops until `TA` clears. QEMU only clears `TA` in `finish_transfer()`, when
`DLEN` counts down to zero. That is the next thing to chase, and it is a
state-machine mismatch in `bcm2835_i2c`, not a missing device.

### 2.4 The earlier reading of this, which was wrong

Before the trace work above, the evidence was a nopped-out ROM stopping after
three unanswered property tags, and I read that as display bring-up:

Patching that one `bl` to a `nop` in a scratch copy of the ROM (offset `0x40cc`,
`eb00096c` → `e1a00000`) gets RISC OS into its own kernel:

    R11=ffff0590  R12=fc0457c4  R13=fa207f6c  R15=fc0176ac   svc32

`0xFC000000` is the RISC OS ROM mapping and `0xFA20xxxx` its kernel workspace —
the MMU is up and the kernel is running, which is past where the ROOL forum
reports of 2015–2016 stopped (`ModuleInit`). It then issues:

| Tag | Meaning (`include/hw/arm/raspberrypi-fw-defs.h`) | QEMU's answer |
| --- | --- | --- |
| `0x00048020` | `FRAMEBUFFER_SET_GPIOVIRTBUF` | `out_sz:0` — nothing |
| `0x00040010` | `FRAMEBUFFER_GET_GPIOVIRTBUF` | `out_sz:0` — nothing |
| `0x00030002` | `GET_CLOCK_RATE` | answered |
| `0x00038041` | `SET_GPIO_STATE` | `out_sz:0` — nothing |

The GPIO ones are the activity LED, which on Pi 3 and later hangs off the
VideoCore's GPIO expander rather than the SoC's own pins — so an empty answer
there is unlikely on its own to stop a boot. What we can say is only what was
observed: after those calls the kernel cycles between four PCs and the
framebuffer stays black (640×480, all zero). Published work on the raspi2b path
attributes the display hang to mailbox **channel 3 (VCHIQ)** having no peer,
which `BCMVideo` waits on without a timeout.

**That reading was wrong.** The GPIO tags stop nothing, and VCHIQ is never used
at all — across a full boot the guest posts to channels 0 and 8 and no others.
The lesson is the method in §2.3: trace what the guest actually touches, rather
than inferring from what it last asked for.

### 2.5 Incidental findings

- `snapshot.load` on the existing `rpcemu/win32/RPCEmu/boot.snap` kills the
  headless RPCEmu: `rpclog.txt` ends `FATAL: Bad PC fc19e000 fc19e000`. Cold boot
  is fine. `compiler/tools/run_on_emu.py` prefers that snapshot, so it is broken
  until the snapshot is regenerated by the current build.
- QEMU's `screendump` defaults to PPM even when the filename ends `.png`.

## 3. Speed, measured on this host

All figures are emulated ARM instructions per second on the i7-12700.

| Emulator | Workload | Rate |
| --- | --- | --- |
| RPCEmu interpreter (`rpcemu-headless.exe`) | RISC OS 5.30, boot then idle desktop | **205 M/s** |
| RPCEmu amd64 dynarec (`RPCEmu-Recompiler.exe`) | same, idle desktop | **~920 M/s** (avg 924, peak 1024) |
| QEMU TCG, `raspi2b` (Cortex-A7) | 2-instruction register loop | **2016 M/s** |
| QEMU TCG, `raspi4b` + `aarch64=off` (A72) | 2-instruction register loop | **2071 M/s** |
| QEMU TCG, `raspi2b` | 6-instruction loop, ldr+str, 64-byte stride over 4 MB | **465 M/s** |
| QEMU TCG, `raspi4b` + `aarch64=off` | same | **579 M/s** |
| Real Pi 4 A72 @ 1.5 GHz | — | ~1000–1800 M/s (estimate, not measured) |

Method: the RPCEmu numbers come from the same `inscount` counter in both builds
(`qt5/rpc-qt5.cpp:592`, `qt5/main_window.cpp:1529`) — headless via
`status.instructions` over 4 s windows, dynarec read from the window title. The
QEMU numbers come from a 100-byte bare-metal ARM32 blob built with the installed
clang, timed with the BCM system timer and printed over the PL011.

Read it honestly: the register-loop figure is an upper bound (TCG chains that
into a tight host loop with no memory traffic); the load/store figure is the
realistic one, and it is a synthetic loop rather than real OS code. The
like-for-like comparison — QEMU running the RISC OS desktop — is a Sprint 6
deliverable. But the conclusion already holds at this resolution: **QEMU TCG is
roughly RPCEmu-dynarec class and several times the interpreter, on the right
instruction set.** Speed is not the reason to build something new.

## 4. Prior art (verify before relying on it)

- **`anyvm-org/riscos-builder`** and **`vmactions/riscos-vm`** (both MIT, created
  Aug 2026, last pushed 2026-09-02 and 2026-09-09 — existence, licence and dates
  confirmed via the GitHub API). Reported to boot the stock ROOL 5.30 Pi ROM
  under **QEMU 10.2.3 + a 71 KB patch**, `-M raspi2b`, ROM via `-bios`, SD card
  as qcow2, display over VNC. Their patch reportedly adds a mailbox channel-0
  power device and a channel-3 VCHIQ peer, allows byte access on `bcm2835_ic` and
  the I2C FIFO, fixes three `hcd-dwc2.c` bugs and a `bcm2835_dma` `xlen -= 4`
  unsigned wrap, and adds an SMSC95xx USB NIC. **Their channel-0 diagnosis
  matches exactly what §2.2 found independently, which is the strongest evidence
  either way.** Zero stars, one month old, no third-party reproduction — treat as
  a strong lead, not a dependency, and read the patch before adopting it. The
  repo licence is MIT but a QEMU patch is derivative of GPL-2.0 QEMU.
- **`slp/qemu-riscos`** (Sergio López, Dec 2013) booted the RISC OS Pi desktop on
  Gregory Estrade's out-of-tree BCM2835 branch — with a patched QEMU *and* a
  patched ROM.
- ROOL forum record: hangs at `ModuleInit` (Feugey, QEMU 2.6, 2016), "without a
  working timer implementation RISC OS got no further" (Markettos, ~2021), "locks
  up when enabling USB power … infinite loop" (tymaja, 2025). All consistent with
  §2.2/§2.3.
- **Not** relevant despite the name collisions: RISC OS Pyromaniac (a Python
  reimplementation on Unicorn — does not boot a ROM), Timothy Baldwin's Linux port
  and RISC OS Direct for Pi 5 (both `qemu-arm` **user**-mode SWI interception,
  and their `RISCOS.IMG` is an ELF, not the Pi ROM), RPCEmu / VirtualAcorn / ArcEm
  (RiscPC and Archimedes, ARMv3/v4).
- Theo Markettos (ROOL, 2023) argues against a QEMU fork and for a RISC OS HAL
  targeting QEMU's `virt` machine with VirtIO. Nobody has written it. Worth
  knowing as the position of the people who maintain the OS.

## 5. Why a Pi 4 personality and not just `raspi2b`

`raspi2b` is Cortex-A7: **ARMv7-A**. The compiler targets `armv8a-none-eabi`
`-mcpu=cortex-a72` (`compiler/src/main.rs:29`), and ARMv8-A in AArch32 adds
instructions an A7 will refuse — `CRC32`/`CRC32C`, the load-acquire/store-release
family (`LDA`/`STL`/`LDAEX`/`STLEX`), `VMAXNM`/`VSEL`/`VCVTA` and friends. A
sandbox that cannot execute what `roscc` emits is the problem we already have
with RPCEmu; moving it from ARMv4 to ARMv7 does not fix it.

`raspi4b` with `aarch64=off` gives the actual target: A72, ARMv8-A A32, VFPv4,
NEON, CRC32, BCM2711 memory map. §2.1 shows it running today.

## 6. The design

### 6.1 Shape

A branch of QEMU carrying a machine `raspi4b32` — `raspi4b` with the boot CPU in
AArch32 by construction rather than by a `-cpu` incantation, plus the device work
below. Keeping it as a *separate machine name* rather than mutating `raspi4b`
keeps the diff honest and upstreamable in pieces: the generic bug fixes go
upstream, the RISC OS-shaped ones live on the branch until they are defensible.

Everything QEMU already has is kept: GIC-400, CPRMAN, system timer, DMA, GPIO,
PL011 + mini-UART, RNG, framebuffer, EMMC2/SDHCI, thermal, SPI, I2C, DWC2 + MPHI.
The `raspi4b` gaps are PCIe, GENET and PWM — see §6.3.

### 6.2 Device work items, in dependency order

| # | Item | Evidence | Size |
| --- | --- | --- | --- |
| D1 | **Mailbox channel 0** (VideoCore power management) — a peer that accepts a device bitmask and replies with it, i.e. "powered". | §2.2, first-hand | ~30 lines |
| D2 | **Byte access on `bcm2835_ic`** — RISC OS dispatches IRQs with `LDRB`; a word-only `MemoryRegionOps` turns every interrupt into a data abort. | prior art; confirm on our branch | `min_access_size = 1` |
| D3 | **Byte access on the I2C FIFO** — same shape. | prior art | trivial |
| D4 | **Mailbox channel 3 (VCHIQ) peer** — enough handshake that `BCMVideo` completes and a display is set up. | §2.3 + prior art | days |
| D5 | **Missing property tags** — `0x48020` set display num, `0x40010`, `0x38041` set GPIO state, plus `0x10001` board model and `0x10004` board serial, which currently log NYI. Board model matters: the HAL branches on it. | §2.3, first-hand | ~1 day |
| D6 | **`hcd-dwc2` fixes** (frame counter, bus start on port-enable, `GINTSTS.HCHINT` re-evaluation) — only needed once we want USB. | prior art | days |
| D7 | **`bcm2835_dma` `xlen -= 4` unsigned wrap** — reportedly fixed upstream (Jul 2026); confirm it is in 11.1. | prior art | check only |

### 6.3 What we deliberately do not emulate

- **VL805 xHCI over PCIe.** On real Pi 4 hardware RISC OS drives USB through the
  VL805 on the PCIe bus; QEMU models neither the PCIe root complex nor the VL805,
  and adding both is a large project for a peripheral we want mainly as a
  keyboard. The Pi 4's DWC2 on the USB-C port is the cheaper road if we ever need
  real USB (D6) — but see §6.4: for an agent-driven machine we do not want a
  keyboard at all.
- **GENET Ethernet.** Deferred. Not on the critical path for compiler testing;
  when it is wanted, SMSC95xx-over-DWC2 is far cheaper than GENET, because RISC
  OS's EtherUSB already binds `smsc95xx`.
- **PWM, audio, camera, V3D.** Not needed.

### 6.4 Input, and the agent channel

The known limitation of the existing raspi2b work is *there is no keyboard*: the
BCM2835 ROM ships no USB keyboard driver, prints "No keyboard present -
autobooting", and reports every HID device as `&24425355` (`"USB$"`). That is a
RISC OS limitation, not a QEMU one, and no amount of xHCI work fixes it.

This matters less than it sounds, because we already decided (in
`rpcemu/RPCEMU-AGENT.md`) that "the unit of interaction is a request/response over
a socket, not a keystroke and a screenshot". The Pi 4 personality should skip
synthetic typing entirely and provide the same five things that document
identified — command, observation, control, determinism, cheap iteration:

| Need | On QEMU | Cost |
| --- | --- | --- |
| Registers, memory, breakpoints, watchpoints, exact single-step | **gdbstub**, already there (`-gdb tcp:…`, `-S`); `-accel tcg,one-insn-per-tb=on` verified accepted on this build | wrapper only |
| Halt / continue / machine state | **QMP** — `stop`, `cont`, `query-status`, `human-monitor-command` | wrapper only |
| Screenshots, frame capture | QMP `screendump` (pass `format:"png"`) | wrapper only |
| Snapshots | `savevm`/`loadvm` against a qcow2 SD image | wrapper only |
| Determinism (the half of D3 never finished for RPCEmu) | **`-icount shift=N`**, verified accepted; record/replay on top | free, and better than what we have |
| Instruction trace and counting | TCG plugins — **the Windows binary ships none**, so this needs our own build | Sprint 1 |
| Command + console capture | **a portal device** (§6.5) | Sprint 4 |
| Symbols, heap accounting, RISC OS-aware analysis | host-side, ported from `rpcemu/src/src/debug/` | Sprint 6 |

So the JSON-RPC surface we already designed — `regs.read`, `mem.read`, `bp.set`,
`wp.set`, `trace.start`, `snapshot.save`, `vdu.read`, `portal.run` — survives
almost unchanged; only the transport under it changes, from in-process calls to
QMP + gdbstub. That is a shim, not a rewrite.

### 6.5 The portal: commands in, console out, files across

RISC OS on a Pi has no HostFS. The `hostfs\` shared directory that makes the
current workflow pleasant does not exist on this machine, and that is the single
biggest workflow regression of moving to a Pi 4 sandbox. Two options:

- **A. SD image, host-side injection** (do this first). The ROOL SD image is a FAT
  boot partition plus a RISC OS filesystem partition. Writing build output into
  the FAT partition from Windows is easy (`mtools`, or a small Python FAT writer)
  and RISC OS can read it. Slower loop than a live shared directory, and it needs
  the machine stopped or the image re-attached, but it costs a day.
- **B. A portal MMIO device + a RISC OS module** (do this when A chafes). A tiny
  QEMU device at an unused peripheral address, and a matching RISC OS module that
  claims it, gives: run a `*` command, stream `OS_WriteC` output back to the host,
  and read/write host files as a filing system. This is the Pi equivalent of
  HostFS and of `portal.run`, and it is what would make the Pi 4 machine as
  pleasant as the RiscPC one. It is also the largest single piece of new code
  here, and it needs a RISC OS-side module built with the DDE.

### 6.6 "Interpreter and optional JIT"

Worth being precise about what the interpreter in RPCEmu is *for*: it is the CPU
that can be stepped and breakpointed (`rpcemu/src/CMakeLists.txt:6`). QEMU gives
that property without a second CPU implementation:

- `-accel tcg,one-insn-per-tb=on` makes every translation block one instruction,
  so stepping, tracing and fault attribution are exact;
- the gdbstub single-steps, breakpoints and watchpoints regardless of TCG;
- `-icount` makes the virtual clock exact and runs reproducible — the thing we
  never got working in RPCEmu.

So the practical answer to "interpreter and optional JIT" is: **QEMU is the JIT,
and `one-insn-per-tb` + gdbstub is the interpreter-shaped debugging mode.** If a
literal instruction-at-a-time interpreter is still wanted later — for
cycle-approximate modelling, or for a RISC OS-aware trace of the kind
`dbg_trace.c` produces — that is a fair thing to build, but it should be built
against a machine that already boots, not instead of one.

## 7. Sprints

Estimates are for one person working alone, and assume no RISC OS-side source
changes (we are not rebuilding the ROM).

### Sprint 0 — Harness and baseline (2 days)

- Commit `tools/pi4/`: the QEMU launcher, the QMP client (probe registers,
  disassemble, screendump), the boot-probe script that reproduces §2.2, and the
  two bare-metal benchmark blobs.
- Pin the QEMU version and record the SHA-512.
- **Done when:** the probe script reproduces the mailbox hang and prints
  `PC=0x00016764` on a clean checkout, and the benchmark reports MIPS.

### Sprint 1 — Build QEMU from source on Windows (3–5 days)

- MSYS2 mingw64 (meson, ninja, glib, pixman); build `qemu-system-aarch64` with
  `--target-list=aarch64-softmmu --enable-plugins`.
- Branch `riscos-pi4` off the 11.1 tag; verify the built binary reproduces
  §2.1–§2.3 identically to the released one.
- Build the `libinsn` TCG plugin — we need instruction counting for like-for-like
  MIPS numbers and for trace.
- **Done when:** our build reaches the same hang, and the plugin reports an
  instruction count for a fixed run.
- **Risk:** MSYS2 builds are the fiddly part of this whole plan. Fallback: build
  in WSL2 and drive the Linux binary over TCP from Windows.

### Sprint 2 — Boot to the supervisor prompt (1–2 weeks)

- D1 mailbox channel 0, D2 `bcm2835_ic` byte access, D3 I2C byte access, D5
  property tags, D7 confirm the DMA fix. Then D4 VCHIQ until a display exists.
- Read the `anyvm-org/riscos-builder` patch first; take what is right, understand
  the rest. Keep commits separated into "generic QEMU bug" and "RISC OS needs
  this" from the start, because the first set is upstreamable.
- **Done when:** `screendump` shows the RISC OS boot log and a supervisor prompt
  on `raspi4b` in 32-bit mode, from the **unmodified** ROOL ROM. Screenshot
  committed as evidence.
- **This is the gate.** If D4 is much larger than it looks, fall back to
  `raspi2b` for a working sandbox while the Pi 4 display work continues, and
  accept ARMv7 for a few weeks.

### Sprint 3 — Storage and the toolchain disc (3–5 days)

- Attach the ROOL SD image as qcow2 via EMMC2; get RISC OS to mount `SDFS`.
- Host-side FAT injection so build output crosses the boundary (§6.5 option A).
- **Done when:** `*Cat` inside the guest lists the disc, and a file written from
  Windows is visible to RISC OS.

### Sprint 4 — Portal and the agent channel (1–2 weeks)

- Port the JSON-RPC surface onto QMP + gdbstub: `status`, `regs.*`, `mem.*`,
  `bp.*`, `wp.*`, `snapshot.*`, `frames.*`.
- Portal device + RISC OS module (§6.5 option B) for `portal.run` and `vdu.read`.
- **Done when:** a host script boots from a snapshot, runs a `*` command and gets
  its console output back as text, with no keyboard anywhere in the path.

### Sprint 5 — Determinism and snapshots (3–5 days)

- `-icount` tuned so the machine runs at a sensible rate; `savevm`/`loadvm`
  wrapped as `snapshot.save`/`snapshot.load`; record/replay evaluated.
- **Done when:** two runs from the same snapshot produce byte-identical
  instruction traces — the goal RPCEmu never reached.

### Sprint 6 — roscc on the real target (3–5 days)

- Retarget `compiler/tools/run_on_emu.py` at the Pi 4 machine.
- Run the A72 test set: `movw`/`movt`, `ldrd`/`strd`, `ldrex`/`strex`, `lda`/`stl`,
  `udiv`, `crc32`, VFP, NEON — the exact list in `rpcemu/A72-MODE-PLAN.md` §2,
  which is currently the list of things that *fail* on the RiscPC sandbox.
- Measure MIPS on the RISC OS desktop with the plugin, for a like-for-like
  comparison with the 205 / 920 in §3.
- **Done when:** a hello-world built `-mcpu=cortex-a72` runs under RISC OS 5 and
  prints, with `movw`/`movt` in it, and the test set passes or has a known list.

### Sprint 7 — Give back, and decide (3–5 days)

- Send the generic fixes upstream (`bcm2835_ic` access size, dwc2, property tags);
  keep the RISC OS-shaped ones on the branch.
- Write up the measured numbers and decide whether §8 is ever needed.

Total to a usable A72 sandbox: **roughly 5–8 weeks**, with Sprint 2 carrying
almost all of the risk.

## 8. The fallback we are not taking yet

Building our own Pi 4 emulator — interpreter first, optional JIT — is the
alternative. The honest costing:

- **CPU core.** `dynarmic` (0BSD, x86-64 backend, builds with MSVC, A32 decoder
  already covering ARMv8-A A32 including `CRC32`, `LDA`/`STL`, VFPv4 and 147 NEON
  instructions) would supply the JIT. Its upstream repo is gone; the live fork is
  `azahar-emu/dynarmic`. But it is a **user-mode** core: flat address space, a
  `page_table` of host pointers indexed by `vaddr >> 12`, no MMU, no banked
  registers, no SPSR, no CP15 state, no exception vectors. We would write the
  VMSAv7 walker, the mode/banking/abort model and vector dispatch ourselves,
  filling that page table as a software TLB. That is a real design — the page
  table *is* a TLB — but it is months.
- **Devices.** Everything in §6.2 plus everything QEMU already gives us free:
  GIC-400, CPRMAN, system timer, DMA, EMMC2, framebuffer, PL011.
- **Payoff.** §3 says the payoff is not speed. QEMU already matches the RPCEmu
  dynarec. The payoff would be control — determinism, RISC OS-aware
  instrumentation, no MSYS2 — and §6.4 shows QEMU supplies most of that too.

Revisit only if Sprint 2 shows the VCHIQ/display path is structurally unfixable
in QEMU, or if measured MIPS on real RISC OS code comes in below the 205 we
already get from the RPCEmu interpreter.

## 9. What I did not verify

- That the display hang is *specifically* VCHIQ. §2.3 establishes that it stops
  after three unanswered display property tags; the VCHIQ attribution comes from
  published work on the raspi2b path, not from our own trace.
- The `anyvm-org` patch contents — dates, licence and existence confirmed via the
  GitHub API; the 71 KB diff itself has not been read.
- Whether the 5.31 beta ROM behaves differently. Only 5.30 was traced.
- Any figure for a real Pi 4 in MIPS. The ~1000–1800 M/s in §3 is an IPC argument,
  not a measurement; if it matters, measure it on the Pi 4 itself.
- Whether `-global cortex-a72-arm-cpu.aarch64=off` works as an alternative to
  `-cpu` (it should; untested).

## 10. Sprint log

### Sprint 1 — build QEMU from source on Windows (done)

- MSYS2 at `F:\RISCOSDEV\msys64` (no WSL on this host, and C: had 16 GB free, so
  both it and the QEMU tree live on F:; both are gitignored). Toolchain:
  `base-devel`, `mingw-w64-x86_64-{gcc,glib2,pixman,zlib,pkgconf,ninja,meson,python,capstone,libpng}`.
  pacman hit HTTP 429 from the mirrors once; a retry loop got through.
- QEMU shallow-cloned at `v11.1.0`, branch `riscos-pi4`.
- Configure:

      ../configure --target-list=aarch64-softmmu --enable-plugins --disable-werror           --disable-gtk --disable-sdl --disable-vnc --disable-docs           --disable-guest-agent --enable-capstone --disable-spice

  Do **not** drop capstone: without it the monitor's `x/i` says "Asm output not
  supported on this arch", and disassembly is most of the point. libpng is
  needed for `screendump` in PNG.
- Two Windows-specific things bit:
  - `configure` fails at the postconf step, because `scripts/symlink-install-tree.py`
    builds `qemu-bundle` out of symlinks and Windows needs Developer Mode or
    Administrator to create one. The links point at build outputs that do not
    exist yet, so copying cannot stand in for them. Patched to warn and skip;
    pass `-L <srcdir>/pc-bios` if a blob is ever needed. Local workaround, not
    for upstream.
  - The built binary needs `F:\RISCOSDEV\msys64\mingw64\bin` on `PATH` for its
    glib/pixman DLLs. Without it Windows kills it silently — no error, no output.

### Sprint 2 — boot to the supervisor prompt (in progress)

Eight commits on `riscos-pi4`, seven of them real:

    hw/misc: implement BCM2835 mailbox channel 0 (power management)
    hw/arm/raspi: use the AArch32 secondary boot stub for 32-bit guests
    hw/i2c/bcm2835_i2c: allow byte access
    hw/i2c/bcm2835_i2c: only start a transfer on ST, and buffer the TX FIFO
    hw/misc/bcm2835_property: implement the touch and GPIO virtual buffer tags
    hw/misc/bcm2835_property: answer the GPIO state tags
    hw/arm/bcm2838: connect the system timer to the GIC
    scripts/symlink-install-tree: survive a host without symlink permission

Four of the seven are plain QEMU bugs with nothing RISC OS-specific about
them, and should go upstream: the secondary boot stub, both I2C fixes, and the
system timer routing. Any 32-bit guest on `raspi3b`/`raspi4b` hits the first;
any guest that fills the I2C FIFO before setting ST hits the second; any guest
that uses the system timer through the GIC on `raspi4b` hits the last.

Evidence for each is in §2.3. Two of them — the secondary boot stub and the I2C
access size — are plain QEMU bugs with nothing RISC OS-specific about them, and
should go upstream. The mailbox channel-0 peer is a missing device rather than a
bug. The EDID EEPROM is a modelling choice and the weakest of the four.

**Verification.** `tools/pi4/mboxtest.s.in` is a bare-metal regression test that
posts the HAL's channel-0 message and waits with a bounded spin: stock QEMU
prints `TIMEOUT`, the patched build prints `00000080`. MMIO volume over a 12 s
boot is the other measure, and it tracks the fixes:

| Build | MMIO events in ~12 s | Where it stops |
| --- | --- | --- |
| stock | 1,049,989 | mailbox channel 0, `PC=0x16764` |
| + channel 0 | 1,049,989 | secondary-core wait, `read 0xff8000c4` |
| + smpboot32 | 6,024 | I2C FIFO byte store → data abort |
| + I2C byte access | 5,685,747 | EDID NACK, `S=0x157` |
| + EDID EEPROM | 5,643,283 | BSC status spin, `S=0x57` |

### Sprint 2, continued — the clock

Three more blockers came out of the same method, and the last of them was the
one that mattered.

**5. The I2C state machine.** Two bugs in `bcm2835_i2c`. It began a transfer on
`I2CEN` as well as `ST`, so each write that merely enabled the controller
opened and (with `DLEN` still zero) immediately closed a transfer of its own.
And it had no transmit FIFO — bytes went straight to `i2c_send()` and were
dropped if no transfer happened to be active. The documented sequence is set A,
set DLEN, fill the FIFO, *then* set ST, which is exactly what the HAL does, so
its two bytes vanished, `DLEN` never counted down and `TA` never cleared.
Requiring ST and adding the hardware's 16-byte TX FIFO fixed it. QEMU's own
`bcm2835-i2c-test` still passes.

**6. The EDID EEPROM was a mistake, and is gone.** Reading `HAL_BCM2835/s/IIC`
and the kernel's `PMF/i2cutils` shows that address `0x50` here is
`RTCAddressPHI` (`&A0 >> 1`) — a PCF8583 RTC/CMOS probe, not a monitor. A real
Pi has no RTC there, so **NACK is the correct answer**; making it ACK fed RISC
OS EDID bytes as CMOS contents. Once the state machine was fixed the probe
NACKs cleanly (`S = 0x152`: DONE set, TA clear, ERR set) and the guest moves
on — to `0x68` on **BSC1**, the DS1307-style RTC, which is the right bus for a
Pi 4. EDID does not come over I2C at all: it arrives by property tag
`0x00030020` GET_EDID_BLOCK.

**7. The system timer was never connected to the GIC.** `bcm2838_realize()`
re-routes UART0, AUX, I2C, mailbox, SDHOST, EMMC, MPHI, DWC2 and DMA from the
legacy interrupt controller to the GIC-400 — but not the system timer, whose
four compare outputs are wired once in the shared BCM2835 code and never
forwarded. RISC OS uses compare 1 as its centisecond tick and enables INTID 97
for it. The timer expired exactly once, set its status bit, raised a line into
a controller nobody was listening to, and the OS then waited for a clock that
could never advance. Every timed wait was infinite.

The trace said it plainly: one write to the compare register, one
`bcm2835_systmr_timer_expired`, no `bcm2835_systmr_irq_ack`, ever. Afterwards:

| | before | after |
| --- | --- | --- |
| timer expiries in ~20 s | 1 | 1,335 |
| timer IRQ acks | 0 | 1,335 |
| GIC IAR reads (INTID 97) | 0 | 2,953 in 45 s |

**Where it is now.** RISC OS services its tick, dispatches SWIs, runs module
code in RAM, has started probing the DWC2 USB controller, and has posted its
first message to mailbox **channel 3 (VCHIQ)** — which has no peer, and which
the published work on the raspi2b path names as the display blocker. So the
next task is the VCHIQ peer, and this time the guest has actually got there.
There is still no framebuffer: no `FRAMEBUFFER_ALLOCATE` has been issued.

`SET_CLOCK_RATE` (`0x00038002`) is still NYI in QEMU and RISC OS now calls it
twice; `GET_BOARD_MODEL` and `GET_BOARD_SERIAL` log NYI but do return data.

A caveat for upstreaming: QEMU's `bcm2835-i2c-test` attaches a `tmp105` at
0x50 on every bus, so any device we add there would collide. That is one more
reason the EDID EEPROM was the wrong idea.

### What made the difference

Tracing beat theorising every time. The winning move was
`-trace enable=memory_region_ops_*`, then histogramming by device and by
address: each blocker announced itself as a single hot register. When the guest
went quiet instead of spinning, interleaving `-d in_asm` with the MMIO trace and
reading the seam showed the data abort directly. Every hypothesis formed any
other way — display bring-up, VCHIQ, EDID-before-byte-access — was wrong.

## 11. Reaching the prompt

![RISC OS 5.30 on an emulated Pi 4](first-boot.png)

Two more blockers stood between the working clock and that screenshot.

### The VCHIQ handshake, and why refusing was the answer

The guest posts the bus address of a `vchiq_slot_zero` structure to mailbox
channel 3 and then waits. QEMU has never had a device on that channel, so the
message went into an unmapped slot of the mailbox address space.

The wait is unbounded and cannot be failed from outside: `vchiq_connect` blocks
on a counting semaphore with no timeout, no deadline, and no register the guest
re-reads. There is no value QEMU can return and no error it can inject. **The
only exit is a real CONNECT message in shared memory plus a doorbell
interrupt.** And the module that calls it is not the video driver — the ROM's
module order is `RTSupport, USBDriver, DWCDriver, XHCIDriver, VCHIQ, BCMSound,
ScreenModes, BCMVideo`, and it is **BCMSound** whose init calls `VCHIQ_Connect`
unconditionally. The display code never ran at all.

The structure was read out of a live guest before a line was written:

| | at `+0x20` (VideoCore) | at `+0x194` (ARM) |
| --- | --- | --- |
| `initialised` | 0 | 1 |
| `slot_first` / `slot_last` | 2 / 32 | 34 / 64 |
| `tx_pos` | — | **8** |
| `slot_queue[0]` | — | 34 |

and slot 34 already held `{0x01000000, 0}` — a CONNECT, written and unread. The
shared-state stride derives from `slot_zero_size` rather than being hardcoded,
because the debug array is last in the struct and its length is a build option.

So the peer sets the VideoCore side's `initialised`, fills the slot queue the
guest zeroed and expects us to own, arms our trigger, queues a CONNECT and rings
the VC→ARM doorbell at mailbox base `+0x40` on SPI 34 (INTID 66, one above the
mailbox — matching the HAL's `iDev_ARM_DBell0`). Then it **refuses every service
the guest opens**: `AUDS`, `GCMD`, `DISP`, `TVSV`.

Refusing is not a shortcut, it is the correct answer. Accepting `AUDS` walks
BCMSound into a blocking `MsgQueue`; accepting `TVSV` arms two more untimed
spins. Declining leaves `GPUModeAvailable` at zero, so mode setting falls back
to the property channel — which QEMU models completely. The tag chain that
follows is the proof:

    0x00010006 GET_VC_MEMORY      <- BCMVideo init, running at last
    0x00030020 GET_EDID_BLOCK
    0x00048001 FB_RELEASE
    0x00048003/4/9/5/6/7, 0x00040008 GET_PITCH
    0x00040001 ALLOCATE_BUFFER    <- the framebuffer
    0x00040002 unblank, 0x0004800b SET_PALETTE

### The PCIe root complex

RISC OS then printed a data abort on its own console. The fault addresses were
root complex offsets `0x9210` (`PCIE_RGR1_SW_INIT_1`), `0x4008`
(`PCIE_MISC_MISC_CTRL`) and `0x402c` (`PCIE_MISC_RC_BAR1_CONFIG_LO`) — the
documented reset-and-probe sequence, run only on a Pi 4, looking for the VL805
xHCI. Nothing is mapped there, so each access took an external abort (DFSR
`0x8`, not a translation fault). An unimplemented-device stub reads as zero,
which the guest reads as link down. Aborts over a 30 second boot: **14 → 1**.

### The design principle that produced all of this

From the project owner, and it is worth writing down because it is what made
the difference:

> rather than emulating videocore; we should emulate the functions needed by
> risc os … let's be as soft and fake as possible and emulate hardware only if
> we are desperate … we probably do need interrupts though, the whole system is
> running off timers.

Real interrupts where the OS genuinely depends on them — the tick and the
doorbell are both real, and ~2,900 are serviced per 30 seconds. Functional
shims everywhere else. The VCHIQ peer is about 250 lines and models no
hardware; it answers questions.

### The commits

    hw/arm/bcm2838: stub the PCIe root complex region
    hw/misc: add a VCHIQ peer for mailbox channel 3
    hw/arm/bcm2838: connect the system timer to the GIC
    hw/misc/bcm2835_property: answer the GPIO state tags
    hw/misc/bcm2835_property: implement the touch and GPIO virtual buffer tags
    hw/i2c/bcm2835_i2c: only start a transfer on ST, and buffer the TX FIFO
    hw/i2c/bcm2835_i2c: allow byte access
    hw/arm/raspi: use the AArch32 secondary boot stub for 32-bit guests
    hw/misc: implement BCM2835 mailbox channel 0 (power management)
    scripts/symlink-install-tree: survive a host without symlink permission

### What is left

- **One data abort remains**, at a different address, still being chased.
- **No keyboard.** The four type-A ports are the VL805 behind PCIe, which we
  deliberately do not model; the HAL exposes only DWC2, on the USB-C port. The
  intended answer is to inject keys rather than emulate a controller — most
  likely through a guest-side VM compatibility module talking to one MMIO
  device, which is also where console capture and a host filing system belong.
  That is the same shape as the HostFS podule ROM in the RPCEmu setup.
- **No disc**, so the prompt is where it stops. Sprint 3.
- `GET_EDID_BLOCK` (`0x00030020`) is still unimplemented in the property
  channel, and `SET_CLOCK_RATE` is still NYI.
