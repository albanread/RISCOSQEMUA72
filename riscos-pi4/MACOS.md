# The macOS port — what moved, what did not, and what was measured

The fork was written on Windows: MSYS2 for the build, Direct3D 11 for the
window, a Win32 waitable timer for the clock. This is the record of taking
it to macOS on Apple silicon — what turned out to be portable, what had to
be written again, and the numbers that decided the difference.

Short version: **the emulation was already portable and the front end was
not.** Every RISC OS-specific thing in the fork — the mailbox peers, the
VCHIQ handshake, the interrupt controller, the DMA 2D copies, the DWC2
fixes, the EDID answer, the vsync generator — is plain C against QEMU's
own APIs, and none of it needed a line changed. What needed writing was a
window: `ui/metal.{h,c,m}`, the twin of `ui/dx11.{h,c,cpp}`.

## 1. The baseline, before any macOS code was written

An unmodified checkout, configured with `--enable-cocoa` and built with
Apple clang, boots RISC OS 5.30 from the ROOL SD image to the 800×600
desktop with NetSurf on the Welcome page. Power-on to a painted desktop is
**under 20 seconds** on an M4 (the README's Windows figure is about 27 on
an i7-12700); the desktop was already up at the first 20-second sample, so
the true figure is lower and has not been narrowed.

Networking works from the first boot with nothing added: `EtherUSB` binds
the CDC configuration of `usb-net,rndis=off`, slirp's DHCP server answers
during `!Boot`, and NetSurf fetches over it. The README's caveat that the
networking claim "predates the window work and wants re-verifying against
a fetch" is answered here — it was verified against one.

The fork's own RISC-OS-free regression test passes unchanged:

```
$ qemu-system-aarch64 -M raspi4b -cpu cortex-a72,aarch64=off \
      -kernel mboxtest.bin -display none -serial stdio -serial null
00000080
```

which is the mailbox channel-0 reply. Stock QEMU prints `TIMEOUT`.

`riscos-pi4/tools` is portable Python and needed nothing: `fat.py` read the
`CMOS` file out of the card image's boot partition and `mkcmos.py` built
the blob from it, resolving `EtherGENET` to chunk 106 out of the ROM's own
module chain, exactly as documented.

So the port is not a port of the emulator. It is a front end.

## 2. What was platform-specific, and what only looked it

| Piece | Verdict |
| --- | --- |
| `hw/misc/bcm2835_vchiq.c`, `bcm2835_mbox_power.c`, `bcm2835_vsyncgen.c`, `hw/intc/bcm2838_ic.c`, and every device fix in the README's table | Portable. No `#ifdef _WIN32` between them |
| `riscos-pi4/tools/*.py` | Portable |
| `scripts/symlink-install-tree.py` | Already `os.name == 'nt'`-gated; a no-op here |
| `system/hrtimer.c` | Had a POSIX fallback all along — see §5, where it is measured rather than assumed |
| `ui/dx11.{h,c,cpp}` | Windows only, and the whole of the work |

## 3. `-display metal`: the same program, in the platform's idiom

The Windows front end's design note says "`ui/cocoa.m` is the precedent for
every rule below". That turned out to be literally true: the main-thread
hand-off it was built on — `system/main.c` runs `qemu_init()` on the main
thread, and a display backend that sets `qemu_main` gets the main thread
while the emulation moves to its own — exists *because* the Cocoa port
needed it. Nothing in `system/main.c` was touched.

So the shapes correspond one for one:

| Windows | macOS |
| --- | --- |
| `ui/dx11.h` — eleven `extern "C"` calls | `ui/metal.h` — the same eleven |
| `ui/dx11.c` — QEMU-side glue, C | `ui/metal.c` — the same glue |
| `ui/dx11.cpp` — Win32 + D3D11, C++ | `ui/metal.m` — Cocoa + Metal, Objective-C |
| `D3DCompile` on an HLSL string at start-up | `newLibraryWithSource:` on an MSL string at start-up |
| `D3D_SHADER_MACRO` per pixel format | `MTLFunctionConstantValues` per pixel format |
| Dynamic texture + `Map(WRITE_DISCARD)` | `MTLBuffer`, shared storage, a three-deep ring |
| Waitable flip-model swap chain | `CAMetalLayer`, `displaySyncEnabled`, `nextDrawable` |
| `PeekMessage` pump | `nextEventMatchingMask:untilDate:distantPast` pump |
| AT set 1 scan codes → `qemu_input_map_atset1_to_linux` | macOS virtual key codes → `qemu_input_map_osx_to_linux` |
| `ClipCursor` + `SetCursorPos` | `CGAssociateMouseAndMouseCursorPosition(false)` + `CGWarpMouseCursorPosition` |
| Hand-rolled PNG over zlib | ImageIO, which is already there |
| System menu item | An `NSMenu` |
| `ShowCursor(FALSE)` while grabbed | An `NSTrackingArea`, and hidden over the guest's screen at all times |
| Three mouse buttons | Control-click is Menu, Command-click is Adjust |

The C++ boundary on Windows exists because QEMU's headers are not
C++-parseable. Objective-C has no such problem — `ui/metal.m` could have
included `qemu/osdep.h` and done without `ui/metal.c` entirely. The split
is kept anyway, because that eleven-call contract is what makes the
Windows front end reviewable, and a front end that can be read beside its
twin is worth more than one file fewer.

### The shaders

The two passes are the same two passes: decode the guest's raw bytes to
linear RGB at the guest's own resolution, then stretch that over the whole
content view, because the window is the monitor. The decode handles 32,
24 and 16 bpp and the palettised cases (8 bpp is the one RISC OS asks for;
1/2/4 with LSB-first packing ride along), and the scaler offers plain
bilinear, sharp bilinear (the default) and nearest, with an optional
scanline mask at 2× and above.

HLSL to MSL is close to transliteration. Two differences worth naming:

- **Where the bytes live.** D3D11 reads the raw framebuffer as a
  `Texture2D<uint>` of `R8_UINT`, pitch wide. Metal reads it as a
  `device const uchar *` — a plain buffer, indexed with the pitch. That
  removes the texture-width ceiling on wide modes, and on Apple silicon
  the "upload" is a `memcpy` into memory the GPU already sees.
- **How the variants are made.** `#if BPP == 32` and a separate compile
  per format becomes `constant uint BPP [[function_constant(0)]]` and a
  specialised `MTLFunction` from one library. Same result, less machinery.

### One pointer, not two

The guest device is an absolute tablet, so ungrabbed the guest's arrow
sits exactly under the host's — which on Windows means two identical
arrows on top of each other and nobody notices. On macOS the two arrows
are different shapes, and the doubling is obvious.

So the host cursor is hidden by a rule rather than by the grab: it is
hidden whenever the guest is drawing the pointer under it, which is any
time the pointer is over the view in a key window, grabbed or not. An
`NSTrackingArea` makes entering and leaving the guest's screen an event;
losing focus, ungrabbing and shutting down all put the cursor back.
`[NSCursor hide]` is a counted call, so the state is tracked and the call
made only on a change — an unbalanced pair is a cursor that never returns.

### Three buttons on a machine with one

RISC OS wants Select, Menu and Adjust. A MacBook has one button, so
Control-click is Menu and Command-click is Adjust (Option and Shift do
Adjust as well).

Two things about that are not obvious. macOS turns a Control-click into a
right click *before* the view sees it, so Menu has to be recognised on
that stream too — and since the modifier can be released before the
button is, the button chosen is latched at press time and that is what
gets released.

The third only showed up when Control-click worked and Shift-click did
nothing at all. A modifier used to pick a button was still being sent to
the guest as a key, so RISC OS saw Shift *and* Adjust, which is a
different gesture from Adjust — the click was arriving and being read as
something else. Control-Menu happens to be harmless, which is why one of
them worked. The qualifying modifier is now taken away from the guest for
the duration of the click and handed back on release if it is still held.

Command is the one to reach for: RISC OS has no Command key, so nothing
is lost to it, whereas Shift-Select and Control-Select are gestures the
Filer really uses.

### The mistake that cost the first run

The first version skipped rendering whenever
`[window occlusionState] & NSWindowOcclusionStateVisible` was clear,
reasoning that an invisible window wants no frames. The machine booted,
the Metal device came up, and not one frame was ever drawn. `sample`
found the main thread parked in the `usleep` of that branch, and a
one-shot log of the window's actual state said why:

```
window: visible 1, miniaturized 0, key 0, occlusion 0x2000, drawable 1600x1200
```

Visible, on screen, 1600×1200 of drawable behind an 800×600 window — and
`occlusionState` without the visible bit. Occlusion is not a question
about whether a window exists, and it is not the right gate. It is
`isMiniaturized` and `isVisible` that are, with
`allowsNextDrawableTimeout` doing the rest: an unseen window makes
`nextDrawable` return nil rather than block for ever, which is the same
protection the Windows front end gets from waiting on the swap chain's
frame-latency object instead of calling `Present` blind.

## 4. Verifying a window without a camera

A wrong decode shader still presents a frame, so "it runs" proves nothing
about the picture. `METAL_SHOT_EVERY=<frames>` writes the decoded surface
— the shader's own output, read back off the GPU — to a PNG on that
period. That is what the screenshots in this port were checked against,
and it is the same path `⌘S` and F13 take. It is off unless asked for.

The periodic log line carries the mode, the frame rate and the input
counters, so a keyboard or mouse that is not arriving says so:

```
frame 16200: pipeline up, fb gen 10, 800x600 bpp 32, keys 67, mouse moves 448, button events 6
```

`METAL_DEBUG=1` goes further and traces every key event the pump sees,
with the routing that decides where it lands. That is the difference
between "the app never got the keystroke" and "the view never got it",
and it is not a question a counter can answer:

```
pump: key event type 10 code 17, our window 1, app active 1, key win 1, first responder MetalView
view: keyDown code 17
```

## 5. The clock: measured, then left alone

The README's Windows claim is *timers are exact*: 100.0 ticks a second
with a worst gap of 11.6 ms over an idle desktop, from a high-resolution
waitable timer in `system/hrtimer.c`. That file's `#else` branch — a
`GCond` wait on a microsecond deadline — is what macOS gets.

The temptation was to replace it with `mach_wait_until` on a
time-constraint-scheduled thread, which is what real-time audio does on
this platform. It was measured first, by counting the system timer's
compare-1 expiries (`riscos-pi4/tools`' trace point
`bcm2835_systmr_timer_expired`) over a settled desktop:

```
timer #1:  100.0/s over 30.0s   gap median 9.96 ms, p99 12.38 ms, worst 12.54 ms
```

100.0 a second, worst gap 12.54 ms, and that was measured with a *second*
full machine running in a window on the same Mac. Against 11.6 ms on
Windows, on an idle host, there is nothing here worth the machinery. The
POSIX branch stands, and `mach_wait_until` stays on the list of things not
needed yet.

## 6. What is not done yet

- **Apple Events: Sprints E0 and E1 are built.** `ping` and `describe`
  answer over the wire from the properly-launched app —
  `riscos-pi4/tools/run-app.sh` (an `open --args` launch, **no QMP**:
  the app persona's only always-on channel is Apple Events) over the
  bundle `make-bundle.sh` assembles. The command table in `ui/metal.c`
  is the one source of truth; `mksdef.py` generates the dictionary
  from it and `describe` returns it as JSON. `SCRIPTING.md` §13–14
  record the measurements (16.7 ms per event, QMP 0.1 ms) and the wire
  lessons. The control and debug commands are E2–E3.
- **The app has its icon.** A silver cog on a navy rounded-square tile,
  drawn entirely from geometry by `riscos-pi4/tools/mkicon.py` (original
  artwork — the repository carries no third-party material — and a nod to
  RISC OS's cog branding), packed into `riscos-pi4/app/AppIcon.icns` and
  wired through `CFBundleIconFile` and `make-bundle.sh`.
- **Grab, full screen and the snapshot menu are written but unexercised.**
  ⌃⌥G, ⌃⌘F and Load Snapshot have not been driven yet.

  Everything else in the input path has been. The pointer, the buttons and
  a window drag went through against a running desktop — the drag is also
  the DMA 2D negative-stride copy path, since the whole window redrew
  rather than the exposed strips. So did the keyboard, into StrongED: the
  letters, Space, Return, Shift for a capital, and five Deletes correcting
  a word, all of which is `qemu_input_map_osx_to_linux` being faithful
  from an `NSEvent` keyCode through to RISC OS.
- **No `-display metal` equivalent of the Windows DPI dance.** The layer
  is sized in backing pixels and the guest is scaled to the whole view, so
  a Retina display shows the guest at the panel's own resolution — but
  moving a window between displays of different scale has not been tested.
- **`vsync=` is a display suboption, as on Windows.** The generator is a
  qdev property (`-global bcm2835-vsyncgen.hz=N`), so `-display cocoa`
  can have the right rate too; that is how the Cocoa fallback was run.
- **The instruction rate is not chased**, only measured; see §8.

## 7. Building and running on macOS

Apple clang, and Homebrew for the rest:

```bash
brew install meson ninja pkgconf glib pixman capstone libslirp libpng

mkdir build-macos && cd build-macos
../configure --target-list=aarch64-softmmu --enable-plugins --disable-werror \
    --disable-gtk --disable-sdl --disable-vnc --disable-docs \
    --disable-guest-agent --enable-capstone --disable-spice --enable-slirp \
    --enable-cocoa \
    --cc=/usr/bin/clang --cxx=/usr/bin/clang++ --objcc=/usr/bin/clang
ninja
```

`--enable-metal` is not needed: the feature is `auto` and finds its
frameworks on any macOS. Pass Apple's clang explicitly — a Homebrew clang
on `PATH` will be picked up otherwise, and the Objective-C in `ui/metal.m`
wants the system compiler and the system SDK to agree.

The ROM, the card image and the CMOS blob are exactly as the README
describes them; `riscos-images/run-macos.sh` is that launch line with
`-display metal` in place of `-display dx11`:

```bash
qemu-system-aarch64 -M raspi4b -cpu cortex-a72,aarch64=off \
    -kernel RISCOS.IMG \
    -device loader,file=cmos.bin,addr=0x510000,force-raw=on \
    -drive file=card.img,if=sd,format=raw,snapshot=on \
    -netdev user,id=n0 \
    -device usb-hub,bus=usb-bus.0,port=1 \
    -device usb-kbd,bus=usb-bus.0,port=1.1 \
    -device usb-tablet,bus=usb-bus.0,port=1.2 \
    -device usb-net,netdev=n0,rndis=off,bus=usb-bus.0,port=1.3 \
    -display metal,vsync=30 \
    -qmp tcp:127.0.0.1:4455,server,nowait
```

`-display metal` takes `vsync=N` (default 30), `scaling=linear|sharp|nearest`
(default sharp) and `scanlines=on|off`, the same three the Windows front
end takes. In the window: ⌃⌥G grabs and releases the pointer, middle click
grabs, ⌘S or F13 writes `metal-screenshot-N.png`, ⌃⌘F is full screen, and
closing the window powers the machine down cleanly. Failures land in
`metal-debug.txt` next to the process.

`-display cocoa` remains as a fallback: it goes through QEMU's own display
path, converting the framebuffer on the CPU, and it works.

## 8. Speed, measured

Best of three for the benchmarks, and one run of the boot, all on an M4
with a *second* full machine running in a window on the same host — so
every macOS figure here is a floor, not a best case. The Windows column
is the README's, on an i7-12700.

| | i7-12700 | M4 | |
| --- | --- | --- | --- |
| Power-on to a screen that stops changing | ~27 s | **20.8 s** | 1.3× |
| Centisecond ticker, worst gap | 11.6 ms | 12.5 ms | — |
| `bench` — 2 instructions, registers only | ~2000 M/s | 1673 M/s | 0.84× |
| `bench2` — 6 instructions, a load and a store per iteration | ~500 M/s | 895 M/s | 1.8× |

The split is the interesting part. A two-instruction register loop is
almost pure TCG dispatch, and the i7's clock wins it. Add a load and a
store — which is what real code does — and the M4 is nearly twice as
fast. The boot, which is device emulation, block I/O and the Wimp rather
than a spin loop, goes with the second number.

`riscos-pi4/tools/ticks.py` measures the ticker on either host with the
same instrument. The boot figure came from sampling `screendump` over QMP
twice a second and taking the moment the image stopped changing for three
seconds; there is no tool for it yet.
