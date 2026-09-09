# A native Windows front end for the emulated Pi 4 — design and sprints

The QEMU fork now boots RISC OS 5.30 from the SD image to a networked
800×600 desktop, with a keyboard and mouse. What it shows that on is QEMU's
own display code, which converts the guest framebuffer on the CPU and hands
it to GTK or SDL. This is the design for replacing that with a window we own:
Direct3D 11 draws the guest's framebuffer straight from guest memory through
a shader, the UI thread owns the window and turns Windows input into the
guest's USB keyboard and mouse, and the emulation runs on its own threads in
the same process.

## 1. Shape

```
  process: qemu-system-aarch64.exe, -display dx11
  ┌─────────────────────────────────────────────────────────────────────┐
  │ main thread (UI)                                                    │
  │   Win32 window + message loop                                       │
  │   D3D11 device, swap chain, per-frame: read fb config → upload raw  │
  │   framebuffer bytes → decode shader → scale shader → Present        │
  │   WM_INPUT / WM_KEY* → qemu_input_* (under the BQL, briefly)        │
  ├─────────────────────────────────────────────────────────────────────┤
  │ "qemu_main" thread: QEMU main loop — timers, BHs, slirp, block I/O  │
  ├─────────────────────────────────────────────────────────────────────┤
  │ vCPU threads ×4 (MTTCG) — the Cortex-A72s                           │
  └─────────────────────────────────────────────────────────────────────┘
        guest RAM (host memory) ──── read directly by the UI thread
        bcm2835-fb state ─────────── config polled by the UI thread
```

**In-process, by hand-off.** There is no libqemu; QEMU is a program. But it
has, since the Cocoa port needed it, a documented way for a display backend
to take the main thread: `system/main.c` runs `qemu_init()` on the main
thread, and if the backend has set the `qemu_main` function pointer it moves
the main loop onto a new thread and calls `qemu_main()` instead. That is
exactly the model asked for — the UI thread owns the window, the emulation
lives on other threads — and it is the same process, so the framebuffer is a
pointer, not a copy. `ui/cocoa.m` is the precedent for every rule below.

**Language.** QEMU is C. The build already enables C++ on Windows
(`enable_cpp = host_os == 'windows'` in `meson.build`), so the front end is
one C++17 translation unit, `ui/dx11.cpp`, with `extern "C"` glue at the
QEMU boundary. Direct3D from C is possible and miserable; from C++ it is
ordinary.

**Out-of-process fallback.** If the in-process route ever fights QEMU too
hard, the same UI can live in its own executable: QEMU runs with guest RAM
in a shared-memory backend, the UI maps the same file and reads the
framebuffer from it, input goes over QMP `input-send-event`. It costs a copy
of nothing and gains crash isolation, but loses the ability to poke QEMU's
state directly, which the debugger side will want. Start in-process.

## 2. Threads, and who may touch what

| Thread | Owns | Touches QEMU state |
| --- | --- | --- |
| UI (main) | window, D3D11, input state, grab | reads guest RAM and the fb config without locks; takes the BQL only to inject input and for menu commands |
| qemu_main | the main loop | everything, as today |
| vCPU ×4 | TCG | everything, as today |

Rules:

- The UI never holds the BQL while rendering. A frame is: read the fb
  config (ten words), read the framebuffer bytes, upload, draw. None of it
  needs a lock; the guest writes its screen under no lock either, and a torn
  frame is exactly what a real monitor shows mid-update.
- Input injection — `qemu_input_event_send_key_qcode`,
  `qemu_input_queue_rel/btn/abs` — happens under the BQL, wrapped exactly as
  `cocoa.m`'s `with_bql` does: lock if not held, call, unlock. Microseconds.
- Mode changes are detected by polling: the fb device gets a generation
  counter that the property channel bumps when it commits a new config; the
  UI re-reads the config when the counter moves, so it never sees half a
  config.
- Shutdown: closing the window calls `qemu_system_shutdown_request()`; the
  main loop thread tears QEMU down and the process exits, as with any
  backend. The UI thread just keeps pumping messages until then.
- `qemu_init()` still runs on the main thread before the hand-off, so all
  the parsing, device creation and `-device loader` work is unchanged.

## 3. The display pipeline

The BCM2835 framebuffer device holds `BCM2835FBConfig`: `xres, yres`
(what the guest calls the physical size), `xres_virtual, yres_virtual,
xoffset, yoffset` (the buffer and the pan), `bpp`, `base` (bus address of
the buffer), `pixo` (1 = RGB, 0 = BGR byte order) and `alpha`. The palette
that the guest programs through the `SET_PALETTE` tag lives in guest memory
at the VideoCore RAM base, 256 × 32-bit `0x00BBGGRR`. Everything the shader
needs is therefore in guest memory or ten words of device state.

Per frame, on the UI thread:

1. **Config.** Copy the fb config if the generation moved; validate it
   (bpp ∈ {1,2,4,8,16,24,32}, sizes within the RAM, pitch = `xres_virtual ×
   bpp / 8`). Translate `base` to a host pointer once per config through
   `address_space_map` on the guest's DMA address space (the bus address
   carries the 0xC0000000 alias bits; the fb device already knows how to
   strip them).
2. **Upload.** Copy `pitch × yres` bytes into a dynamic `R8_UINT` texture of
   width `pitch`, height `yres` (`Map` with `WRITE_DISCARD`, memcpy, `Unmap`).
   800×600×4 is 1.9 MB, 115 MB/s at 60 Hz — nothing. The palette is a second
   256×1 `R8G8B8A8` texture, uploaded when its bytes change.
3. **Decode.** A full-screen pass whose pixel shader reads the bytes of pixel
   `(x, y)` from the raw texture and produces linear RGB into an
   `R8G8B8A8_UNORM` render target the size of the visible framebuffer
   (`xres × yres`, from `xoffset, yoffset` — the pan is a texture-coordinate
   offset).
4. **Scale.** A second pass draws that target into the window: aspect
   preserved, letterboxed, integer scaling when the window allows it,
   nearest or sharp-bilinear filtering, optional scanline overlay.
5. **Present** through a DXGI flip-model swap chain with a waitable object;
   vsync on by default (the guest has no vsync of its own to fight).

Steps 3 and 4 are separate so that sprite work later — a RISC OS
`SpriteExtend` offload has been floated — has a decoded, linear surface to
compose onto, and so that the scaler can be swapped without touching the
decoders.

### 3.1 What RISC OS actually asks for

`BCMVideo`'s GraphicsV driver advertises **8 bpp and 32 bpp** to the kernel
(`s/GraphicsV`: "8,32bpp supported. 16bpp is RGB565"), so every RISC OS mode
on a Pi is one of those; the classic 1/2/4 bpp numbered modes are run as
8 bpp buffers with their palette. 32 bpp is `&TTBBGGRR` in memory, which the
GPU calls RGB order, unless `framebuffer_swap` flips it — the driver tracks
`FBSetPixelOrder EOR framebuffer_swap`. The ROOL image's `config.txt`
carries `framebuffer_swap=0`. 16 bpp, where the driver enables it, is
RGB565. 24 bpp never appears. YUV overlays go through dispmanx over VCHIQ,
which this fork declines, so they cannot appear either.

RISC OS sets the GPU's physical size equal to the mode's size (the
640×256 desktop came out as a 640×256 console), and on real hardware the
GPU's scaler stretches that to the monitor. In this fork there is no
monitor; the window is the scaler. Non-square modes — 640×256 is a 4:3
picture — therefore need a pixel-aspect decision that the framebuffer alone
does not carry. Heuristic first (a buffer wider than 2:1 gets 2:1 pixels),
and later the VM compatibility module can tell the UI the mode's eigen
factors directly.

### 3.2 Shaders, per format

One HLSL source, compiled into one pixel shader per format with `#define`s
through `D3DCompile` at start-up (no build-time tooling), selected by the
config's `bpp`/`pixo`. The constant buffer carries `width, height, pitch,
pan, swizzle, palette-present`. The set:

| Format | Who uses it | Decode |
| --- | --- | --- |
| **32 bpp XRGB / XBGR** | RISC OS 16M-colour modes; the 800×600 desktop | 4 bytes → RGB, swizzle from `pixo`; `alpha` ignored |
| **8 bpp palettised** | every RISC OS mode of 256 colours or fewer | 1 byte → palette texture lookup |
| **16 bpp RGB565** | `BCMVideo` 16 bpp where enabled | 2 bytes → 5:6:5 expand |
| 16 bpp 5:5:5 / 4:4:4:4 | not on a Pi; other guests | same shader, different masks, kept for completeness |
| 24 bpp packed | not by RISC OS; Linux can | 3 bytes → RGB |
| 4 / 2 / 1 bpp packed, palettised | not on a Pi (the GPU has no such buffer); kept so the decoder is complete | `byte = x·bpp/8`, `shift = (x·bpp) % 8`, LSB first, palette lookup |

Output pass shaders, independent of format:

| Shader | Purpose |
| --- | --- |
| nearest / integer | pixel-exact at whole multiples, the default for a RISC OS desktop |
| sharp-bilinear | smooth at non-integer scales without smearing pixel edges |
| scanline / CRT (optional) | because a 640×256 mode 12 deserves it; off by default |

Two shaders carry the entire real RISC OS load — 8 bpp palette and 32 bpp —
and they are the ones that get the attention; the rest are the same source
with different constants and cost nothing to keep.

### 3.3 Dirty tracking, later

Uploading the whole buffer each frame is the right first cut. If it ever
matters, QEMU's dirty bitmap (`memory_region_snapshot_and_clear_dirty` with
the VGA client, the same mechanism the fb device uses today) gives changed
rows at the cost of a short BQL section; upload only those rows. Measure
before doing it; at 800×600 it will not matter.

## 4. Input

**Keyboard.** `WM_KEYDOWN/UP/SYSKEYDOWN/SYSKEYUP` with the scan code and
extended bit from `lParam`, repeats dropped (bit 30 — the guest does its own
auto-repeat). Scan code → Linux keycode through the table QEMU already
generates for Windows (`qemu_input_map_win32_to_linux`) → qcode →
`qemu_input_event_send_key_qcode` to the `usb-kbd`, which RISC OS now
enumerates through the FIQ path. While grabbed, `ui/win32-kbd-hook.c` (also
already in the tree) keeps Alt+Tab and the Windows key from leaving the
guest. Ctrl+Alt+G toggles grab, as every QEMU backend does.

**Mouse.** Relative by default: `WM_INPUT` raw deltas while grabbed →
`qemu_input_queue_rel`, buttons → `qemu_input_queue_btn`, wheel → button
events for `usb-mouse`; the host pointer is hidden and clipped to the
window. RISC OS maps left/middle/right to Select/Menu/Adjust, which is what
a three-button `usb-mouse` reports. Absolute positioning (`usb-tablet`, no
grab needed) is worth one afternoon's test against RISC OS's HID driver;
if it works it becomes the default, if not the VM compatibility module is
where absolute coordinates go later.

## 5. Controls, and the debugger later

Menu and accelerators on the window, each a short BQL section calling what
QMP calls: reset (`qemu_system_reset_request`), pause/resume
(`vm_stop`/`vm_start`), screenshot (our decoded target to PNG; independent
of QEMU's screendump and its libpng), grab toggle, fullscreen (borderless,
Alt+Enter), quit. A status line with the guest mode, frame rate and the
instruction rate. QMP and the gdbstub stay on their sockets; the
"integrated debugging environment" attaches there first and moves
in-process only where it must.

## 6. Build

- `ui/dx11.cpp`, added in `ui/meson.build` under a new `dx11` option
  (`-Ddx11=enabled`, Windows only), linking `d3d11`, `dxgi`, `d3dcompiler`,
  `user32`, `gdi32`, `shcore`.
- `qapi/ui.json`: `{ 'name': 'dx11', 'if': 'CONFIG_DX11' }` in `DisplayType`,
  so `-display dx11` selects it.
- `hw/display/bcm2835_fb.[ch]`: a `bcm2835_fb_get_config()` accessor and the
  config generation counter — a dozen lines, generic, upstreamable.
- Registration as `qemu_display_register(&qemu_display_dx11)` from a
  `type_init`, `.init` setting `qemu_main = dx11_main`.

## 7. Sprints

### Sprint 0 — the window owns the thread (2 days)

`-display dx11` accepted; a Win32 window appears on the main thread; QEMU's
main loop is running on `qemu_main`'s thread; RISC OS boots (verified over
QMP, the window is still blank); closing the window shuts QEMU down
cleanly; a D3D11 device and swap chain clear the window at vsync. Done
when: the boot timeline over QMP matches the `-display none` numbers, i.e.
the UI thread costs the emulation nothing.

### Sprint 1 — the framebuffer, decoded on the GPU (3–4 days)

The fb accessor and generation counter; raw upload; the 32 bpp and 8 bpp
decoders with the palette texture; the scaler with aspect and integer
scaling; mode changes resize the window. Done when: the 800×600 desktop
renders at the monitor's rate, a 256-colour mode shows the right palette,
and a mode change mid-session does the right thing. Screenshot to PNG comes
with it because it is the test tool.

### Sprint 2 — keyboard and mouse (3 days)

Scan codes to qcodes; grab, hook and release; raw-input relative mouse;
buttons and wheel; the `usb-tablet` experiment. Done when: typing at the
RISC OS command line and dragging a window with the pointer both feel like
the real thing, and Ctrl+Alt+G gives the host its pointer back.

### Sprint 3 — the rest of the formats, and polish (2–3 days)

16 bpp 565, 24 bpp, packed low-depth decoders; sharp-bilinear and the
scanline option; fullscreen; DPI awareness; the status line; WARP fallback
when there is no GPU. Done when: every entry in the shader table has been
seen working, RISC OS's own modes on real data and the others on a
synthetic buffer.

### Sprint 4 — measure, then dirty rows if needed (2 days)

Profile the UI thread against the guest's instruction rate; add dirty-row
uploads only if the numbers ask for it; document the cost of a frame. Done
when: the instruction-rate figures with the window open match those
without.

### Later, not in these sprints

The VM compatibility module talking to the UI — eigen factors for pixel
aspect, absolute pointer, host files, console capture — and the sprite
offload that the decoded surface exists to make possible. Both need the
guest side first.

## 8. Risks, named

- **The main loop on a non-main thread on Windows.** Cocoa proves the
  hand-off; nothing in the Windows main loop (`WaitForMultipleObjects`,
  `select`) cares which thread it is on, and `qemu_init` still runs on the
  main thread. Verify in Sprint 0 before building on it.
- **BQL contention.** Every UI-side lock section is a few microseconds and
  never per-frame. If the profile says otherwise, input can be batched
  through a bottom half on the main loop instead.
- **Torn frames.** Accepted; a monitor tears too. If it ever offends, the
  dirty bitmap gives row-level coherence.
- **Direct3D availability.** Feature level 11.0, WARP fallback; no compute
  shaders, no feature that a 2012 machine lacks.
