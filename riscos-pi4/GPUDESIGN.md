# The blitter on the Mac — the pointer as a sprite the host answers for, and the sprite plots the host executes

Sprint 13 takes pieces of drawing RISC OS does in guest ARM code and
moves them to the host. The tree-wide verification of §1 settled the
scope: **the sprite plots are the real work** — the bulk of desktop
pixels, pure ARM on every Pi that shipped — and they ride RISC OS's own
extension point, with no redesign of the ROM's video stack. The pointer
joins them as the one sprite request the ROM actually sends its GPU.

> **Status, 18 September.** The sprint shipped, and §5's design is
> history: **the fill was claimed after all** (§5a's deferral reversed
> by a route it had not considered — a vector claim, not driver
> registration), the module is pure assembly called `GVFill`, not a
> mojomod called FastSpr, and the sprite coverage is the narrow
> same-format window the census earned, with masked plots declined
> after a backout. §8b records what actually stands; `blitter/README.md`
> is the operational document and `blitter/DEPTHS.md` the below-32bpp
> design and its status. The receipts of §1 are untouched below — they
> are why the shape is what it is.

> The design principle, landed where it belongs: **there is no GPU in
> QEMU — we are the GPU.** Everything the ROM asks its GPU, the host
> answers: the property channel, `'AUDS'`, and now `'DISP'`. The sprite
> plots are the complement — work no ROM ever asks any GPU to do — and
> for those, RISC OS's one sanctioned listener is a module on the
> vector, which is the same thing SpriteExtend already is.

The sources read for this are `Kernel/s/vdu/
{vdugrafd,vdugrafa,vduwrch,vdudriver,vduswis,vdupointer}`, `Kernel/hdr/
KernelWS`, and `BCMVideo/s/{GraphicsV,HWPointer,Dispmanx}`; the
tree-wide receipts of §1 come from ROOL's `BCM2835Dev` source tarball
(5.31 head), unpacked beside the component checkouts for grep; every
claim below carries its file and line. One caveat up front: those are
`master`/5.31 sources, and the ROM is 5.30 — the pointer, render and
sprite code is 2012-era and stable across that gap, but §10 says how to
verify against the ROM itself before trusting a structure offset.

## 1. What the guest does today, read out of the code

**The render interface.** GraphicsV reason 13 (`GV_Render`,
`BCMVideo/s/GraphicsV:91`, table index 13 confirmed by the kernel's use in
`vdugrafd:160`) carries an operation number in r1 and a parameter block in
r2, and the kernel tries it before every software fill or copy:

- `Kernel/s/vdu/vdugrafd:155` — `OS_Plot` block copy and move: window
  dragging, `Wimp_BlockCopy`, window scrolling. Params
  `{srcL, srcB, dstL, dstB, W-1, H-1}`, pixels.
- `Kernel/s/vdu/vdugrafa:174` — rectangle fill: window backgrounds and
  borders, `CLG`. Params `{L, T, R, B, colourptr}` where `colourptr`
  (`GColAdr`, `vdugrafa:172`) points at the **pre-encoded 32-bit pixel
  word** — a fill pattern, no ColourTrans work anywhere.
- `Kernel/s/vdu/vduwrch:411` and `:1957` — `CLS`/text-window clear (fill)
  and text-window scroll (copy), the same two operations from the writer.

If the call returns `GraphicsV_Complete` (r4 = 0) the software path is
skipped (`vdugrafd:162`, `vdugrafa:181`); otherwise the kernel does the
work itself in ARM code — the word-at-a-time shifting loop at
`vdugrafd:229-299` for a refused copy, a `NewHLine` per row at
`vdugrafa:187-194` for every fill today. Both sync-flag variants are
passed (`GVRender_Sync`, both bits), so a driver may complete
asynchronously as long as it waits when asked — a contract §6 exploits.

**What BCMVideo claims.** `BCMVideo/s/GraphicsV:387-546`: copies and NOPs
only. A copy becomes a DMA control block — TI = 2D mode, 128-bit wide,
both strides packed signed 16:16 in the STRIDE word
(`GraphicsV:495-506`) — with the three overlap cases the fork's
`hw/dma/bcm2835_dma.c` already executes on the host. Fill is refused.

**Checked tree-wide, not file-by-file.** The question "does the ROM ever
invoke a GPU blitter?" was settled against the whole Pi ROM source tree
(ROOL's `BCM2835Dev` tarball, 5.31 head, unpacked locally for grep):

| Receipt | Where | Result |
| --- | --- | --- |
| The render interface itself | `Sources/Programmer/HdrSrc/hdr/GraphicsV:87-89` | **exactly three operations**: `NOP` (0), `CopyRectangle` (1), `FillRectangle` (2). No sprite op, no line op — copy and fill are the whole acceleration surface the OS offers |
| Who issues `GraphicsV_Render` | tree-wide | the Kernel only (`vdugrafd`, `vdugrafa`, `vduwrch` ×2) |
| Who executes it | BCMVideo, and `HAL_BCM2835/s/Video:96-108` | both drive the **system DMA engine**, not the GPU — even the boot-time HAL driver picks a DMA channel for copies |
| `EDispmanResourceFill` — the one GPU-side fill in the vocabulary | `BCMVideo/s/Dispmanx:45` | defined, **never sent** |
| `DMA_TI_SRC_IGNORE` — the DMA engine's fill mode | `BCMVideo/hdr/DMA:89`, `HAL_BCM2835/hdr/DMA` | defined, **used by no code** |
| SprExtend's single GraphicsV touch | `SprExtend/Sources/SprOp:4409-4420` | the **tiling** path: render one tile in software, ask `GVDisplayFeature_CopyRectangleIsFast`, duplicate with screen-to-screen copies — pixel production stays ARM, the engine is still the copier |

The last row is a nuance worth having precisely because it looks like a
counterexample and is not: textured window backgrounds (a sprite tiled
across a Wimp window) render one tile in ARM and then copy it everywhere —
and BCMVideo already reports `CopyRectangleIsFast`
(`GraphicsV:190`), so tiling is *already* transitively host-accelerated
under this fork: every duplication copy is a DMA 2D transfer the host
executes. What remains guest-side in that path is the one tile's
render — which §5's sprite module takes. The GPU firmware, from the
ROM's point of view, is a display compositor — modes, framebuffer,
pointer overlay, video overlays, audio — and never a 2D blitter. That is
the historical shape of the OS: no machine RISC OS ran on before the
Iyonix had a blitter at all, and the drawing model assumes the ARM is
one. The scope decision follows from the receipts: **accelerate what
has a stock-ROM-compatible interface (sprites via `SpriteV`, the
pointer via `'DISP'`), defer what would mean redesigning the ROM's
driver stack (fill) until the project builds its own ROM.**

**The pointer — the one thing the ROM does ask its GPU for.** On real
hardware the pointer is a dispmanx overlay: `BCMVideo/s/HWPointer:25-58`
creates a 32×32 ARGB8888 resource over the VCHIQ `'DISP'` service;
`HWP_Update` (`HWPointer:94-206`) converts the kernel's 2 bpp shape
through the three-entry pointer palette into ARGB — filling transparent
pixels with the last non-transparent colour at alpha 0 so the GPU's
filtering cannot fringe the edges (`HWPointer:145-173`) — and an
RTSupport thread (`HWPointer:279-467`) adds, moves and removes the
overlay element, clamping and hiding at the screen edges
(`HWP_CalcCursorPos`, `HWPointer:469-536`). The kernel subtracts the
active point before the call (`vdupointer:674-682`), so the element's
destination rectangle *is* the sprite position, hotspot already applied.

Under this fork `'DISP'` has been refused like every service except
`'AUDS'` — the boot logs hold the receipts: **`vchiq service open fourcc
0x44495350 srcport 1 -- refusing`, eleven times**, once per attempt
across the boot's mode changes (`BCMVideo/s/BCMVideo:1141-1151` re-runs
`Dispmanx_Init` after each framebuffer allocation, and it retries from
zero every time the service stayed closed). So `HWP_Init` exits before
setting `HWPActive`, `GV_UpdatePointer` (`GraphicsV:163-169`) leaves the
call unclaimed, and the kernel draws the software pointer. That is the
arrow currently baked into the framebuffer — and its cost is structural:
`RemovePointer`/`RestorePointer` (`vdupointer:616-724`) bracket plotting
under `SWP_Mutex`, a 32×32 save-under and restore around every plot
region and a redraw per move, all of it guest ARM instructions.

**The shape block.** `Kernel/hdr/KernelWS:522-536`, the descriptor
GraphicsV_UpdatePointer's r3 points at: width in bytes (`+0`), height
(`+1`), logical address of the 2 bpp image (`+4`), physical address
(`+8`), active x/y from top-left (`+12/+13`).

**How a driver gets GraphicsV calls** — recorded for §5a's deferred
design, and to explain what the sprite module deliberately avoids. The
kernel addresses render, pointer and palette calls to the *current
driver* — R4 = driver number << 24 | reason (`vdugrafd:158-161`). A
module becomes a driver through `OS_ScreenMode`: reason 64 registers,
65 starts, 11 selects (`Kernel/s/vdu/vduswis:2846-2921`); an addressed
call is re-issued to another driver by rewriting R4 and calling
`OS_CallAVector` (`BCMVideo/s/BCMVideo:1657-1662`). That filter-driver
pattern is what a fill claim would require — and what §5a defers.

## 2. The rule, and the shape it forces

**The host is the GPU. QEMU models the machine's hardware surfaces, and
this fork already impersonates the GPU on all of them: the property
channel (firmware tags), mailbox channel 0, `'AUDS'`. Wherever the ROM
sends a request across such a surface, the host answers it.**

```
                     we are the GPU (no guest code)
  ┌──────────────────────────────────────────────────────────────┐
  │ kernel pointer ─▶ BCMVideo HWP ─▶ 'DISP' over VCHIQ ─▶ the peer answers:
  │                                                resource/element/bulk-write
  │                                                → Metal composite
  │ kernel copy ─▶ BCMVideo ─▶ DMA2D registers ─▶ hw/dma executes (as today)
  └──────────────────────────────────────────────────────────────┘
                     one module, one vector (guest code, Sprint 6 doorbell)
  ┌──────────────────────────────────────────────────────────────┐
  │ sprite plot ─▶ SpriteV ─▶ the module claims, doorbells ─▶ host blit
  │     (everything else passes to SpriteExtend, untouched)
  └──────────────────────────────────────────────────────────────┘
                     shipped 12–13 Sep, by the vector route (§5a)
  ┌──────────────────────────────────────────────────────────────┐
  │ kernel fill ─▶ GraphicsV FillRectangle ─▶ GVFill on the vector ─▶
  │                          the riscos-blitter device, host-executed
  └──────────────────────────────────────────────────────────────┘
```

The sprite module claims **one vector** — `SpriteV` — and nothing else:
no `OS_ScreenMode` registration, no driver selection, no forwarding
table, no BCMVideo discovery. A module loaded after the ROM's own sees
`SpriteV` first, handles the reasons it covers, and passes everything
else down the vector to SpriteExtend. SpriteExtend itself is nothing
more than the ROM's module on that vector — claiming ahead of it is the
mechanism RISC OS provides for exactly this, and the failure mode of any
bug is today's behaviour, not a dead screen.

### Answering `'DISP'`: the pointer, said yes to

The service opens with version 1/1 (`Dispmanx:91-108`), alongside a
second `'UPDH'` service for notifications whose ROM-side callback is an
empty stub (`Dispmanx:192-194`) — the peer OPENACKs both and never sends
on `'UPDH'`. `Dispmanx_Send` (`Dispmanx:237-309`) posts the message and
waits for a reply: **4 bytes for every command except
`EDispmanDisplayGetInfo`, which gets 20**; commands flagged `NoReply`
(bit 31 of the first word) want silence. The subset the pointer needs,
which is the subset the host implements:

| # | Command | Reply | Host behaviour |
| --- | --- | --- | --- |
| 8 | `EDispmanDisplayOpen` | display handle | mint a handle |
| 14 | `EDispmanDisplayGetInfo` | 20 B: result, w, h, transform, format | **answer the current mode's size** — with display == framebuffer, the ROM's scale/offset math (`Dispmanx_CalcDisplayScaleOffset`, `Dispmanx:311-367`) degenerates to identity: XScale = YScale = 65536, offsets 0 |
| 3 | `EDispmanResourceCreate` | resource handle | mint; remember 32×32 ARGB |
| 27 | `EDispmanBulkWrite` + NoReply | — | the following VCHIQ bulk carries the 4 KB ARGB image — **gathered by `vchiq_bulk_gather` unchanged**, the sound sprint's machinery |
| 16 | `EDispmanUpdateStart` | update handle | open a transaction |
| 19 | `EDispmanElementAdd` | element handle | record {resource, dest rect, layer}; the pointer element exists |
| 24 | `EDispmanElementChangeAttributes` + NoReply | — | record the new dest rect — this is a move |
| 21 | `EDispmanElementRemove` + NoReply | — | the pointer hides |
| 17 | `EDispmanUpdateSubmit` | 0 | **commit**: the sprite state the compositor reads becomes the transaction's net effect |
| 5 | `EDispmanResourceDelete`, 15 `EDispmanDisplayClose` + NoReply | handle / — | release |

Every other command — modes, reconfigures, snapshots, overlay sources
and all of `s/GVOverlay`'s vocabulary — is answered with a refusal reply,
so the service-level refusal discipline of `DESIGN.md` §11 survives
scoped to the call. The eleven `'DISP'` OPEN refusals in today's logs
become one OPENACK and a working pointer.

Three properties fall out of the protocol's own shape, none of them
designed:

- **The ROM does the pixel work.** The 2 bpp→ARGB conversion, the
  anti-fringe fill, the hotspot subtraction and the edge clamping all
  happen guest-side in code quoted in §1 — the host receives a finished
  32×32 ARGB image and a destination rectangle, and composites.
- **Updates are transactional by construction.** The element model is
  start → change → submit, and dispmanx applies at vsync; committing our
  sprite state at `UpdateSubmit` gives SPRINTS 15's frame coherence for
  the pointer without a line of its machinery — the compositor can never
  see half a move.
- **Mode changes stay on the property channel.** The framebuffer
  allocation completes first and `Dispmanx_Init` runs after it
  (`BCMVideo:1125-1165`); accepting the service changes nothing about
  how modes are selected — only what happens to the pointer afterwards.

The one honest cost of letting the ROM drive: a pointer move wakes its
RTSupport thread and sends `UpdateStart`, `ElementChangeAttributes`,
`UpdateSubmit` — three messages. That is the price of zero guest-side
code of ours, it is far below the software pointer's save-under and
redraw, §7 measures it, and §10 records the escalation if it ever shows
above the noise.

## 3. The review: which render functions can usefully go to the host

"GPU" below means the Metal device; "host CPU" means NEON under the BQL
inside the doorbell handler.

| Function | Issued by | Today | Executor | Verdict |
| --- | --- | --- | --- | --- |
| **Sprite plots** | icons, buttons, every sprite redraw; the Wimp's whole icon vocabulary | SpriteV → SpriteExtend, ARM | **shipped, narrower than designed: `GVFill` claims `SpriteV` for same-format unmasked stores (§8b); everything else falls to SpriteExtend** | the bulk of desktop pixels, on the OS's own extension point |
| **UpdatePointer + pointer palette** | pointer moves, shape changes | software pointer: save-under bracketing every plot, redraw per move | **shipped (§8a): the host answers `'DISP'`; the ROM converts and clamps; Metal and dx11 composite** | the other clean win — no guest code at all |
| CopyRectangle | Wimp drag/scroll, block copy, text scroll | BCMVideo → DMA2D → host `memcpy` rows, synchronous | unchanged for BCMVideo; `GVFill` also answers the copy through the device when both ends map (§8b) | the existence proof for §2's rule |
| FillRectangle | window backgrounds/borders, `CLG`/`CLS` | kernel ARM loop, per row | **shipped (§5a's postscript, §8b): `GVFill` claims the vector, the device fills — plain colours, whole-byte depths, the one-byte pattern a memset** | right design; the era arrived early |
| NOP / sync | kernel before software plots | trivial | n/a for now | only meaningful once §6 exists |
| VDU plotter: lines, circles, points, chars | `OS_Plot` internals, Font Manager | kernel ARM | — | not offloadable on a stock ROM; revisited when we build our own |
| DMA2D engine (`hw/dma/bcm2835_dma.c`) | BCMVideo copies | host CPU, synchronous | unchanged | no change; copy sizes (~1.2 MB worst measured, DESIGN.md §14) are already at host `memcpy` speed |

## 4. The pointer, composited

- The peer keeps the committed sprite state — 32×32 ARGB, dest rect,
  visible, and an update counter bumped at `UpdateSubmit` — beside the
  audio state it already keeps. It executes nothing for the pointer; the
  element model is drawn, not computed.
- `ui/metal.h` grows `metal_glue_cursor_view()` beside
  `metal_glue_fb_view()`: the committed image and rectangle, with the
  update counter playing the framebuffer generation's role. No
  conversion, no hotspot, no clamping on the host side — the ROM already
  did all three (§1), which is the pleasure of answering rather than
  emulating.
- The composite is a **second draw in the scale pass's encoder**: after
  the fullscreen quad, bind a `ps_pointer` pipeline and draw the sprite
  quad. Same render pass, same drawable, no extra target; sampled
  nearest, positioned by transforming the dest rect's guest pixels
  through the same transform the scale pass uses (sharp at any window
  scale, landing exactly under the host cursor the input path places),
  clipped at the drawable edge, and **without scanlines** — on a real
  Acorn machine the sprite layered over the CRT, it was not part of its
  raster.
- Threading follows the framebuffer's discipline: the UI thread reads
  the committed state with no lock; the counter makes a torn shape
  impossible and a torn position one frame late — the input path already
  has that property.
- Screenshots: the ⌘S readback and the SCRIPTING `screenshot` include
  the composited pointer (what a human sees); `screendump` and `capture`
  do not (they read the guest framebuffer, which no longer contains a
  pointer — real-hardware behaviour, and why screen grabbers on a real Pi
  return a pointerless image).

The window's host-cursor hiding stays as `MACOS.md` describes it —
hidden whenever the guest is drawing a pointer under it — except
"drawing" now means "has a sprite up", which is whenever the guest
pointer is enabled.

## 5. The sprite module: the sprint's focus (the design — superseded by §8b)

`SpriteV` is the whole interface, and it is small. The module — call it
`FastSpr` — is built on the mojomod pattern, soft-loaded from PreDesk so
it sees the vector ahead of SpriteExtend, and its handler does three
things: decide coverage, gather the operands, doorbell. Everything else
passes down the vector untouched.

**The reasons** (from `hdr/Sprite:77-102`; the plot family is what
SpriteExtend exists to serve):

| Reason | Code | What it is | Who issues it |
| --- | --- | --- | --- |
| `PutSprite` | 28 | unscaled plot, OS coordinates | the Wimp's icon plotting, applications |
| `PutSpriteUserCoords` | 34 | unscaled, user coordinates | the Wimp, tools |
| `PutSpriteScaled` | 52 | scaled plot | button backgrounds, NetSurf images |
| `PutSpriteTransformed` | 56 | affine transform | rarer; likely census-out in v1 |
| `PlotMask` / `UserCoords` / `Scaled` / `Transformed` | 48/49/50/55 | mask-only shapes | selection shapes, tools |
| everything else | 16–60… | area management, reading, writing | **always pass through** |

**The operands.** The handler validates coverage first — reason, source
format, mask type, table presence, plot action — and passes through on
anything it does not cover, so the covered set can stay honest and
small. Covered calls gather, from guest RAM:

- the **sprite header** at the given offset in the sprite area:
  `spNext`, `spName[12]`, `spWidth` (words−1), `spHeight`, `spLBit`,
  `spRBit`, `spImage` (offset to pixel data), `spTrans` (offset to
  mask), then the mode word and palette (`hdr/Sprite:124-141`);
- the **mask** — the old-style 1 bpp bitmap at `spTrans`, or an alpha
  channel in 32 bpp sprites;
- the **ColourTrans translation table**, when the caller supplies one;
- the destination rectangle and the screen's current format.

The request block carries addresses, not bytes — the host reads guest
memory itself, bounding every offset exactly as the VCHIQ peer bounds
its pagelists — and the doorbell is `SPRITE`. The v1 executor, in the
vmchannel device under the BQL, decodes the source (1/2/4/8 bpp +
header palette, 16 bpp, 32 bpp ± alpha), applies the mask, scales
(SpriteExtend's unfiltered semantics — nearest — for v1), and writes the
destination in the screen's format through the translation table when
one is given. Reply, claim, done.

**Correctness is the fall-through.** Any reason, format or flag outside
the covered set passes to SpriteExtend unclaimed, so correctness never
depends on coverage — sprint 13's own rule. The covered set must produce
**exactly** SpriteExtend's output, and a pixel-compare harness proves
it: plot with the module active and with the claim disabled, diff the
framebuffers (the `synthfb-test.py` pattern, extended from pixel formats
to plot reasons). The covered set itself is chosen on numbers, not
guesswork — G1's census traces a real desktop session and counts
reason × format × size, and the module implements the census's head.

**The tiling synergy is already in the tree.** §1's receipts table
recorded how SprExtend tiles a textured window: render one tile, ask
`CopyRectangleIsFast`, duplicate with screen-to-screen copies. Under this
fork the copies are already host-executed DMA transfers; the one tile's
render is the remaining ARM work — and it is a `PutSprite`-family call
like any other. With the module claiming it, the whole tiling path —
render *and* duplication — runs host-side, end to end.

**Contexts.** SpriteV plots arrive in foreground (Wimp redraw between
`Wimp_Poll`s); the request-block write plus doorbell is bracketed with
interrupts masked regardless, the same discipline the pointer record
would have used.

## 5a. Deferred: the fill, and the ROM we will build

Claiming `FillRectangle` on a stock ROM means becoming the current
GraphicsV driver — registration (64/65/11), discovery, and a forwarding
table for every other reason, because once selected, *all* addressed
calls arrive at the claimant first (`vdugrafd:158-161`). That is
redesigning the ROM's video-driver layering in soft-load form, with the
property that a bug in the forwarding table breaks the display for
everything. The work was drawn in an earlier draft of this document and
set aside by decision: **the project builds its own ROM later, and does
this properly there.**

In that ROM the video driver implements `FillRectangle` natively — the
kernel already asks (`vdugrafa:174`), so no kernel change is needed at
all — and because we will then own both the ROM and the machine, the
right shape is not a vector shim but **a real blitter device on the
machine**: fill, copy and sprite-plot registers, host-executed, using
§6's async contract. That is the 2D engine the Pi never offered the ARM
and RISC OS has not had since VIDC. The design carries over unchanged —
the fill's executor was always `memset32` rows from
`{dst, stride, width, height, pixel}` — and what changes is only who
asks: our driver, on our machine, instead of a shim over BCMVideo's.

**As it turned out, neither the wait nor the shim was needed.** The
fill shipped on 12–13 September without becoming the current driver at
all: `GVFill` claims the **GraphicsV vector** — the seat BCMVideo
itself sits in — and answers reason 13's fill and copy for the cases it
covers, passing everything else down. §5a's risk assessment was about
`OS_ScreenMode` registration and a forwarding table for *addressed*
calls; a vector claim ahead of BCMVideo takes only what it understands
and the ROM's own driver answers the rest — the same fall-through
correctness §5 demanded of the sprite claim. And the executor is a
real device on the machine after all, `riscos-blitter`
(`hw/misc/riscos_blitter.c`), built now rather than in an own-ROM era
because the soft-loaded module made it reachable. §8b records what
stands.

## 6. Asynchronous GPU blits, and the contract that permits them

GraphicsV's sync flags (`GVRender_SyncIfComplete`,
`SyncIfNotComplete`, both always passed by this kernel) mean **Complete
does not mean finished — it means "you may keep going, and you will wait
when you next need to"**. BCMVideo relies on this: it returns after
programming the DMA, and `GV_Render_Sync` polls the channel idle
(`GraphicsV:548-571`). The async executor adopts the same contract on
the Metal side — for the sprite blits now, and for §5a's blitter device
later:

1. The doorbell handler maps the destination (the framebuffer, or a
   sprite in guest RAM) and wraps it in `newBufferWithBytesNoCopy` —
   zero-copy on Apple silicon's unified memory; on Intel Macs the CPU
   path remains, gated on measurement.
2. A compute shader (rows, strides, blend or scale as function
   constants — the decoder's per-format specialisation again) is
   committed from the handler thread; Metal command queues are
   thread-safe and no UI object is touched.
3. **Complete is replied at commit.** The vCPU runs on while the GPU
   blits — the overlap real DMA gives real hardware.
4. The next render op with a sync flag, or a NOP, waits on the fence
   first: a completion handler scheduled as a bottom half writes the
   doorbell's STATUS and the module's NOP-spin sees it.

The win is not bandwidth — §3 says so — it is that the guest's next
thousand instructions overlap the blit. Whether that shows above the
noise is a measurement (§7), and this ships only if it does.

## 7. Measured, before and after

The instruments exist and have been used; the sprite numbers are
written down in §8b and `blitter/README.md`. Still untaken:

- **Guest cycles per plot**: the PC sampler (`tools/probe.py`) over a
  scripted icon redraw (open a directory window, drag a selection), a
  NetSurf page scroll, and a textured-window drag — the census
  workloads. SpriteExtend's inner loops must vanish from the histogram.
- **Pointer motion cost**: the sampler over pure pointer motion, three
  ways — today's software pointer, the `'DISP'` route, and (if ever
  built) a module's single-write route. The `'DISP'` route's RTSupport
  wake and three messages are the number that decides §10's escalation
  question; the assertion that framebuffer traffic from the pointer is
  zero is the same run.
- **Wall time**: drag across the Pinboard and page-scroll, sprint 13's
  done-when.
- **Ticker stability during redraw**: the 100 Hz worst gap (`ticks.py`)
  under a redraw storm.
- **Async gate**: sprite-blit storm with §6 on and off; the overlap must
  beat the fence overhead or it stays in the drawer.

## 8. Sprints

Sprint 13's own estimate is 5–8 days; this scope is finer-grained, and
the order is the point — the no-guest-code win lands first, the census
chooses the work, and nothing touches the ROM's driver stack.

- **G0 — say yes to the sprite (1–2 d). *Done; see §8a.*** The peer
  accepts `'DISP'` and `'UPDH'`, implements §2's message table, commits
  sprite state at `UpdateSubmit`; `metal_glue_cursor_view` and the
  composite pass. No dependency on Sprint 6 or any module. Done when
  sprint 13's first criterion holds — **the pointer is drawn by the UI
  and absent from the framebuffer** — on a boot from the card *and* from
  ROM alone, and a `screendump` has no arrow while the window does.
- **G1 — the census (1 d). *Done, as `*SprStats`.*** The counts are
  `blitter/README.md`'s "where it stands": 98.2% of sprite pixels
  taken on a 1920×1200 desktop (753,664 against 13,571 passed), and
  the declines named by gate — leaving one open shape, the plot-action
  bit (§10).
- **G2 — the sprite module (3–4 d). *Built, differently — `GVFill`,
  not FastSpr (§8b).*** Pure assembly, both vectors, the fall-through,
  and the pixel-compare: **0 differing pixels of 2,304,000** against
  the same scene with the module absent, re-checked after every change.
- **G3 — async GPU (2–3 d, gated). *Not built; still gated.*** The
  doorbell is synchronous and §6 remains a design. The gate's number
  has not been taken — the 6.1× synchronous win (§8b) took the urgency.
- **G4 — the record (1 d). *Done*** — the numbers live in
  `blitter/README.md` and `blitter/DEPTHS.md`, and this document now
  carries §8b.

## 8a. G0, as built

The peer answers `'DISP'` and the window composites the sprite; the
arrow and the busy timer both render correctly over the live desktop,
from the card and from ROM alone. The shape data was verified before
any geometry was trusted: the bulk-written words are little-endian
`0xAARRGGBB` — derived from the kernel's palette encoding (`&00BBGGRR`
with red at bits 15..8, `vdupalxx:679-690`) through `HWP_Update`'s
`REV`-plus-alpha — and the transparent fringe words came out as the
light blue the ROM's own comment predicts, 134 opaque texels of arrow
in 32×32. A boot from the card makes 69 shape bulk-writes and about 356
move transactions; the ROM-only boot runs the identical sequence to
`pointer visible 1 at 303,239`. The software pointer is off by
construction: those transactions only happen because the ROM's `HWPointer`
claimed `GraphicsV_UpdatePointer`, and the kernel paints its own arrow
only when that call goes unclaimed (`vdupointer:711-721`).

Three things the wire taught, each costing a build:

- **The sprite is mode-independent, absolutely.** The first build
  indexed the image with the destination rectangle's size — and the
  rectangle is *scaled* by the ROM's display arithmetic, so the
  indexing strode diagonally across the 32-wide image and the arrow
  came out as sheared garbage. The image is indexed by the resource's
  own resolution (32×32) and stretched to whatever rectangle arrives;
  the screen mode is nobody's business.
- **The destination rectangle lives in display space.** The ROM asks
  `DisplayGetInfo` before the first framebuffer allocation exists, so
  the peer's answer (640×480 fallback) is what its scale-and-offset
  arithmetic runs against — rectangles arrive at 0.8 scale over the
  800×600 desktop. The peer records what it answered and the compositor
  maps the rectangle through that display space, which is the honest
  real-hardware semantics: the firmware scales the desktop into the
  display, the element sits in the display, and with matching aspect
  the mapping lands the sprite on exactly the right guest pixels.
- **A corner-exact triangle loses its lower right half.** The sprite
  quad's covering triangle must overshoot the rectangle the way
  `vs_main`'s does; a triangle whose hypotenuse passes exactly through
  the rectangle's corner drops the anti-diagonal and everything beyond
  it to the top-left fill rule — the busy timer kept its upper-left
  mass and the arrow lost most of itself. The overshoot's overhang is
  discarded in the fragment, since unlike `vs_main`'s it falls inside
  the viewport.

Also in this build: the transaction commit at `UpdateSubmit` (staging
image plus pending rectangle applied atomically, generation bumped once
— a torn sprite is impossible), the lock-free cursor view on the same
seqlock discipline as the framebuffer config, the ⌘S/`METAL_SHOT_EVERY`
readback blending the sprite in software with the same straight alpha,
vmstate v3 carrying the committed sprite, and both services closing
cleanly with the machine. What is not yet done from §10: the pointer
-motion cost measurement (Q3) and the overlays-stay-inert trace (Q4).

The September reliability pass tightened the same seams, front end and
device together:

- **A commit that lands mid-copy drops the frame, both front ends.**
  The cursor metadata was snapshot-checked but the pixels were handed
  out as a pointer and copied with no re-check, so a `disp_commit`
  between check and copy tore the sprite for one frame. Both ends now
  re-read after the copy and, if the generation moved, take nothing
  from the read and draw nothing; the writer publishes the generation
  with the barrier it always owed.
- **The framebuffer config is a seqlock in fact as well as in name**
  (`bcm2835_fb`, `efd305129b`): read and written like one, so a config
  change under a reader is a retry, not a guess.
- **The palette bound is offset plus length** (`bcm2835_property`):
  the old bound checked the offset alone and a short trailing write
  ran past the buffer.
- **Linux builds refuse `'DISP'` again** — scoped to them, so the guest
  draws its own software pointer where no compositor will; the Mac and
  Windows builds still answer.
- **A mode with no decoder is refused**: 1/2/4/8 bpp take the
  palettised decoder, 16/24/32 the direct ones, and anything else is a
  logged clear screen — as dx11 refuses it — not a guess at what the
  bits mean.
- **A screendump row that cannot be read fails the command**: a mode
  change between config snapshot and read leaves the buffer holding
  heap that was never the guest's, and encoding it would put host
  memory into the PNG. dx11's twin says so too when it waited for
  nothing, instead of returning the last frame.
- **Failures say their names**: a failed Metal command buffer logs its
  error, a failed dx11 pipeline build releases what it created, and
  the scripting surface takes the reply's length before it frees the
  reply.

## 8b. The sprint, as built: `GVFill` and the `riscos-blitter` device

What stands differs from §5 in three ways, each forced by the wire:

- **The module is `GVFill`**, pure ARM assembly with no relocations
  (`blitter/blitmod.s`; clang + `llvm-objcopy`, not the module linker —
  the build refuses to emit a module with a relocation left, because
  the RMA loads it nowhere in particular). It claims **two vectors**,
  `GraphicsV` and `SpriteV`, answering what it covers and passing
  everything else down — §5's fall-through rule, applied to a wider
  surface than designed. Its writable state is a 172-byte block
  claimed from the RMA at init; nothing in the module image is ever
  written, which is what makes read-only ROM safe for it too.
- **The executor is a device on the machine** — `hw/misc/riscos_blitter.c`
  — fill, copy and sprite-plot registers reached through the Sprint 6
  doorbell. The device is depth-generic by construction: it works in
  bytes, the fill pattern is up to sixteen bytes with a repeat length
  (a one-byte pattern is a `memset` — white and grey window
  backgrounds exactly that), the copy maps both spans so disjoint ends
  go as one `memcpy` and overlapping ends per row or one `memmove`
  (BANKS.md's front-to-back copy), the sprite op carries a
  bytes-per-pixel field plus a packed-source + wide-colour-table path
  for 1/2/4/8bpp sources, and **a DMA fault is a fault** — a bad
  address is not zero pixels quietly drawn. The device also publishes
  a one-word damage flag, and **both front ends copy the guest's
  screen only when it has drawn and then stopped** (metal since
  11 September; dx11 ported to it and deleted its `fb_probe`
  fingerprinting on 12 September).
- **Sprite coverage is the census's head, not §5's menu.** Taken:
  pointed-at, unmasked, the screen's own pixel format, a plain store,
  whole-pixel at the left edge, only while output is the screen (the
  six VDU constants are cached per output switch), no
  palette-carrying 8bpp sprite, and a 5:6:5 16bpp screen declines old
  type-5 sprites because a copy between the two 16bpp layouts re-tints.
  Cross-depth plots pass to SpriteExtend, which really converts.
  **Masked plots are declined** — the merge was implemented, loaded,
  and drew the desktop wrong; it was backed out whole and stays out
  until the plot-action semantics are understood.

Measured (the instruments are `*SprStats`, `*SprBench`, `*BlitFill`;
`blitter/README.md` carries the operational detail): on a 1920×1200
desktop with NetSurf and a filer window, **98.2% of sprite pixels** on
the host; **6.1× on the plot itself**, 490µs SpriteExtend against 80µs
— 61.8µs of it the host blit, about 18µs guest-side; **0 differing
pixels of 2,304,000** against the same scene with the module absent.
Below 32bpp (v1.01, the Windows rig): fills byte-exact at 8bpp — a
91×17 title-bar segment in 2µs, the 1920×1200 mode-change erase in
303µs — the same code serving 16bpp unchanged; `DEPTHS.md` is the
design and its status.

Two placement facts the wire taught, both in `blitter/README.md`:
**GVFill runs soft-loaded** from the share's `$.Modules` — only HostFS
and its filer belong in the ROM (decided 13 September, BOOTDESIGN
§3.7), and a ROM-spliced GVFill never sees a sprite plot anyway,
because something later in the boot claims `SpriteV` in front of it
(killing DitherExtend, GSpriteExtend, SpecialFX, GDraw,
ArtworksRenderer, StrongTask, Fat32fs and NetTime changed nothing;
`*RMReInit GVFill` puts it back in front). And `*RMKill GVFill` is
the first thing to try whenever anything on screen looks wrong — the
fall-through means the desktop works without it, just slower.

## 9. What we deliberately do not do

- **No fill redesign.** §5a deferred the fill because a claim meant
  becoming the current GraphicsV driver. The vector claim that shipped
  (§5a's postscript) avoided that redesign, so the fill stands — but
  the boundary holds: no `OS_ScreenMode` registration, no forwarding
  table, nothing that must answer for every reason. The own-ROM blitter
  device of §5a remains the destination for the rest.
- **No full dispmanx.** §2's table is the whole implementation — the
  pointer's commands — and every other command is refused at the call
  level, so the overlay and snapshot machinery in `s/GVOverlay` stays
  inert. This reverses the earlier refusal of `'DISP'` outright, the
  same way `SOUND.md` §1 reversed `DESIGN.md` §11's refusal of `'AUDS'`:
  the refusal was right until something stood behind it.
- **No V3D, no VideoCore.** The guest cannot reach them and nothing here
  needs them.
- **No VDU-plotter offload.** Lines, circles, points and font rendering
  have no claimable interface below "rewrite the kernel"; they stay
  guest work on a stock ROM, revisited in the own-ROM era.
- **No GPU-resident screen.** Sprint 13's precondition stands: the
  destination stays in guest RAM while the kernel, the Font Manager and
  applications all draw there directly.
- **No ROM patching.** The module is soft-loaded, the ROM stays
  pristine, and the next ROM release survives.
- **No synchronous GPU round-trips in v1.** The GPU earns its place
  through composition and asynchrony or not at all.

## 10. Open questions, to be settled on the wire

1. **Source drift**: `master`/5.31 against the 5.30 ROM — the structure
   offsets here (pointer block, sprite header, dispmanx message shapes)
   must be read out of the running ROM before G0/G2 hard-code them;
   `mkcmos.py`'s header-parsing trick or a live-guest dump both work.
2. **The dispmanx reply protocol on the wire**: `Dispmanx_Send`'s
   4-or-20-byte reply and the `NoReply` convention are read from the
   ROM's sender; the first G0 run confirms them against our OPENACK and
   slot discipline the way `SOUND.md` §10 confirmed `'AUDS'`.
3. **Pointer-motion cost**: §7's three-way measurement; if the ROM's
   RTSupport wake and three-message move show above the noise, a
   module's single-MMIO-write route for the pointer is the documented
   escalation — built only on that number.
4. **Overlays stay inert**: confirm nothing on a stock 5.30 desktop
   issues the refused dispmanx commands (a trace over a full session),
   and that GVOverlay's calls fail cleanly against the refusal replies.
5. **The census's shape**: *settled — and it left one shape behind.*
   The Mac census (§8b) took 98.2% of pixels and named every decline
   but one: counting each gate separately leaves the **plot action**
   with 56 of 59 declines, all carrying bit 4 set, whose meaning the C
   sources do not reveal — `putscaled_compiler()` reads `gcol & 7` and
   `gcol & 8` and never looks higher, but accepting those calls changed
   3468 pixels across a cluster of filer icons. The answer is in the
   assembly veneer between `OS_SpriteOp` and that compiler, and until
   it is read those plots stay declined. Below 32bpp the census columns
   remain to be taken (`DEPTHS.md`).
6. **SpriteV claim contexts**: plots are expected in foreground, but
   background claims (printing, filer threading) decide whether coverage
   validation must reject a context rather than queue it — settled by
   the same trace.
7. **`OS_SpriteOp` on the screen with the pointer up**: grabbers read
   the framebuffer and will not see the sprite — real-hardware
   behaviour, worth one confirmed example rather than an assumption.
8. **Intel Macs**: `newBufferWithBytesNoCopy` over mapped guest RAM is
   unified-memory reasoning; on discrete-GPU Macs §6 may be a loss. The
   gate is measurement, and the CPU path is always present.
9. **The 2688-byte difference**: the Windows rig's 2026-09-12 pixel
   compare found 2,688 differing bytes of 6.9M where the Mac's figure
   is zero — small, real, unexplained, and on exactly the path
   `DEPTHS.md` extends. Understand it before more depths ride on it.
10. **Masked sprites**: declined since the backout (§8b); taking them
    again means understanding the plot-action semantics first — the
    same reading as item 5, and the reason the two stand together.

## 11. Where it meets the rest of the plan

Sprint 13 is closed: the pointer and the sprites shipped (§8a, §8b),
and the fill arrived early by the vector route (§5a's postscript) with
its own-ROM destination unchanged. SPRINTS 15 (frame coherence) got
its free derivative — the pointer is committed transactionally at
`UpdateSubmit`, so it can never tear, and it is not in the framebuffer
to be torn anyway; the mid-copy drop of §8a closes the read side. SCRIPTING's `screendump`/`screenshot` split documents
itself in §4. Sprint 6B carries the module build for G2 only; G0 needs
nothing but the peer and the window. Sprint 9's speed work gets §7's
instruments and numbers. The own-ROM era inherits §5a's blitter device
and §6's contract unchanged. And the "graphics kernel" — every blit a
shader, the screen GPU-resident — remains the step after, with its
precondition unchanged and now measurable.

## Sources

Read for this design, all primary: `BCMVideo/s/GraphicsV` (the handler,
the copy/DMA programming, the sync, the foreground guard),
`BCMVideo/s/HWPointer` (the dispmanx pointer, the 2 bpp→ARGB conversion
and fringe trick, the clamping), `BCMVideo/s/Dispmanx` (message IDs,
service-open shape, reply sizes, `NoReply`, scale/offset math),
`BCMVideo/s/BCMVideo:608-613,746-748,1125-1165,1657-1662` (driver
registration, the FB2 init order that keeps modes on the property
channel, addressed vector calls), `Kernel/s/vdu/vdugrafd:146-166` (the
copy dispatch and the software loop it avoids),
`Kernel/s/vdu/vdugrafa:142-194` (the fill dispatch, `GColAdr`, the
per-row fallback), `Kernel/s/vdu/vduwrch:411,1957` (CLS and text
scroll), `Kernel/s/vdu/vdudriver:124-131,279-294` (driver workspace,
features cache), `Kernel/s/vdu/vduswis:2045-2128,2846-2925`
(SelectDevice, Register/Start/StopDriver),
`Kernel/s/vdu/vdupointer:616-724` (the software pointer's save-under
bracket, hotspot subtraction), `Kernel/hdr/KernelWS:522-536` (the
pointer shape block), `Sources/Programmer/HdrSrc/hdr/GraphicsV:82-89`
(the three render operations), `Sources/Programmer/HdrSrc/hdr/
Sprite:77-102,124-141` (the SpriteV reason table and the sprite
header), `Sources/Video/Render/SprExtend/Sources/SprOp:4409-4420` (the
tiling path's one GraphicsV question), `Sources/HAL/HAL_BCM2835/s/
Video:96-108` (the boot driver's DMA channel), `BCMVideo/hdr/DMA:89`
(the unused fill mode). Live evidence: the boot logs' eleven `'DISP'`
(0x44495350) refusals. In-tree: `hw/dma/bcm2835_dma.c:62-151` (the 2D
row-whole executor and its trace point), `hw/misc/bcm2835_vchiq.c`
(`vchiq_bulk_gather`, reused for the pointer's bulk write),
`ui/metal.c:50-132` (the fb view and generation discipline),
`ui/metal.m:560-728` (the pipeline, the shared-storage ring, the two
passes the third joins). The ROOL checkouts and the `BCM2835Dev`
tarball live outside the repo; their licence (Apache 2.0) permits the
reading, and nothing of them is copied into this tree.
