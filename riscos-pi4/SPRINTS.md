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
                 └─► 6 host files ────────┘
U3, U4 and 9 slot in wherever a week has room; 11 and 12 come last.
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

## U2 — keyboard and mouse (3 days)

Scan codes to qcodes through the win32 keymap already in the tree; repeats
dropped; grab, the `win32-kbd-hook.c` hook and Ctrl+Alt+G release;
raw-input relative mouse, three buttons as Select/Menu/Adjust, wheel; an
afternoon on `usb-tablet` to learn whether RISC OS's HID driver takes an
absolute pointer.

*Done when:* typing at the RISC OS command line and dragging a window both
feel like the real thing, and the host gets its pointer back on demand.

## U3 — the rest of the formats, and polish (2–3 days)

16 bpp RGB565, 24 bpp and the packed low-depth decoders; sharp-bilinear and
the scanline option; borderless fullscreen; DPI awareness; a status line
with mode, frame rate and instruction rate; WARP fallback where there is no
GPU.

*Done when:* every row of the shader table in `UI-DESIGN.md` has been seen
working — RISC OS's own formats on real data, the rest on synthetic buffers.

## U4 — measure, then dirty rows if needed (2 days)

Profile the UI thread against the guest's instruction rate; add dirty-row
uploads through the memory dirty bitmap only if the numbers ask for it;
document what a frame costs.

*Done when:* instruction-rate figures with the window open match those
without.

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

## 6 — host files (4–5 days)

Two routes, in order of cheapness:

1. **LanManFS over slirp.** RISC OS ships an SMB client; slirp's NAT puts
   the host at `10.0.2.2`. If RISC OS 5.30's LanManFS talks to a Windows
   share (its SMB2 support is the thing to verify), host files arrive with
   no guest code at all. One day to find out, and if it works this is the
   answer for the developer loop.
2. **The VM compatibility module.** A doorbell device in QEMU at an address
   the HAL does not name, and a RISC OS module that lives on the card image
   in `!Boot.Choices.Boot.PreDesk` — loaded by the boot sequence, so no ROM
   splice — offering: host filing system (`HostFS:`), console capture
   (`*` output back to the host), absolute pointer, the mode's eigen
   factors for the UI, clipboard, and time sync. The ROM-splice recipe in
   `DESIGN.md` stays for disc-less boots.

*Done when:* a file written on the host is readable from the RISC OS
desktop within a second and without a reboot, and a `*Cat` typed in the
guest appears on the host.

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
- speed measured against hardware, and the gap understood — 9

*Done when:* the checklist has no gaps, and the badge turns green.

---

## Days, added up

U0–U4: 12–14 days. Sprints 5–8: 15–20. Sprint 9 runs alongside. 10–12: 8–9.
About eight working weeks for one person doing nothing else, which is not
how it will go; the order above is what matters, not the arithmetic.
