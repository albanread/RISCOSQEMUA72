# Colour depths — making the blitter work below 32bpp

> **Status 2026-09-12: implemented and partially verified.** The module
> gates, the width/wastage maths, the bytes-per-pixel write, the 8bpp
> no-palette rule and the fill pattern-length fix are in (v1.01,
> `blitmod.s`); the device needed no change.  Verified on the Windows
> rig: 8bpp fills byte-exact by trace (furniture and full-screen), the
> 16bpp (c32k) mode running with the module loaded, 32bpp unchanged.
> Still open: the per-depth pixel-compare pass (the 8bpp desktop
> rebuild is slow by design — its sprites are cross-depth — so the
> scene harness needs patience), the census columns, and the 2688-byte
> difference noted under Acceptance.

## Why this is on the list

The accelerator only takes work on a 32bpp screen. Measured on the
Windows rig (2026-09-12): a 1920×1200 desktop at the CMOS default
depth scrolls with **zero** blitter operations, the same session with
`*WimpMode X1920 Y1200 C16M` forced starts taking full-screen copies
and the fill path, and the page scrolls still gain nothing because the
sprite path's own gates stay shut. The Mac's 98.2%-of-pixels census was
taken on a 32bpp desktop, so the port's headline numbers silently
assume a depth the Windows setup does not choose by default.

Two remedies, and we should ship both:

1. **Default the Windows rig to a 32bpp mode** — a one-line change to
   the CMOS recipe or the launch line, and everything measured so far
   starts working. Do this regardless; it is today's fix.
2. **Make the accelerator depth-honest** — a screen mode is a user
   choice, 8bpp and 16bpp are real desktop modes (the front ends decode
   them already; a mid-session 32→8→32 change is verified on dx11), and
   the fill/copy/sprite work should not evaporate because the user
   picked one. This document designs that.

The short version of what follows: **the device is already
depth-generic by construction, the fill and copy paths in the module
already are, and everything that remains is the SpriteV gate in
`blitmod.s` plus the 16bpp pixel-format question, which is a real trap
and not a formality.**

## What is already depth-ready (do not rebuild it)

Verified by reading the code, not assumed:

- **The device fills in bytes.** `BLIT_WIDTH` is bytes per row,
  `BLIT_PATTERN` is up to sixteen bytes with a repeat length
  (`include/hw/misc/riscos_blitter.h:47-52`), and the memset fast path
  fires whenever the pattern's bytes are all equal — the check is on
  the bytes, not on a depth (`hw/misc/riscos_blitter.c:194-201`). An
  8bpp constant fill arrives as four identical bytes and memsets
  today.
- **The device copies in bytes.** Strides are signed bytes; nothing in
  the copy path knows what a pixel is.
- **The device sprite op takes a bytes-per-pixel field.** `BLIT_BPP`,
  accepted values 1..4 (`hw/misc/riscos_blitter.c` `blit_sprite`:
  `bpp == 0 || bpp > 4` is the only depth check), and every quantity —
  first byte, span, row width — is derived as pixels × bpp. It also
  carries the packed-source path (`BLIT_SRCBPP` + `BLIT_TABLE`, a wide
  colour table) for 1/2/4/8bpp sprite sources onto a deeper screen,
  which nothing reaches yet.
- **The module's GraphicsV fill path already computes bytes per pixel**
  (`blitmod.s` `gv_handler`: reads Log2BPP, declines only sub-byte
  depths with `CMP r7, #3 / BLO gv_pass`, shifts 1<<r7 into a
  bytes-per-pixel, multiplies the width by it, offsets by
  `bytes-per-pixel × left pixel`). The 16-word (ora,eor) block reduces
  to a repeated-sub-pixel word at any depth, and the four pattern bytes
  it hands the device repeat correctly at 8 and 16bpp.
- **The module already hears mode changes.** `Service_ModeChange`
  invalidates the cached VDU constants, so depth switches mid-session
  re-read the geometry rather than acting on stale values.

One protocol wart worth a comment while we are here: `BLIT_WIDTH` is
documented as "bytes per row", which is true for fill and copy, but the
sprite op uses it as **pixels** and multiplies by `BLIT_BPP` itself.
The header comment should say so; no behaviour change.

## What actually gates, and the design per gate

All of it is in the SpriteV handler (`blitmod.s` `sv_handler`), in
three places:

### Gate 1: the sprite's type word — `type == 6`

The mode word's type field (bits 27..31) is required to be 6, 32bpp.
**Replace with: the sprite's type must be the type that matches the
screen's format**, from the Log2BPP the handler already reads:

| Screen Log2BPP | Screen NColour | Sprite type that matches |
| --- | --- | --- |
| 5 (32bpp) | &FFFFFF | 6 |
| 4 (16bpp) | &7FFF | 5 — **only if the screen really is 5:5:5, see below** |
| 3 (8bpp) | 255 | 4 |

Add `NColour` (VDU variable 3) to `sv_constvars` beside Log2BPP so the
gate is on the format pair, not on depth alone. Sub-byte screens
(Log2BPP 0..2) keep today's decline; the device's table path could
serve them later, nothing today reaches for them.

### Gate 2: the 16bpp pixel layout — the real trap

RISC OS has **two** 16bpp layouts, and a byte-for-byte copy between
them silently re-tints the picture:

- Old-format **type 5** sprites are 5:5:5, 32K colours (NColour &7FFF).
- The BCM2711 framebuffer's 16bpp is RGB **5:6:5**, and RISC OS
  represents that as NColour &FFFF at Log2BPP 4.

So "16bpp sprite onto 16bpp screen" is only a copy when the two 16bpp
formats agree. The design:

- If the screen is 5:6:5 (NColour &FFFF): old type-5 sprites are
  **declined**. They genuinely need conversion — SpriteExtend is doing
  real work on them, and passing them through as copies would be wrong
  pixels, which is the one thing this module must never produce.
- New-format sprites (mode word is a pointer to a mode descriptor) can
  be checked for a true format match by following the descriptor and
  reading NColour/ModeFlags. That is a dereference in a vector handler
  of a pointer OS_SpriteOp already validated. **Deferred**: wire it
  only if the census (below) shows the 16bpp desktop actually plots
  new-format sprites; desktop icons are old-format in practice.
- If the screen is 5:5:5 (NColour &7FFF) — possible on other hosts,
  not the Pi — then type 5 matches and the copy is honest.

The 8bpp case has no such split: a byte is a palette index, one
layout, no formats to disagree.

### Gate 3: the screen's depth — `Log2BPP == 5`

Folded into Gate 1's format table: the check becomes "sprite format
matches screen format" rather than "screen is 32bpp".

### Wastage bits at sub-word depths

Today's `spLBit == 0 && spRBit == 31` demands whole-word rows, which at
32bpp is any width. At 8bpp it means widths divisible by four and at
16bpp even widths — and desktop icons are routinely odd-width. The
device can already express partial rows: `BLIT_WIDTH` is pixels on the
sprite op and `BLIT_SSTRIDE` is bytes per source row, so:

- keep `spLBit == 0` (rows start word-aligned; near-universal),
- accept any `spRBit`, and compute the true pixel width:
  `pixels = ((spWidth+1)*32 - (31 - spRBit)) / bits_per_pixel`,
- pass `BLIT_SSTRIDE = (spWidth+1)*4` as today — rows are word padded
  whatever the depth, so the stride formula does not change.

The current code's width accounting (`ADD r5, r5, #1` — "pixels: one
word each at 32bpp") is the line that assumes words are pixels; it
becomes the formula above, and the taken-area counters multiply by real
pixels.

### The register write that says 4

`MOV r11, #4 / STR r11, [r10, #BLIT_BPP]` becomes the bytes-per-pixel
the fill path already knows how to compute (reuse the same
`1 << (Log2BPP - 3)` sequence; the two handlers can share a helper or
duplicate three instructions, as the file's style prefers).

### 8bpp sprites and palettes

A same-depth 8bpp byte copy is only correct if the sprite's palette
indices mean the same colours as the screen's. Desktop sprites are
drawn against the Wimp palette, and 8bpp modes bring up that same
palette, so in practice the indices agree. But a sprite may carry its
own palette (visible as `spImage > 44` — palette words between the
header and the image), and then agreement is an assumption we should
not make silently:

- **v1:** take only sprites with no palette (`spImage == 44`); decline
  palette-carrying ones. The census tells us what that costs.
- **v2, if the census says it matters:** cache the screen palette on
  `Service_ModeChange` (64 words read once per mode) and compare a
  palette-carrying sprite's 64 words against it in the handler — tens
  of loads, still cheap next to the blit it saves, and a byte-exact
  answer rather than a hope.

### What deliberately does not change

- **Masked sprites** stay declined at every depth (the device's
  `BLIT_F_MASK` exists; the module's masked work was backed out whole
  on 32bpp and that decision is untouched by depth).
- **The plot-action bit** (bit 4; 56 of 59 declines on the Mac census)
  is orthogonal to depth and stays the separate open question it
  already is.
- **Sub-byte screens** (1/2/4bpp) stay declined, as fills already do.
- **Cross-depth plots** (8bpp sprite onto a 32bpp screen and friends)
  stay declined for now. The device's packed-source + wide-table path
  is the designed home for the common case of that — old low-bpp
  sprites onto deep screens — and wiring it is its own sprint, not a
  rider on this one.

## Measure before building — the census, per depth

The fork's own method: census first, build second.

1. Extend `*SprStats` with depth-aware buckets: plots declined by
   *format mismatch* (the new gate), by *palette present*, and by
   *wastage*, each split by the screen depth they arrived at. The
   counters exist; this adds buckets, not machinery.
2. Run the desktop at each of 8bpp, 16bpp (whichever layout the Pi
   mode is) and 32bpp, scrolling the same NetSurf page, and record the
   split. The 32bpp column reproduces the Mac census and becomes the
   regression baseline; the 8 and 16bpp columns say which gates the
   desktop actually trips, and therefore which parts of this design
   earn their code.
3. Expect the 8bpp column to be dominated by palette-present and
   plot-action declines, and decide the v2 palette compare on that
   number rather than on principle.

## Acceptance: pixel-exact, per depth

The same discipline as the 32bpp work, run per depth:

- Same scene (NetSurf welcome page plus a filer window), screendumped
  with the module loaded and with it killed, byte-compared. Zero
  differing pixels is the bar, at 8bpp and at 16bpp as it was at 32bpp.
- **Chase the existing 2688-byte difference first.** The 2026-09-12
  Windows run at 1920×1200×32 found 2,688 bytes differing of 6.9M with
  the module on vs off — the Mac's figure is zero. That is a small,
  real, unexplained render difference on the path this design extends,
  and it should be understood before more depths ride on it.
- `*SprBench` needs a sprite in each tested format to be meaningful:
  parameterise the mode word it builds (type 6 today; add type 4 for
  8bpp; a 16bpp variant only in the layout that passes the gate). An
  8bpp bench sprite is a small table; a 5:6:5 one needs a descriptor
  and can wait on the census.

## Effort and order

1. Windows CMOS default to 32bpp (independent, immediate).
2. Census buckets + the 8/16/32 desktop columns — measurement, no
   behaviour change.
3. The 8bpp gate set (format table, bpp write, width formula, wastage
   relaxation, `spImage == 44`): the whole of 8bpp is this, because
   fills were already done and 8bpp has no format split.
4. 16bpp, gated on the layout the census shows the Pi mode to be, and
   on whether type-5 sources even appear in numbers worth taking.
5. The 2688-byte investigation, before or alongside 3.

Steps 3 and 4 are confined to `blitmod.s`; the device changes not at
all, and the header gets one clarifying comment.
