# GVFill — rectangle fills and sprite plots done by the host

RISC OS asks its video driver to fill rectangles through
`GraphicsV_Render` reason 2, and plots sprites through `SpriteV`.
BCMVideo declines the fill — its dispatcher tests only for
`CopyRectangle` and `NOP` — and sprites are plotted by SpriteExtend,
which *generates* a bespoke ARM routine per format and caches only
eight of them.  Under emulation that CPU work is the slow part.

`GVFill` claims both vectors, answers the cases it understands by
handing the rectangle to the `riscos-blitter` device, and passes
everything else through untouched.

## What it takes on

**Fills — any plain colour, at any whole-byte screen depth.**  The
colour block is sixteen words of interleaved `(ora, eor)` and a fill is
`(dest ORR ora) EOR eor` per word; when every `ora` is all ones that
reduces to a constant.  Other GCOL actions need the destination read
back and are left alone.  The constant is handed to the device as the
shortest byte pattern it repeats at — one byte for an 8bpp plain
colour, so the device memsets — which also keeps odd widths fillable
(the device refuses a width the pattern does not divide).

**Sprites — pointed at, unmasked, the screen's own pixel format.**
The sprite's type must be the one matching the screen (6 for 32bpp, 4
for 8bpp, 5 for a 5:5:5 16bpp screen — the Pi's 16bpp is 5:6:5, where
old type-5 sprites genuinely need conversion and are passed up), the
plot must be a plain store, not really scaling, and whole-pixel at the
left edge; the right edge may stop mid-word, at any depth.  At 8bpp a
sprite that carries its own palette is passed up too: byte indices
only copy when both sides mean the same colours by them.  A pixel
translation table is allowed through only where it has nothing to say
(same-format sprites); cross-depth plots — every desktop sprite onto
an 8bpp screen — are passed to SpriteExtend, which really converts
them.

Masked sprites are **not** taken.  The mask merge was implemented,
loaded, and drew the desktop wrong — windows in the wrong place, content
showing through where it should have been covered — and was backed out
whole.  Masked plots are declined until the plot-action semantics are
understood; see below.

Packed sources (1/2/4/8bpp through a wide colour table) are implemented
host-side but not currently reached; see below.

## What it measures

`*SprStats` — sprite plots seen, taken on, and passed, by count and by
area, plus which gate declined the rest.

`*SprBench` — a thousand plots of a 512x512 32bpp sprite the module
owns, with the host doing it and then SpriteExtend, timed by the
guest's clock.  It owns the sprite deliberately: an earlier version
replotted whichever sprite the desktop last handed it and died when
NetSurf moved its buffer.

`*BlitFill` — paints 200x100 pixels at the top left, proving the device
without involving GraphicsV.

## Where it stands

On a 1920×1200 desktop with NetSurf and a filer window:

- **98.2% of sprite pixels** on the host — 753,664 against 13,571
  passed.
- **6.1x** on the plot itself: 490us a plot for SpriteExtend against
  80us, of which 61.8us is the host blit and about 18us is guest-side.
- **0 differing pixels of 2,304,000** against the same scene with the
  module absent, re-checked after every change.

Below 32bpp (v1.01, measured on the Windows rig, 2026-09-12): the fill
path takes window-furniture and full-screen fills at **8bpp** with
byte-exact geometry — a 91×17 title-bar segment in 2us, the
1920×1200 mode-change erase in 303us — and the same code serves 16bpp
unchanged, widths and strides being bytes.  Sprite plots onto an 8bpp
screen are cross-depth by nature (the desktop's sprites are deep, with
translation tables) and are passed to SpriteExtend by design; the
same-format sprite gates are in place for when same-depth sprites
appear.  A 16bpp 5:6:5 screen declines old type-5 sprites on purpose:
a copy between the two 16bpp layouts re-tints, and passing those up
keeps the pixels right.

What is left is one thing, not many.  Counting every gate separately —
named 0, wastage 0, scaling 0, mask 0, depth 3, no table 0 — leaves the
plot action with 56 of 59 declined calls.  Their raw values are 16 and
24: **bit 4 is set on all of them and its meaning is not established**.
`putscaled_compiler()` reads `gcol & 7` and `gcol & 8` and never looks
higher, so ignoring it looked safe, but accepting those calls changed
3468 pixels across a cluster of filer icons.  Finding out what bit 4
does means reading the assembly veneer between `OS_SpriteOp` and that
compiler, which is not in the C sources.  Until then they are declined.

## Building

Pure assembly with no relocations, so clang and llvm-objcopy suffice —
not the `roscc` module linker, which is Windows-only and only needed
for C.

```bash
./build.sh          # refuses to emit a module with relocations left
```

That check is the build, not a nicety: a module is loaded wherever the
RMA has room and nothing relocates it, so a stray relocation is a
branch into nowhere — and it does not fail at load, it takes the
desktop black minutes later.

**`ADR` reaches about a kilobyte and this handler has outgrown it
twice**, the second time breaking code that used to assemble.  The
handler carries its workspace in `r12`, which the vector dispatch
provides for exactly that purpose.  Anything added should use
`[r12, #off]` rather than a new `ADR`.

## Running

Put `GVFill,ffa` in the HostFS root and load it.  The `,ffa` matters:
`*RMLoad` checks the filetype is &FFA, and the doorbell maps a `,xxx`
suffix to the type while leaving it in the name.

```
*RMLoad hostfs:$.GVFill,ffa
```

`*RMKill GVFill` takes it back out of the path instantly, which is the
first thing to try if anything on screen looks wrong.

## Host-side shape (the Mac review)

Three device changes, made on the Mac after reviewing the hot paths:

- **Copy maps both spans** like the fill always did: disjoint ends
  (between banks) copy directly, one memcpy when the rectangle is
  contiguous — which is BANKS.md's front-to-back copy — and overlapping
  ends (a window moved within the same framebuffer) go per row through
  the scratch, or one memmove when contiguous.  Falls back to the dma
  paths when either end will not map.
- **A physical sprite source is mapped once** over its row span instead
  of a dma dispatch per row, the same treatment the destination already
  had; the virtual source keeps the page-run walker.
- **A one-byte pattern is a memset**, and white and grey window
  backgrounds are exactly that; the row build is skipped entirely.

Verified on the Mac end to end: `GVFill,ffa` delivered through
`HostFS:` from the Mac share, `*BlitFill` painting a colour-exact
200x100 rectangle (sampled from an `screendump`), and `*SprBench`
reading ~61us a plot against ~490us for SpriteExtend on an M-series
host.  The timing was read off the guest's screen (the spool route had
its own adventure), so the exact figure wants a rerun on the Windows
rig alongside the zero-differing-pixels check.
