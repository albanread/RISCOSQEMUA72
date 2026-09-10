# tools — reproducing the findings

Supporting material for [`DESIGN.md`](../DESIGN.md). Nothing
here is part of a build; it exists so the measurements in that document can be
re-run.

## Prerequisites

- QEMU for Windows (tested: 11.1.0, `qemu-w64-setup-20260811.exe` from
  qemu.weilnetz.de, SHA-512 verified, installed with `/S /D=<dir>` to
  `%LOCALAPPDATA%\Programs\qemu`). Override the location with `QEMU_DIR`.
- The ROOL Raspberry Pi ROM: `BCM2835.5.30.zip` from
  <https://www.riscosopen.org/content/downloads/raspberry-pi>, which contains
  `RISCOS.IMG`.
- clang and ld.lld (the installed LLVM) to build the benchmarks.

## run.py — the launch, and snapshots

`run.py` is the canonical launch wrapped up: it creates a qcow2 overlay
over the card image on first use (so the card image stays pristine and a
snapshot carries machine *and* disc state), boots the machine, and:

    run.py                     boot from scratch
    run.py --save desktop      save machine+disc as 'desktop' once the
                               desktop is painted, then keep running
    run.py --snapshot desktop  cold-start from that snapshot (<1 s to a
                               painted desktop)

The window's system menu also carries "Load snapshot", which rewinds the
running machine to the 'desktop' snapshot.

One rule with teeth: **always shut the emulator down cleanly** — close
the window, or `quit` over QMP.  The overlay is a real disc to the
guest; killing the process hard is yanking the power mid-write, and
SDFS/FileCore can be left with a torn update.  Diagnosed live: after a
day of hard kills a fresh boot stopped with `SDFS error &1E4 on drive
0 : disc error` just after SDFS started; `qemu-img check` passes in
that state (the qcow2 is fine — the filesystem *inside* the disc is
what tore; only the guest can see it).  Recovery is to quarantine the
overlay and let `run.py` mint a fresh one from the pristine image.

## Boot probe

    python probe.py raspi4b RISCOS.IMG 30 -- -cpu cortex-a72,aarch64=off
    python probe.py raspi2b RISCOS.IMG 30

Expect the PC to sit at `0x00016764` — the mailbox poll loop RISC OS never
leaves, because nothing answers VideoCore mailbox channel 0. See §2.2 of the
document. Add `-- ... -trace enable=bcm2835_mbox*` to see the traffic.

To watch it get past that: copy the ROM, patch the offending `bl` at file offset
`0x40cc` from `6c 09 00 eb` to `00 00 a0 e1` (a `nop`), and probe the copy. RISC
OS then reaches its own kernel at `0xFC0…`.

## Mailbox channel-0 regression test

`mboxtest.s.in` posts the exact message the RISC OS HAL posts — channel 0,
"power up the USB host controller" — and waits for the reply with a bounded
spin, so a missing peer fails the test instead of hanging.

    sed 's/UARTB/0xFE201000/; s/MBOXB/0xFE00B800/' mboxtest.s.in > mboxtest.s
    clang -target armv7a-none-eabi -c mboxtest.s -o mboxtest.o
    ld.lld -Ttext=0x10000 --oformat binary -o mboxtest.bin mboxtest.o
    qemu-system-aarch64 -M raspi4b -cpu cortex-a72,aarch64=off \
        -kernel mboxtest.bin -display none -serial stdio -serial null

- Stock QEMU 11.1.0 prints **`TIMEOUT`** (verified on `raspi2b` and `raspi4b`).
- With the `bcm2835-mbox-power` device it prints **`00000080`** — the reply,
  carrying channel 0 in the low nibble and the USB HCD bit set.

## Building a CMOS blob

A Raspberry Pi has no CMOS chip. The HAL reads its non-volatile settings from a
blob the firmware leaves in memory immediately after the OS image, checks a
version word, and blanks the lot to 0xFF if it is out of range -- which is what
happens with stock QEMU, and why the machine boots unconfigured and never
reaches the desktop.

Under emulation QEMU is the firmware, so `-device loader` can supply that blob:

    python mkcmos.py --rom RISCOS.IMG --base CMOS --unplug EtherGENET \
        --symbols cmos-symbols-530.json -o cmos.bin

    qemu-system-aarch64 ... -device loader,file=cmos.bin,addr=0x510000,force-raw=on

Nothing in it is transcribed by hand. The CMOS layout comes from RISC OS's own
`hdr/CMOS` by simulating ObjAsm's `^`/`#` storage map, and the defaults from the
kernel's `DefaultCMOSTable`; the load address comes from the ROM's own `OSIm`
header. It cross-checks the layout against the address comments in the header
and reports any that disagree, and it mirrors the kernel's backwards-indexed
unplug table rather than assuming the arithmetic.

Two options keep a source checkout out of the recipe:

- `--symbols cmos-symbols-530.json` loads the resolved symbol table instead
  of parsing the headers (build it once with `--dump-symbols FILE` alongside
  `--riscos-src`); the committed table lives next to this README. With
  `--base CMOS` the kernel defaults are not needed either.
- `--unplug EtherGENET` names the module instead of numbering it: with
  `--rom` it walks the ROM module chain exactly as `Kernel/s/ModHand` does
  -- size word, module, title at `+0x10`, chunks counted from zero -- and
  prints the number it resolved to. `--unplug 106` remains valid.

`--unplug EtherGENET` disables the Pi 4's GENET Ethernet driver in RISC OS
5.30. With no Ethernet controller to find, its `genet_attach` returns `ENXIO`
but leaves `nicifp` NULL, and a callback booked during module init then
dereferences it -- a data abort on address `0x18`. That is a bug in the
driver, not in the emulation, and no device model can prevent it; the only
lever is not to start the module.

`--language 1` makes the supervisor prompt a deliberate choice. The stock
default is 11, the Desktop.

**A valid checksum means the kernel skips `cmos_reset` entirely**, so the blob
has to carry every setting, not just the one you came for. A blob of zeros with
a correct checksum boots to a black screen: the abort is gone and so is
everything else.

## Instruction-rate benchmarks

`bench.s.in` is a register-only loop (2 instructions × 100M); `bench2.s.in` adds
a load and a store per iteration walking a 4 MB buffer with a 64-byte stride
(6 instructions × 20M). Both time themselves with the BCM system timer and print
the elapsed microseconds as hex over the PL011.

Substitute the peripheral addresses for the machine, then assemble flat:

    # raspi2b (BCM2836): UARTB=0x3F201000 TIMB=0x3F003004
    # raspi4b (BCM2711): UARTB=0xFE201000 TIMB=0xFE003004
    sed 's/UARTB/0xFE201000/; s/TIMB/0xFE003004/' bench.s.in > bench.s
    clang -target armv7a-none-eabi -c bench.s -o bench.o
    ld.lld -Ttext=0x10000 --oformat binary -o bench.bin bench.o
    qemu-system-aarch64 -M raspi4b -cpu cortex-a72,aarch64=off \
        -kernel bench.bin -display none -serial stdio -serial null

MIPS = instructions ÷ (printed microseconds ÷ 1e6). Measured on an i7-12700:
~2000 M/s for `bench`, ~500 M/s for `bench2`.
