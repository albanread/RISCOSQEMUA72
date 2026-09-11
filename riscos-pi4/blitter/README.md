# GVFill — rectangle fills done by the host

RISC OS asks its video driver to fill rectangles through
`GraphicsV_Render` reason 2, `FillRectangle`.  BCMVideo declines it —
the dispatcher in `BCMVideo/s/GraphicsV` tests only for
`CopyRectangle` and `NOP` — so every window background, every `CLS` and
every menu erase is plotted by the CPU a word at a time.  Under
emulation that CPU is the slow part, which is why a large window
redraws in visible pieces.

`GVFill` claims GraphicsV, answers `FillRectangle` by handing the
rectangle to the `riscos-blitter` device, and returns
`GraphicsV_Complete` so the kernel skips its own plot.  Everything else
passes through with R4 untouched.

## What it accelerates

Only a plain colour.  The colour block is sixteen words of interleaved
`(ora, eor)`, and a fill is `(dest ORR ora) EOR eor` per word; when
every `ora` is all ones that reduces to a constant and the destination
need not be read.  Any other GCOL action, and any depth below 8bpp
where a pixel is not a whole number of bytes, is left to the kernel.

The block's shape was settled by measurement, not by reading: `hdr/
KernelWS` describes `GColAdr` as the address of an eight-word Ecf,
while `vduwrch` builds a sixteen-word ora/eor block, and since no
driver has ever implemented this operation nothing in the sources
settles which arrives.  Observing both call sites on a live desktop
showed sixteen words of `(ora, eor)` from each — `vduwrch`'s on the
stack, `vdugrafa`'s in kernel workspace with `FFFFFFFF`/`FF888888`
pairs, the desktop grey.  The KernelWS comment is stale.

## Building

Pure assembly with no relocations, so it needs only clang and
llvm-objcopy — not the `roscc` module linker the HostFS build uses,
which is Windows-only and only necessary when there is C to link.

```bash
clang --target=arm-none-eabi -mcpu=cortex-a72 -mfloat-abi=soft \
      -c blitmod.s -o blitmod.o
llvm-objcopy -O binary --only-section=.text blitmod.o 'GVFill,ffa'
```

`llvm-readobj -r blitmod.o` should report no relocations.  If it ever
reports any, the module is not position independent and will not load.

## Running

Put `GVFill,ffa` in the HostFS root and load it.  The `,ffa` suffix
matters: `*RMLoad` checks the filetype is &FFA, and the doorbell maps a
`,xxx` suffix to the type while leaving it in the name.

```
*RMLoad hostfs:$.GVFill,ffa
*BlitFill
```

`*BlitFill` paints 200x100 pixels at the top left, which proves the
device without involving the GraphicsV path.

The module maps its own device page with `OS_Memory 13` rather than
assuming an address: RISC OS builds its logical map from what the HAL
asks for, and a peripheral page nothing claimed simply aborts — which
is what happens if you try to reach 0xFD404000 directly.

## Status

Verified on a 1280x1024 desktop: fifteen fills accelerated on a single
redraw, two of them full-screen clears of 5120x1024 bytes, with the
desktop drawing correctly.

Not yet measured: a before-and-after timing.  A `*BlitBench` command
that ran the same fills with the hook off and on hung in the software
pass and was removed; the timing is still worth having.
