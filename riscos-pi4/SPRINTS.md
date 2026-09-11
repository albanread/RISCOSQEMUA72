# Sprints

The goal has not moved: the fastest RISC OS A72 emulator there is, with an
integrated debugging environment, and the badge goes green when it works.
This is the road from where the fork stands to that, in sprints of a few
days each, with what each one delivers and how you know it is done. The
first plan lived in `DESIGN.md` §7; this supersedes it.

## Where the fork stands

| Original sprint | State |
| --- | --- |
| 0 Harness and baseline | done — QMP probe, screendump, bare-metal benchmarks, instruction-count plugin |
| 1 Build QEMU on Windows | done — MSYS2 MINGW64, capstone, libpng, slirp |
| 2 Boot to the supervisor prompt | done, and past it — the desktop from ROM, then from the card |
| 3 Storage and the toolchain disc | done for reading — SDFS from the ROOL image on EMMC2; no host files yet |
| 4 Portal and the agent channel | not started — see Sprint 6 below |
| 5 Determinism and snapshots | not started — Sprint 5 and 10 |
| 6 roscc on the real target | not started — Sprint 7 |
| 7 Give back, and decide | not started — Sprint 11 |

Beyond the plan: USB keyboard and mouse through the FIQ path, networking
through a CDC-Ethernet `usb-net`, 800×600 from a synthetic EDID, and a boot
measured at 22–27 seconds to an idle desktop with the network up.

## The order

```
U0 → U1 → U2 ────────────► a usable desktop application
                 │
                 ├─► 5 snapshots ──► 7 developer loop ──► 8 debugger ──► 10 record/replay
                 │                        ▲
                 └─► 6 host files ────────┼─► 13 blitter, sprites, pointer
                                          │
U3, U4 and 9 slot in wherever a week has room; 14 reviews 9 and 13
together; 11 and 12 come last.
```

U0–U2 first because everything after them is used through that window. Then
snapshots, because a desktop that appears in under a second changes how
every later sprint is tested. Host files and the developer loop are the
point of the whole exercise; the debugger sits on top of them; speed is
worked in parallel and never blocks anything.

---

## U0 — the window owns the thread (2 days)

`-display dx11` accepted; a Win32 window on the main thread; QEMU's main
loop moved to the `qemu_main` thread by the hand-off `system/main.c` already
provides; a D3D11 device and swap chain clearing the window at vsync;
closing the window shuts QEMU down cleanly.

*Done when:* RISC OS boots behind the (blank) window and the boot timeline
over QMP matches `-display none` — the UI thread costs the emulation nothing.

**Done.** The display lives in `ui/dx11.c` (QEMU side, C) and `ui/dx11.cpp`
(window + D3D11, C++), split at an `extern "C"` boundary because QEMU's
headers are not C++-parseable. Verified on the 5.30 ROM with the ROOL card:
the desktop is in the framebuffer at t+30s behind the window, and a
headless `-display none` run with the same devices is pixel-identical
(84 of 480,000 pixels differ — the clock). Posting `WM_CLOSE` to the window
ends the process in under a second.

Getting there needed the boot unblocked first: the CMOS blob in use had
been built without `--unplug 106`, so EtherGENET started, was handed a
GENET device by the HAL's fixed table, found no controller behind it (none
is modelled), and aborted at `&FC3FB800` — the data abort that used to be
the whole screen.
`tools/patch-rom-nogenet.py` is the source-free equivalent of the unplug
bit until GENET is modelled; the machine also gained an
unimplemented-device stand-in at GENET's register block (`0xfd580000`),
same shape as the PCIe one, so a future driver that probes it reads "no
silicon" rather than an external abort.

## U1 — the framebuffer, decoded on the GPU (3–4 days)

The fb config accessor and generation counter in `bcm2835_fb`; raw upload
of the guest framebuffer each frame; the **8 bpp palette** and **32 bpp**
decoders — the two formats RISC OS's `BCMVideo` ever asks the Pi's GPU for —
with the palette read from where the `SET_PALETTE` tag put it; the scaler
stretching any mode to the whole client area, as the GPU stretches every
mode to the display mode; PNG screenshot from the decoded surface.

*Done when:* the 800×600 desktop renders at the monitor's rate; a 256-colour
mode shows the right palette; a mode change mid-session is handled without
touching the window.

**Done, except the two mode-change checks, which need U2's input to test.**
The config is a seqlock: `bcm2835_fb_reconfigure` bumps the generation odd
while writing and even on commit, and `bcm2835_fb_get_config` hands the UI
thread a snapshot that is never torn; a mode change is one rebuild of the
per-mode pipeline, and the UI never sees the window move. Guest RAM is
mapped through the fb device's own DMA address space — `cfg.base` for the
buffer, `vcram_base` for the palette — and read on the UI thread with no
lock, which is exactly what a monitor does. One HLSL source compiles into
all seven decoders at start-up (32 and 8 are the RISC OS pair; 16/24 and
the sub-byte modes ride along); a failed decoder is a log line and a clear
screen, never a half-built pipeline. PrintScreen writes the decoded
surface to `dx11-screenshot-N.png`.

Verified on the pristine 5.30 ROM with the unplug CMOS: the 800×600
desktop renders behind vsync (and in a resized 896×614 window — the scaler
stretches any mode to the client area); the screenshot decodes to exactly
the QMP screendump's pixels; closing the window exits the process in
about a second.  The two mode checks waited for U2's keyboard and are now
done with it: `*SCREENMODE 800x600x256` (and the user's own Display
Manager) brought up a 256-colour desktop — `pipeline built: gen 16,
800×600, pitch 800, bpp 8` — with the palette decoder confirmed by eye,
and the same session moved 32bpp → 8bpp → 32bpp again, the pipeline
rebuilding on each generation bump without the window so much as
flickering.

## U2 — keyboard and mouse (3 days)

Scan codes to qcodes through the win32 keymap already in the tree; repeats
dropped; grab, the `win32-kbd-hook.c` hook and Ctrl+Alt+G release;
raw-input relative mouse, three buttons as Select/Menu/Adjust, wheel; an
afternoon on `usb-tablet` to learn whether RISC OS's HID driver takes an
absolute pointer.

*Done when:* typing at the RISC OS command line and dragging a window both
feel like the real thing, and the host gets its pointer back on demand.

**Done.** It ended up absolute, not relative, and one guest-visible QEMU
bug had to die first:

- The keyboard is scan codes through `qemu_input_map_atset1_to_linux`
  (the extended-bit + scan form), delivered under the BQL like cocoa's
  `with_bql`.  Verified by typing at the F12 CLI: `*HELP` prints its
  listing, `*BASIC` runs, and an immediate-mode `MOUSE x,y,b:PRINT`
  became the readback instrument for the pointer work.
- `usb-tablet` it is: RISC OS's USBDriver claims absolute HID devices
  (`UMS_ABS`), but its attach path treats a failed SET_PROTOCOL as fatal
  and QEMU stalled that request for tablets — so the pointer froze at
  the screen corner.  Accepting SET/GET_PROTOCOL for `HID_TABLET`
  (`cc932f6a7f`) fixed it; QMP-injected absolutes then map exactly
  (8000,4000 of 0..32767 reads back 390,1058 on the 1600×1200-unit
  screen).
- The window sends absolute coordinates on a `usb-tablet`: ungrabbed,
  the host cursor's client position scaled to the guest screen, so the
  two arrows coincide — the reported offset was relative deltas
  accumulating drift.  Grabbed (a click into the window captures the
  pointer, Ctrl+Alt+G or focus loss releases), host deltas accumulate
  into a virtual position still sent absolutely: relative feel, no
  drift, and release re-aligns the arrows.  Confirmed by the user on
  the desktop.
- The mouse readback channel (`MOUSE`) verified buttons too: Select
  held reports 4.  A synthetic click on the icon bar's disc icon opened
  the SDFS Filer window.

U2's one late lesson is recorded under U0's window instead: a flip-model
`Present(1)` parked the UI thread forever once Windows ghosted the
window; the chain is waitable now and presents only on a free slot.

## U3 — the rest of the formats, and polish (2–3 days)

16 bpp RGB565, 24 bpp and the packed low-depth decoders; sharp-bilinear and
the scanline option; borderless fullscreen; DPI awareness; a status line
with mode, frame rate and instruction rate; WARP fallback where there is no
GPU.

*Done when:* every row of the shader table in `UI-DESIGN.md` has been seen
working — RISC OS's own formats on real data, the rest on synthetic buffers.

**Done, except the status line.**

- Every decoder row is proven.  8/16/32 on the live desktop (16 bpp was
  pixel-exact against QMP's own screendump: 0 of 480000 differ; 8 bpp
  user-confirmed in the Display Manager; 32 bpp the boot default).  For
  24 and the packed 4/2/1 there is no guest route — BCMVideo programs
  nothing but 8/16/32 — so `synthfb BPP [XRES [YRES]]` (HMP) forces the
  framebuffer into any depth and size with a pattern whose bytes are a
  function of their offset; `tools/synthfb-test.py` freezes the guest,
  screenshots the window's decoded surface and compares it against the
  pattern's expected image.  All seven depths pass pixel-exact (0 of
  307200 differ each).  The sub-byte depths first needed `get_pitch` to
  round up to whole bytes — it gave them a zero pitch and a zero-sized
  framebuffer.
- `-display dx11,scaling=sharp|linear|nearest` (default sharp) and
  `,scanlines=on|off`.  Sharp bilinear narrows the bilinear band to a
  1/ratio strip at each source-pixel edge: 1:1 passes through untouched,
  magnification stays crisp.  Scanlines darken alternate output rows at
  2x+ magnification only.
- Alt+Enter is a borderless-fullscreen toggle (Raymond Chen's
  WINDOWPLACEMENT dance); the process is per-monitor-DPI-aware so the
  host never bitmap-scales the window on scaled displays; WARP remains
  the software fallback when there is no GPU.
- The firmware now clears a freshly-allocated framebuffer, as the real
  one does: RISC OS boots through a 640×480×16 mode before the desktop,
  and without the clear the window showed that buffer's garbage RAM
  until the OS painted (seen live).  A pure pan keeps its buffer.

## U4 — measure, then dirty rows if needed (2 days)

Profile the UI thread against the guest's instruction rate; add dirty-row
uploads through the memory dirty bitmap only if the numbers ask for it;
document what a frame costs.

*Done when:* instruction-rate figures with the window open match those
without.

**Done.** `bench`/`bench2` (register-only and load/store loops, timed by
the guest's own 1 MHz system timer, printed over the PL011) ran
headless, with the window clearing, and with the window's full pipeline
live — `benchfb` is the variant that programs a 800×600×32 framebuffer
over mailbox channel 1 before timing, so upload+decode+scale+present
run at 60 Hz while the loop is measured.  Interleaved pairs (the
desktop was also running; single samples wobble ±10 %):

    headless                ~1580 MIPS
    window, full pipeline   ~1540 MIPS   (-2.3 %, within the noise)

The 1.9 MB-per-frame upload costs the guest nothing measurable — the UI
thread and the GPU are off the vCPU's core — so dirty-row uploads are
not asked for and were not built.  For scale against real silicon:
~1580 MIPS register-only and ~455 MIPS memory-bound is between a
quarter and a half of one real Pi 4 core on synthetics (best case par),
NEON-heavy code worse; that is TCG's usual standing, not the window's.

---

## 5 — snapshots: the desktop in under a second (3 days)

Put the card image behind a qcow2 overlay so disc state is part of a
snapshot; `savevm` at the idle desktop; `loadvm` from the command line and
from the window's menu; check that the FIQ path, the SD read-ahead and the
network survive a restore (the GIC's legacy-FIQ level and the DWC2 IRQ level
were written with this in mind); a `--snapshot NAME` convenience in the
tools.

*Done when:* a cold start to a usable networked desktop takes under one
second from a snapshot, and a boot from scratch still works unchanged.

**Done.**  `tools/run.py` is the wrapper: it creates the qcow2 overlay over
the card image on first use, launches the canonical machine, and
`--save NAME` snapshots machine+disc once the desktop is painted (the
emulator keeps running); `--snapshot NAME` is a `-loadvm` cold start.

Two devices needed real work:

- `usb-net` was vmstate-stubbed `unmigratable=1`, which blocked savevm for
  the whole canonical machine.  The CDC path now migrates (USB core,
  control-request results, half-sent frames both ways; the RNDIS response
  queue is documented as uncovered and is always empty in CDC mode).
- `bcm2835_fb` post_load forces `invalidate` and calls
  `qemu_console_resize` to the restored mode: a restored machine never
  passes through reconfigure, so the console kept its placeholder surface
  and the first screendump segfaulted (gdb: `draw_line` dst=0x448fc000,
  unmapped).

Measured: cold start to a painted desktop **0.68 s** (`-loadvm desktop`),
and the restored machine is fully alive — F12 opens the CLI, its caret
blinks (the centisecond ticker survived), `*CAT SDFS::SD_FSDisc.$.`
lists the disc, and `*PING 10.0.2.2` over the restored usb-net/slirp
answers in 2 ms.  The window's system menu carries "Load snapshot",
which rewinds to the snapshot named `desktop` through a bottom half on
the main loop (the hmp_loadvm sequence); verified live — a session mid-
`*CAT` at the CLI reverted to the clean desktop.  Boot from scratch is
unchanged (`run.py` with no options).

## 6 — host files (4–5 days)

Two routes, in order of cheapness:

1. **LanManFS over slirp.** RISC OS ships an SMB client; slirp's NAT puts
   the host at `10.0.2.2`. If RISC OS 5.30's LanManFS talks to a Windows
   share (its SMB2 support is the thing to verify), host files arrive with
   no guest code at all. One day to find out, and if it works this is the
   answer for the developer loop.

   **Dead, and proven from both ends.**  The guest reaches the host fine
   (NetSurf browses the wider internet through slirp; a `*PING 10.0.2.2`
   answers in 2 ms; NetSurf fetched a listing from a host-side HTTP
   server).  But Windows 11 here runs `EnableSMB1Protocol: False` with
   signing required, and the ROM's LanManFS offers exactly two dialects —
   `PC NETWORK PROGRAM` and `NT LM 0.12`, both SMB1 (the strings in
   RISCOS.IMG; no SMB2/3 dialect exists in the ROM).  `*LMount` brings up
   the login dialogue, and the logon attempt fails as `Error: Bad
   authentication` — LanManFS's mapping of a refused negotiation.
   Re-enabling SMB1 on the host would "work" but is a deprecated
   protocol with signing disabled; not recommended, not the answer.

2. **The VM compatibility module.** A doorbell device in QEMU at an address
   the HAL does not name, and a RISC OS module that lives on the card image
   in `!Boot.Choices.Boot.PreDesk` — loaded by the boot sequence, so no ROM
   splice — offering: host filing system (`HostFS:`), console capture
   (`*` output back to the host), absolute pointer, the mode's eigen
   factors for the UI, clipboard, and time sync. The ROM-splice recipe in
   `DESIGN.md` stays for disc-less boots.

   **This is the plan.** The design is `FSDESIGN.md`: the `vmchannel`
   device (synchronous doorbell, request blocks in guest RAM, no queues,
   trivially snapshot-safe) and the `HostFS` module (roscc/Mojo, the
   mojomod pattern, soft-loaded from the card).  Sprint 6 splits:

   **6A — the vmchannel device (1–2 days).**  Find the hole in the Pi 4
   map the HAL does not name (candidate window `0xfd400000`, survey
   first), implement the device with PING and the file-command set,
   `root=` property with canonicalisation clamped to it, and smoke it
   bare-metal: a bench-style guest that rings the doorbell and prints the
   echo over the PL011.

   *Done when:* the bare-metal guest pings the device and gets its bytes
   back over the serial port.

   **Done.**  The hole is 0xfd400000 (peri_low +0x1400000): nothing in
   the HAL's device table lives at &014xxxxx and nothing else maps there.
   `hw/misc/vmchannel.c` implements the FSDESIGN protocol — PING plus
   the file command set, paths clamped under root= by construction (a
   guest path must be `$`-rooted with plain `.`-separated components) —
   and lives behind `-global bcm2838-peripherals.vmchannel-root=DIR`.
   The smoke test `tools/vmchtest.s` (bench-harness style, 196 bytes)
   prints, over the PL011:

       48434d56   magic: the guest found the doorbell
       00000000   rc: a poisoned deadbeef was cleared by the device
       48434d56   scratch word: device -> guest write proven
       11223344   arg bytes echoed: both DMA directions work
       00000001   STATUS

   No vmstate: registers only, nothing persists between requests.

   **6B — the HostFS module (2–3 days).**  Module on the mojomod
   pattern, FS registration (`HostFS`, unclaimed number), the FSEntry
   points the desktop exercises, metadata synthesised from host mtime,
   `*HostFSPing` for the CLI.  Soft-load from `!Boot.Choices.Boot.PreDesk`
   on the card image.

   *Done when:* `*Cat HostFS:$` lists the files seeded in the root and a
   file saved from RISC OS appears on the host with sane name, type and
   date; and a `savevm`/`loadvm` round-trip with HostFS mounted leaves
   everything working.

   **6C — the extras that ride the same channel (1 day).**  Console
   capture (`*HostConsole on` → write vectors → CONSOLE commands → host
   log), time sync from host UTC at boot, and the eigen-factor/absolute-
   pointer hooks if the UI work wants them.

   *Done when:* a `*Cat` typed in the guest appears in the host log.

   **6D — HostFS on the icon bar (nice to have, low priority; not
   required for Sprint 6).**  A small `HostFSFiler` module so a HostFS
   root — a shared host folder, or a Samba share mounted on the host —
   shows on the icon bar as a disc and opens like any drive.  The
   desktop Filer already draws the windows and handles double-click and
   drag for any registered filing system through the `FSEntry_Func`
   calls the module answers, so this adds only the icon-bar lifecycle
   (`Service_StartFiler` / `StartedFiler` / `FilerDying` / `ShutDown`),
   the icon, `Filer_OpenDir "HostFS:$"` on a click, and a menu.
   RAMFSFiler is the template to copy and trim.  One module
   prerequisite: `FSEntry_Func 30` (ReadFreeSpace), fed by a new device
   command sourcing the host figure (`statvfs` / `GetDiskFreeSpaceEx`),
   or the disc's free display is blank.

   Deferred behind reliable host file transfer, which is the live 6B
   work and the immediate need: this is desktop polish on top of a
   working filing system, not a way to make one work.

   *Done when:* a HostFS root appears as a disc icon on the icon bar, a
   click opens a filer window on it, and Free reports the host's figure.

*Done when (sprint 6 as a whole):* a file written on the host is
readable from the RISC OS desktop within a second and without a
reboot, and a `*Cat` typed in the guest appears on the host.

## 13 — the blitter: render ops, sprites and the pointer on the host (5–8 days)

The Mac expansion of this sprint is designed in
[`GPUDESIGN.md`](GPUDESIGN.md) — researched against the ROOL Kernel and
BCMVideo sources and the whole `BCM2835Dev` ROM tree, which settled the
scope: the **pointer** is answered by the host as the GPU (`'DISP'`,
the `'AUDS'` pattern), the **sprite plots** are the focus (a `SpriteV`
module, RISC OS's own extension point), and the **fill** is deferred
until the project builds its own ROM, where it becomes a real blitter
device on the machine.

The render operations RISC OS actually issues, who issues them, and what
they cost today are tabulated in DESIGN.md section 14. The DMA copy path
described there, `hw/dma/bcm2835_dma.c` moving rows whole, is the baseline
this sprint replaces, and its trace point `bcm2835_dma_2d` is how to count
the copies a workload makes.

RISC OS draws with a blitter it does not have. `OS_SpriteOp` plots go
through the `SpriteV` vector to SpriteExtend, which does them in ARM code;
rectangle copies and fills go through `GraphicsV_Render`, a hook the kernel
already routes to the display driver for acceleration — `BCMVideo` hands
them to the GPU on real hardware; and the pointer, RISC OS's one true
hardware sprite, is a dispmanx overlay on real hardware and, because this
fork declines VCHIQ, is currently painted into the framebuffer by the
kernel. All three are guest-side work that the host can do at memory speed,
and all three reach the host the same way: the VM module of Sprint 6 and
its doorbell. Needs 6 and U1.

1. **The pointer, composited.** The module claims GraphicsV's pointer calls
   — shape (up to 32×32, 2 bpp, three colours and transparent, with its
   hot spot) and position — and passes them to the host. The UI keeps the
   shape as a small texture and draws it in the output pass, on top of the
   scaled frame, at the host's resolution: sharp at any scale, never in the
   framebuffer, and moving it writes no guest memory at all.
2. **Render ops.** `GraphicsV_Render` copies and fills executed by the host
   directly in guest RAM — SIMD on the host CPU first, which is already a
   hundred times the emulated rate; the compute-shader version follows once
   the screen has a GPU-side owner (below).
3. **Sprites.** `SpriteV` claimed ahead of SpriteExtend for the plot reasons
   that matter — `PlotSpriteUserCoords`, `PutSpriteScaled`, the masked and
   ColourTrans-translated variants. The host reads the sprite, its mask and
   the translation table from guest memory and blits into the framebuffer.
   Any reason or format the host does not handle falls through to
   SpriteExtend, so correctness never depends on coverage.

The "graphics kernel" itself — every blit as a shader, the screen living
on the GPU — is the step after these, and it has a precondition this sprint
makes explicit: the destination stays in guest RAM for as long as the
kernel's VDU drivers, the Font Manager and applications draw there too,
because a GPU-resident screen needs every writer to go through the host.
Sprites and render ops are the bulk of the pixels; moving them is what
makes the rest possible.

*Done when:* the pointer is drawn by the UI and absent from the
framebuffer; dragging a window across the Pinboard backdrop, and scrolling
a NetSurf page, show a measured speed-up with sprites and render ops
offloaded; and a pixel-compare test shows every fall-through case producing
exactly SpriteExtend's output.

**Windows parity, from the Mac's G0.** The Metal front end has built the
pointer half of this (`GPUDESIGN.md` section 8a). The device answers
`'DISP'` in `hw/misc/bcm2835_vchiq.c`, which is shared C and already ours,
and `ui/metal` composites the sprite through `metal_glue_cursor_view`. The
D3D11 side is the untouched twin: a `dx11_glue_cursor_view` beside
`dx11_glue_fb_view`, and a pointer-composite step in the output pass. Copy
the Metal reference for the pointer. The render-op and sprite halves are
still unbuilt on both front ends, so only the pointer has a twin to follow.

## 7 — the developer loop: roscc on the target (3–4 days)

`compiler/tools/run_on_emu.py` gains a QEMU back end: build on the host,
place the binary through Sprint 6's route, run it in the guest from a
snapshot, capture output and an exit status, tear down — the whole loop in
seconds. Then the compiler's test suite runs in the guest.

*Done when:* `run_on_emu.py --qemu hello` prints the guest's output and
returns its status in under five seconds from a snapshot, and the test suite
reports pass/fail per test.

## 8 — the debugger (5–8 days)

The gdbstub and QMP are the substrate; the work is RISC OS awareness. A
debugger pane in the window (or a sibling tool speaking the same sockets):
registers, memory, disassembly with capstone, breakpoints and single-step;
the module list and the Wimp task list read through the Sprint 6 module;
symbols from the compiler's output so breakpoints can be set by name in a
freshly built module or application; abort and error capture — a data abort
in the guest lands in the debugger with the faulting instruction, not on a
RISC OS error box; a SWI trace through a TCG plugin, filtered by SWI number.

*Done when:* a breakpoint set by symbol in a roscc-built program stops at
the right place, the stack and registers are readable, and an intentional
null-pointer read in that program is caught with its address.

## 9 — speed, measured against a Pi 4 (4–5 days, in parallel)

A benchmark set that runs in both places — a real Pi 4 and the emulator:
BASIC loops, a Dhrystone-class C program built with roscc, file copies on
SDFS, a NetSurf page load. Numbers from hardware, then the emulator's
profile: the 1 ms stall per SD block that `DESIGN.md` §12 measured but did
not explain; the USB SOF FIQ's 25 register accesses a millisecond; TCG's TB
flushes and the TLB-walk share; MMIO fast paths for the registers RISC OS
reads most (the system timer, GINTSTS). Fix what the profile shows, one
change per measurement.

*Done when:* every benchmark has a hardware number, an emulator number and a
ratio in the README, and at least one measured stall is gone.

## 14 — performance analysis and review (4–5 days)

Sprint 9 measures and fixes what it finds; this one steps back and does
the analysis properly, once the window, the blitter and the snapshots
exist, because they change what the machine spends its time on. It is a
report before it is a change.

**Tooling first, scripted so it can be re-run.** The instruction-count
plugin already built (`tests/tcg/plugins/libinsn`) for the retired rate;
`libhotblocks` and `libhotpages` from the same directory for which guest
code and pages are hot, mapped to ROM modules with the chain table the
profiler already uses; the PC sampler from `DESIGN.md` §12 for the
wall-clock share by module; `-d` trace counts per device for the MMIO trap
rate; host-side profiling of the process (ETW through Windows Performance
Recorder, or VTune) for where the host threads spend their time — TCG
translation versus execution, the memory dispatch, the BQL, the main loop.

**The analysis**, each with a number and a share of wall clock or of CPU:

- guest instruction mix and the TB translation rate (how much is
  re-translation after TB flushes);
- the TLB-walk share — the 16 % seen in one profile is either real or an
  artefact, and it matters which;
- MMIO trap rate per device, and the cost per trap: the system timer,
  `GINTSTS`, the SD buffer port, the framebuffer;
- interrupt rates — the 1 kHz SOF FIQ and its hand-off IRQ, the SD
  interrupts, the tick — and the guest time each handler takes;
- the millisecond stall per SD block from §12, run to ground with the host
  profiler now that the trace has said where to look;
- main-loop wake-ups and timer deadlines, and the BQL's contention profile
  between the UI, the main loop and core 0;
- the UI thread's frame cost, and the blitter's gains from Sprint 13,
  measured against the same Wimp workloads before and after;
- the whole against the Pi 4 numbers from Sprint 9, per benchmark.

**The review.** A written `PERF.md`: the method, the numbers, the top five
costs ranked by what they take, each with a proposed fix and the gain to
expect, and an honest answer to the question the project started with —
where TCG's ceiling is for this workload, and whether a purpose-built
recompiler for a single Cortex-A72 running RISC OS would beat it by enough
to be worth building. Reviewed together before anything in it is started.

*Done when:* the top five costs are quantified with their share of the
whole, each has a fix and an expected gain written down, the scripts
reproduce the numbers on a clean checkout, and the review has happened.

## 10 — record and replay (3 days)

`-icount shift=auto,rr=record` with the card behind an overlay and the
network through slirp: a boot and a session recorded to a file and replayed
bit-for-bit; the debugger from Sprint 8 attached to a replay, so a bug seen
once can be stepped through as often as needed.

*Done when:* a recorded session replays to the same screenshot, and a
breakpoint during replay stops at the same instruction every time.

## 11 — give back (3 days)

The generic fixes go to qemu-devel as a series, each with the guest-neutral
justification the commit messages already carry: the AArch32 secondary boot
stub, the I2C byte access and TX FIFO, the system timer to the GIC, the SD
card on EMMC2, the GIC legacy-FIQ input, the DWC2 HCHINT and IRQ-level fixes,
`usb-net rndis=off`, the SD read-ahead, mailbox channel 0, the property tags.
The fork rebases onto the next QEMU release; the RISC OS-specific pieces
(VCHIQ peer, EDID answer, the BCM2711 legacy controller) stay in the fork
until someone upstream wants a Pi 4 that boots more than Linux.

*Done when:* the series is on the list and the fork's own patch count is
smaller than when the sprint began.

## 12 — packaging, and the badge (2–3 days)

A zip with the executable, its DLLs, the tools and the documents; a
one-page quick start that says where to get the ROM and the image and what
`mkcmos` does; the checklist the badge stands on, ticked or not:

- boots the ROOL image to the desktop in one command — yes
- keyboard, mouse, network, 800×600 — yes
- native window, GPU-drawn — U1
- desktop in under a second — Sprint 5
- host files and the developer loop — 6, 7
- the debugger catches a fault by symbol — 8
- the pointer composited and sprites blitted by the host — 13
- speed measured against hardware, and the gap understood — 9, 14

*Done when:* the checklist has no gaps, and the badge turns green.

---

## 15 — frame coherence: no torn or half-moved windows (2–3 days)

What is seen: brief tearing, and windows that sit in the wrong place for a
frame during a drag. Where it comes from: the UI thread copies the guest
framebuffer once per host frame without any lock, while the vCPU thread is
drawing into it and, since the DMA fix, moving whole windows through it in
one instantaneous 2D copy under the BQL. A copy that starts mid-move shows
a window half in each place. RISC OS itself never double-buffers the Wimp,
so its intermediate states are real, but on a Pi they last a scanout; here
the guest draws two to three times slower and the capture is unsynchronised,
so they are seen.

- The frame is captured on the timer thread at the guest's vsync, into a
  staging buffer, *before* the vsync interrupt is raised, with the BQL
  held: the DMA copy cannot interleave, and anything RISC OS does "at
  vsync" (pointer redraw, palette and screen-start updates, a game's
  bank swap after OS_Byte 19) lands after the capture, so bank-switched
  games are tear-free by construction. The UI thread uploads the staging
  buffer at the next host vsync.
- Capture rate follows the vsync rate; 60 by default once this is in, so
  the window shows every guest frame the guest could have shown a monitor.
- The pointer stops being drawn into the framebuffer: Sprint 13's host
  sprite (the mailbox cursor tags answered for real) removes the pointer
  save-and-restore, which is the other visible flicker.
- Measured: a torn-frame counter in the debug log (the capture compares a
  row checksum before and after) reads zero over a drag session.

Done when a window dragged across the desktop is never seen in two places,
and a bank-switching game shows no tear.

## 16 — modes: what the Display Manager is allowed to see (2 days)

RISC OS builds its mode list from the monitor's EDID, filtered by what
BCMVideo can drive, and BCMVideo drives any size the GPU scales. Today the
EDID the property channel answers with carries one detailed timing
(800×600) and two established timings, so the Display Manager offers three
sizes. The window's decoders cover every depth in the firmware contract;
the guest driver asks only for 8, 16 and 32 bpp, and that is BCMVideo's
choice, not a limit of ours.

- The EDID becomes data: a table of modes (640×480 through 3840×2160,
  including 1280×720, 1280×800, 1366×768, 1440×900, 1600×1200, 1920×1080,
  1920×1200, 2560×1440) rendered into the base block's standard timings
  and detailed timings and a CTA-861 extension block for the rest, with
  checksums computed, and block 1 answered by GET_EDID_BLOCK.
- The preferred timing is a setting (Sprint 17): it is the desktop's
  default mode at boot. `-display dx11,mode=1920x1080` for the command
  line.
- The window's default size follows the preferred mode times the DPI
  scale, so a 1:1 desktop is readable on a scaled display.
- Verified with the Display Manager's list and a mode change into each
  size, and the pipeline log showing the new pitch.

Done when the Display Manager lists the table and every entry displays.

**Windows parity, from the Mac's wide modes.** The Metal decode reads the
framebuffer as a plain `device const uchar *` buffer indexed by pitch,
which removed the texture-width ceiling on the wide entries above. The
D3D11 decode reads a `Texture2D<uint>` of `R8_UINT`, so a 3840-wide 32bpp
mode is a 15360-byte pitch, just inside D3D11's 16384 texture limit, and
anything wider overflows it. Move the decode source to a `ByteAddressBuffer`
indexed by pitch, the twin of the Metal read, before the table reaches past
3840 wide. The EDID table is device-side, so mode work done for the Mac
lands here too; presenting the wide modes is the front-end task that stays
ours.

## 17 — settings: one window, and a page that explains itself (4–5 days)

The main window stays minimal: no toolbars, no panes. A key chord
(Ctrl+Alt+S, alongside Ctrl+Alt+G and Alt+Enter) or the system menu's
"Settings…" replaces the guest view with a full-client settings page while
the machine keeps running; the same chord or Escape brings the desktop
back. Large type, sections down the left, one setting per row with a line
of help beside it, and a mark on anything that needs a restart.

- The settings model is a JSON document (`%APPDATA%\RISCOSQEMU\settings.json`)
  that the launcher and the emulator both read: machine (RAM, ROM, CMOS
  choices via mkcmos), display (mode, window size, fullscreen, scaling,
  scanlines, vsync rate), input (grab policy, key chords, tablet or
  mouse), storage (card image, overlay, snapshots), network (user-mode,
  port forwards, HostFS root), diagnostics (fps and MIPS in the title,
  logs). Tier two drives the same document.
- The page is drawn in the existing D3D11 swap chain (Dear ImGui, MIT,
  with its Win32 and DX11 backends), so there is still one window and no
  second toolkit; live settings apply at once, restart settings offer
  "Apply and restart", which relaunches with the new command line.
- Every setting has its help text in the model, so the page and the
  command-line reference are generated from one source.

Done when a new user can pick a mode, a card image and a HostFS folder
without the command line, and the page reads as help rather than as a
form.

## 18 — sound: the audio path, on Windows (1–2 days)

Sound is designed and built device-side in [`SOUND.md`](SOUND.md):
`hw/misc/bcm2835_vchiq.c` answers the `AUDS` service, bulk-receives the
sample stream, and plays it through QEMU's own audio backend, clocked by
what the backend drains (the `COMPLETE`-is-the-clock trick). None of it is
front-end code. It uses `DEFINE_AUDIO_PROPERTIES` and `audio_be_*`, so the
backend is chosen with `-audiodev`: `coreaudio` on the Mac, and `dsound`
on Windows, already in our build. For us this is wiring and verification,
not new code.

- Reach the vchiq peer's `DEFINE_AUDIO_PROPERTIES` from a Windows backend:
  `-audiodev dsound,id=a0` in `riscos-pi4/tools/run.py`, with a backend
  choice added to the settings model (Sprint 17).
- Verify a RISC OS sound reaches the Windows speakers: a `Maestro` tune or
  a sampled sound, and confirm the priming burst (`SampleRate/(BuffSize*5)`,
  forced even, minimum two) does not stall at the start of every sound.
- `AUDIODEV=none` stays silent rather than an error, the fallback
  `SOUND.md` section 12 already built.

Done when a RISC OS sound plays on Windows through `dsound`, and a machine
started with no audio backend still boots and runs silently.

## Tier two, noted for later

The IDE-embedded developer emulator reuses everything above: the settings
document, QMP as the control plane, the snapshot menu, and a window that
can render into a child HWND or a shared DXGI surface the IDE composes.
Nothing in tier one should assume it owns the top-level window.

## Days, added up

U0–U4: 12–14 days. Sprints 5–8: 15–20. Sprint 13: 5–8. Sprint 9 runs
alongside; 14 takes 4–5 after 9 and 13. 10–12: 8–9. About ten working
weeks for one person doing nothing else, which is not how it will go; the
order above is what matters, not the arithmetic.
