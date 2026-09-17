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
— capstone around any PC.  `rdb.mem addr,len` / `rdb.memw addr,val` —
dumps via the walk, writes gated behind the same confirmation the
screendump capture path uses.  `rdb.bp ±addr[,task]` /
`rdb.wp ±addr,len,kind` — breakpoints and r/w/x watchpoints over
`cpu_{break,watch}point_insert`.  `rdb.tasks`, `rdb.das`,
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

## 5. The window, and the phasing

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
2. **The metal window.**  renders exactly those commands; acceptance
   is a human breaking on a module SWI and stepping it while watching
   registers and stack — the session SCRIPTING.md's E-sprints were
   measured with.
3. **Analysers.**  module walker, SWI namer, heap/stack views on
   named areas; acceptance is walking the system heap's fragmentation
   before and after a known allocation churn.
4. **The guest agent** (with or after Portal): live task and DA
   enumeration, task-verified breakpoints (§2.3); acceptance is the
   two-task adversarial case — a breakpoint that would have fired on
   the wrong task, verified and skipped.

## 6. Non-goals, written down

No per-task stepping without a machine stop (TCG has no per-task
scheduling to hang it on).  No source-level debugging — the DDE emits
no DWARF worth trusting and the window will not pretend otherwise.
No memory writes without the confirmation path.  No gdbstub
replacement — the external gdb path stays exactly as it is; this
window is for the human at the machine and the agent on the socket.
