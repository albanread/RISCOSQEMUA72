# Double buffering the desktop with the screen banks RISC OS already has

## The idea

Not mine — it came out of a session watching the desktop tear:

> create fb, display fb, copy fb -> bb, update bb, swap

The copy is the part that makes it work.  The Wimp redraws
*incrementally*: when something changes it hands each task a list of
damaged rectangles and repaints only those, leaving everything else
alone because it is still correct.  Flip a bank under that and the back
buffer holds the frame from two flips ago — the backdrop, other
windows, the icon bar would all snap back to stale content.  Copying
front to back first makes the back buffer correct before anything
draws into it, and incremental redraw then works exactly as it does
now.

That copy is why RISC OS does not do this: on a real Pi it is 9.2 MB of
LDM/STM every frame.  On a host it is one `BLIT_OP_COPY`, which the
blitter already implements and has never once been asked to perform.

The prize is not speed.  It is that **a mistimed swap shows a complete
older frame instead of a half-drawn one** — late rather than torn.
Every scheme tried so far has had to guess when the guest has finished
drawing, and guessing wrong produced a torn frame.  With banks, that
same wrong guess costs a frame of latency and nothing else.

## What is already known

Verified, with where it came from.  Do not re-derive these.

- **The memory is already there.** `Kernel/s/vdu/vdudriver:1365` —
  "Massage ScreenSize, needs to be large enough for two screen banks",
  `MOV r0, r0, LSL #1`.  Confirmed live: the framebuffer is programmed
  as 1920x1200 visible, **1920x2400 virtual**.
- **It never pans.**  The fb config logs `offset 0,0` on every mode
  change and throughout scrolling.  The second bank is allocated and
  unused.  `ui/metal.c` logs this geometry once per mode.
- **The switch is supported.**  `Kernel/s/vdu/vdu23:1289`
  `DoSetDisplayBank` (OSBYTE &71 = *FX 113).  It converts the bank
  number to an address, bounds-checks it against `ScreenEndAddr` — so
  it only works at all because two banks were allocated — updates
  `DisplayBankAddr`, and calls `SetVinit`, which programs the hardware
  and sets `DisplayStart`.  `ScreenModeReason_CountScreenBanks` is 7.
- **The Wimp does not care which bank it draws into.**  27 source files
  under `Desktop/Wimp/s`, **zero** references to `ScreenStart` or
  `DisplayStart`.  It plots entirely through `OS_Plot` and
  `OS_SpriteOp` — 48 call sites in `Wimp10` alone — so the VDU drivers
  resolve the address and the Wimp follows whatever bank is current.
- **BCMVideo can pan.**  `ARM2VC_Tag_FBSetVirtOffset` with `mbyoff`,
  sent from its update path.

Two theories died on measurements and should not be retried:

- *The blitter needs the framebuffer's pan offset.*  It is always 0,0,
  so adding it is a no-op.
- *RISC OS draws into a second bank already.*  It does not; it draws
  and displays bank 1 throughout.

## Open questions, cheapest first

**1. Does the desktop survive a bank switch at all?**  No code needed.
At the F12 prompt:

```
*FX 113,2
```

and look.  Then `*FX 113,1` to come back.  If the screen goes black and
returns, banking works and the desktop tolerates it.  If it wedges,
the whole idea stops here and an afternoon is saved.

**2. Is there a write-bank call to match?**  `DoSetDisplayBank` handles
OSBYTE &71.  Find its counterpart for &70 (select the bank VDU output
goes to) in `Kernel/s/vdu/vdu23`, and confirm it updates `ScreenStart`.
Without it there is nowhere to draw.

**3. Does `SetVinit` actually reach `FBSetVirtOffset` on the Pi?**  The
tag exists in BCMVideo and has only ever been observed idle.  Watch the
fb geometry log in `metal-debug.txt` while doing (1): if `offset`
changes from 0,0 the whole path is live.

**4. Does anything else break?**  The kernel's pointer code reads
`DisplayBankAddr` (`vdupointer:799`), and save-unders read the screen
back.  Check by eye during (1): does the pointer survive, do menus
still restore what was under them.

## Module or ROM?

Probably ROM, and worth deciding early rather than discovering halfway.

**What a module can do.**  RISC OS is cooperative -- one task runs at a
time -- so a swap and copy in a pre-filter is atomic with respect to
task drawing: nothing else can draw between a task entering Wimp_Poll
and the next task being scheduled.  Drawing the Wimp does itself
(furniture, menus, drag outlines) happens after that point and lands in
the new back buffer, which by then is correct.  So an external module
can very likely produce a working double buffer.

**What only the Wimp can do.**  The full-screen copy is needed *only*
because the Wimp does not know it is double buffered.  A Wimp that did
would keep the damage from the previous frame as well as this one, and
redraw their union into the back buffer -- no copy at all.

Put another way: with two banks a damaged rectangle is not correct
until *both* have been repainted, so damage stops being a per-frame
list and becomes per-bank -- each rectangle has to survive until every
bank has seen it.  That bookkeeping belongs where the damage lives.
The Wimp has to cycle its own banks; it cannot have them swapped
underneath it.  That is the
textbook approach and it is strictly better: it turns a fixed
full-screen cost per frame into a cost proportional to what actually
changed, which on an idle desktop is nothing.  The damage lists live
inside the Wimp and nothing outside it can see them.

So the module version is the cheap experiment that proves the idea and
gives a usable result; the ROM version is the right implementation.  If
the experiment works, the question becomes whether to carry a Wimp
patch, which needs the RISC OS build environment rather than the
clang-and-objcopy route that builds a relocatable module (see
[[riscos-guest-module-workflow]] and `blitter/README.md`).

Worth noting what this would be worth upstream: the desktop tears on
real hardware too.  The reason RISC OS does not double buffer is the
cost of the copy, and the damage-union approach removes that reason.

## Implementation, once those answer

1. **Front end follows the displayed bank.**  `ui/metal.c` maps and
   copies from `cfg.base`; with banking in use it must follow
   `DisplayStart`.  This is needed regardless and is independent of
   everything else — do it first and confirm nothing changes while the
   guest is still single-banked.
2. **A separate module.**  Not GVFill.  The blitter acceleration and
   the display sync should not share a fate; GVFill spent a day
   breaking the screen and a sync mechanism that dies with `*RMKill`
   would be the wrong dependency.
3. **Drive it from the Wimp bracket.**  `Filter_RegisterPreFilter`
   fires on entry to `Wimp_Poll` — a task has finished its work.  On
   that: swap the display to the bank just drawn, copy it to the other
   bank with `BLIT_OP_COPY`, point VDU output at that one.
4. **Measure.**  The tearing was always judged by eye; keep doing that,
   but also count swaps and copy cost from the blitter trace.

## Backing out

It is a module: `*RMKill` it and the desktop returns to single-banked
drawing.  The front-end change in step 1 is a no-op while nothing
banks.  Nothing here needs a ROM change.

## How to work on it

Every wrong turn this project has taken came from writing code before
taking the measurement that would have settled the question.  The pan
offset and the second-bank theory both died on facts available in one
command.  Take the measurement first.

Verify by eye as well as by pixel count: a screenshot diff called a
build good that was drawing a window twice, because the diff was
comparing two scenes that differed for an unrelated reason.

And run it past someone using it.  `rmload killed it` was worth more
than a day of green tests.
