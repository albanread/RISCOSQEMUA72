# DEBUGDESIGN — a debugging window for RISC OS applications

Debugging a RISC OS application on this emulator today means either a
1990s guest-side tool or an external `gdb-multiarch` against the raw
gdbstub — workable, and nothing a human wants to look at.  This
document designs the alternative: a **second window** in the native
front ends — registers, disassembly, stack, heap, memory dumps,
breakpoints, single step, safe resume, and analysers — built on one
rule the scripting surface already established: **the window is a
consumer, never an implementation**.  Every capability lands as a
command in the existing table (SCRIPTING.md's namespace, served over
Apple Events and QMP alike); the window renders what those commands
return, an agent drives the same commands headless, and the dx11 twin
gets its window by consuming the same model — the lesson of the
backdrop's duplicated `tile_is_2x` written down as architecture.

---

## 1. What already exists, and the design rides all of it

- **Machine stop and step.**  `vm_stop`/`vm_resume` are the main loop's
  own; HMP `stop`, `cont`, `singlestep` already exist; TCG's
  `one-insn-per-tb` gives exact single-instruction stepping at heavy
  cost, flipped on for a step and off for a run.
- **Breakpoints and watchpoints.**  QEMU's internals —
  `cpu_breakpoint_insert` / `cpu_watchpoint_insert`
  (`include/hw/core/cpu.h:1152`) — the same machinery the gdbstub
  uses: TB-invalidation breakpoints, not memory patching.  Invisible
  to the guest (no checksum surprises, no ROM writes), exact, and
  already synchronised with TCG.  The fork added an HMP command before
  (`synthfb`, `hw/display/bcm2835_fb.c:614`); adding `rdb.` commands
  over these APIs is the same shape of change.
- **Registers, memory, disassembly.**  The monitor already reads
  registers and disassembles (`x/i` — the README keeps capstone in the
  build *for exactly this*); memory reads walk the MMU
  (`vmch_guest_rw`) — and with the machine stopped, the walk's
  reliability questions are settled by construction: the guest is
  quiescent, `thread=single` is pinned, and the pages are as mapped.
- **The command table.**  One table generates the sdef, the `describe`
  answer, and the QMP surface (SCRIPTING.md).  The debugger's commands
  join it; nothing new is invented for the window.

## 2. The RISC OS trap the design must not step on

Application space is **one address range, many tasks**: 0x8000 up
belongs to whichever Wimp task is paged in, re-mapped on every task
switch.  A breakpoint set in a task's code at 0x8000+n therefore arms
against the *address*, not the *task* — the next task switch brings a
different program under the same breakpoint, and a single-step
"through" one application steps whichever task owns app space at that
instant.  This is the oldest footgun in RISC OS debugging and the
design treats it as first-class:

1. **Machine-stop semantics, honestly labelled.**  All stop/step/break
   operations are machine-wide, exactly like the gdbstub, and the
   window says so — "the machine is stopped", not "your task is
   stopped".  No pretending per-task scheduling exists.
2. **Stable-address debugging works perfectly**: modules, ROM, RMA,
   the SVC/IRQ stacks — breakpoints there are exact, because those
   addresses never move.
3. **Task awareness is a guest agent away.**  A small module (the
   Portal's ring, PORTAL.md, is the natural transport once it exists)
   can answer "which task owns app space right now" (Wimp task
   handles) and arm the front end to *verify the task before treating
   a hit as a hit*: on breakpoint, query the owner; mismatch means
   continue and keep the breakpoint.  Not v1 — but the command
   namespace reserves the slot, and §5 phases it.

## 3. The commands (the window's whole diet)

`rdb.stop` / `rdb.cont` / `rdb.step [n]` — machine stop, resume,
step; `step` toggles `one-insn-per-tb` around the resume, and **safe
resume is defined here**: the stop lands at a translation-block
boundary (never mid-doorbell — a synchronous doorbell op completes
before the stop is visible), the resume restores exactly the mode it
found (`one-insn-per-tb` off, devices untouched), and the front end
refuses a resume it did not pair with its own stop.  `rdb.regs` — all
register banks per mode, plus CPSR/SPSR decoded.  `rdb.dis addr[,n]`
— capstone around any PC.  `rdb.mem addr,len` / `rdb.memw
addr,val` / `rdb.regw reg,val` / `rdb.fill addr,len,val` — dumps via
the walk; every write lands in the patch journal (§5) and is gated
behind the same confirmation the screendump capture path uses.
`rdb.asm "add r0, r0, #4"` — one A32 instruction, assembled host-side,
answered with its encoding and capstone's reading of it back (§5).
`rdb.bp ±addr[,task][,action=...]` /
`rdb.wp ±addr,len,kind` — breakpoints and r/w/x watchpoints over
`cpu_{break,watch}point_insert`.  An action turns a breakpoint into a
scripted trap — set a register, write memory, skip the instruction,
fake a SWI's return — then continue, all at the stop, all
deterministic (§5).  `rdb.tasks`, `rdb.das`,
`rdb.heap area` — the analysers of §4.  Every command returns JSON
shaped for the table, so `describe` advertises them and an agent can
run a whole session headless the day they land.

## 4. Analysers

- **Disassembly** — capstone, already linked, already proven by the
  monitor's `x/i`.
- **SWI namer** — a table from the same ROOL source `hostnet`'s
  `swis.txt` generation already uses; the disassembly annotates
  `SVC &xxxx` with names.
- **Module walker** — the module chain is a linked list of documented
  headers in ROM and RMA; a read-only walk from a memory dump yields
  titles, versions, sizes — the same attribution `hotblocks` +
  `*Modules` does by hand, done live (tcg-profiling.md §6.1's trick,
  automated).
- **Heap and stack views** — the system heap and Dynamic Areas are
  OS_Heap-format blocks with PRM-documented headers; a walker labels
  free/used/adjacent and computes fragmentation.  Stacks: USR, SVC,
  IRQ — `rdb.regs` gives the three R13s, the view dumps downward with
  return-address guessing flagged as guessing.  Discovery of *task*
  heaps needs the guest agent (§2.3); the host-side walker works on
  any base it is pointed at from day one.

## 5. The live side — assembling, patching, and scripting the traps

Modifying RISC OS as it runs is the point of a monitor, and it is where
the design earns its safety story:

- **The assembler is one instruction at a time**, A32 only, host-side:
  an encoder for the working set — data processing, loads/stores,
  branches, `swi`, `bkpt`, `nop`, push/pop — the BBC BASIC heritage
  shape, a mini-assembler with the disassembler as its checker.  Every
  `rdb.asm` answer carries the encoding *and* capstone's reading of it
  back; if the round trip does not agree, the answer is a refusal, not
  a guess.  The window's disassembly view gets an inline patch field:
  type the instruction, see the encoding and the confirmation, commit
  into the journal.
- **The monitor views are the hex and register editors**: memory with
  an ASCII gutter and modified words starred, every register bank
  editable through `rdb.regw`.  Writes into MMIO are marked as MMIO —
  they do real device things — and writes aimed at ROM are *refused
  with the explanation*: loader ROM is a read-only mapping, and deeper,
  `rom_reset` restores the blobs, so a ROM "patch" would be a lie even
  if the write landed.  Live patching belongs where it works:
  application space, RMA and module code — and RMA is the delicate
  one, which is why the module walker (§4) labels relocated words so
  the editor can warn.  ROM changes go through `mkrom.py`, offline, by
  design.
- **Scripted traps** are the breakpoint actions: break on a SWI, fake
  its return value and V flag, continue; break on a driver call and
  skip it; break on an error path and force it.  The action runs at
  the stop under the BQL with the machine quiescent — the exactness
  the breakpoint itself has — and its writes land in the journal like
  any other patch.
- **The journal is the safety net.**  Every modification — memory,
  register, fill, assemble, trap action — is recorded with before and
  after, revertible singly or wholesale, and starred in every view that
  shows the location.  Snapshots (already in the window menu) restore
  the guest and thereby the patched memory with it; the journal is the
  session's own record and revert, the snapshot is the machine's.

## 6. The window, and the phasing

The metal front end gains one `NSWindow` (debug panels: registers,
disassembly with PC tracking, stack, memory, breakpoints, analysers)
opening from the Machine menu beside Backdrop — driven entirely by
§3's commands over the internal dispatch the scripting surface
already uses.  The dx11 twin's window is a later sprint that consumes
the identical command model; the twin-duplication review (7/8 on the
reliability list) is why the model is defined once, here, first.

Sprint order, each with its acceptance test:

1. **Backend commands + scripting exposure.**  stop/step/regs/mem/dis/
   bp/wp live over QMP and Apple Events; an agent script stops the
   machine, sets a breakpoint in RMA, resumes, and reports the hit.
   No window.  Everything is testable headless on the farm.
   **Done** -- the thirteen `rdb-*` monitor commands (QMP via
   human-monitor-command), verified end to end against a bare-metal
   AArch32 loop: breakpoint hit at the exact address, single step
   through a taken branch, register and memory writes read back.  The
   Apple Events half of the exposure is still owed; the commands it
   will call are not.
2. **The metal window.**  renders exactly those commands; acceptance
   is a human breaking on a module SWI and stepping it while watching
   registers and stack — the session SCRIPTING.md's E-sprints were
   measured with.
3. **Analysers, and the live side.**  Module walker, SWI namer,
   heap/stack views on named areas; the assembler with its round-trip
   check; the patch journal with revert; breakpoint actions with
   auto-continue.  Acceptance: assemble `mov r0, #0` over a live
   instruction and watch capstone read it back; break on a SWI, fake
   its return, continue, and see the caller take the fake; a write
   aimed at ROM refused, explained, journaled as a refusal.
4. **The guest agent** (with or after Portal): live task and DA
   enumeration, task-verified breakpoints (§2.3); acceptance is the
   two-task adversarial case — a breakpoint that would have fired on
   the wrong task, verified and skipped.

## 7. Non-goals, written down

No per-task stepping without a machine stop (TCG has no per-task
scheduling to hang it on).  No source-level debugging — the DDE emits
no DWARF worth trusting and the window will not pretend otherwise.
No Thumb in the assembler (RISC OS 5.30 executes none in anger), no
macros, and no live ROM patching — the splice path exists for that.
No memory writes without the confirmation path.  No gdbstub
replacement — the external gdb path stays exactly as it is; this
window is for the human at the machine and the agent on the socket.
