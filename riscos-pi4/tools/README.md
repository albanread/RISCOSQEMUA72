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

## rom.py — which ROM and CMOS a launch boots

The Python half of `rom.zsh`, so a Windows machine and the farm start the
same way a Mac does, off the same `mkrom.py` and `mkcmos.py`.  `run.py`
and `farm.py` import it; nothing else needs to know it is there.

    run.py --hostfs DIR              a share brings HostFS and its
                                     icon-bar filer into the ROM
    run.py --hostfs DIR --boot hostfs
                                     boot the share's own !Boot instead
                                     of the card's; the card stays on,
                                     reachable as SDFS::0
    run.py --modules a,ffa b,ffa     further modules, in init order

The spliced image is cached beside the stock ROM under a hash of the ROM
and every module's contents, so editing a module rebuilds it and a
relaunch does not.  With nothing to splice, the stock `RISCOS.IMG` boots
untouched.  A module whose title is already in `--modules` stands instead
of the default build, so you can test your own HostFS by naming it.

Two things worth knowing before you use it:

- **A snapshot decides its own ROM.** `-loadvm` restores RAM, and the ROM
  lives in RAM: `-kernel` is loaded and then overwritten.  So a machine
  saved without HostFS in ROM comes back without it however you launch
  it, and `run.py` says so rather than letting you wonder.  Re-save from
  a cold boot to change what a snapshot has.  `--boot` with `--snapshot`
  is refused outright: one chooses what to boot, the other skips the
  boot.
- **A card can shadow the ROM.** `!Boot.Choices.Boot.PreDesk.BootHostFS`
  is an unconditional `RMLoad`, and `RMLoad` replaces a ROM module of the
  same name, so a card carrying it ends up on its own older HostFS.
  FSDESIGN-V1.md §13 R2 has the fix (an `RMEnsure`) and who owns it.

`farm.py up` takes the same `--boot` and `--modules`, and `--rom-hostfs`
to splice HostFS in.  That one is opt-in on the farm and not on `run.py`,
because every instance has a share — so the Mac rule "a share brings
HostFS" would splice into all four — and the farm's base image carries
exactly the `BootHostFS` above, which would shadow it anyway.  `--boot
hostfs` turns it on by itself, since reaching the share needs the module
in the ROM to get there.

## A farm with no cards

The SD images are being retired, so an instance's share can be the whole
machine.  Put a tree holding `!Boot` (and, for a dev machine, the DDE)
in `qemu-farm/<name>/share`, then:

    farm.py up alpha --no-card --boot hostfs

`--boot hostfs` implies `--rom-hostfs`, so the ROM gets HostFS and its
filer and the CMOS is built with `FileSystem HostFS`, kept on the share
as `CMOS,ff2` so `*Configure` survives a power-off.  Measured here: a
clean desktop 33 s after launch with no card attached at all, and `cc`
compiling a source dropped into the share from the host.

Two things the tree needs, both once, when it is built from a card:

- **`PreDesk.BootDDE` repointed** from `SDFS::0.$` to `HostFS:$`, or the
  DDE paths point at a card that is not there.
- **`PreDesk.BootHostFS` removed.**  It is an unconditional `RMLoad` of
  the card's own HostFS, and `RMLoad` replaces a ROM module of the same
  name, so leaving it in means the machine runs 1.01 from the card
  rather than 2.00 from the ROM (FSDESIGN-V1 §13 R2).  With no card it
  is pointing at nothing anyway.

And one trap when you build such a tree: **`*Copy` onto a Windows share
stops at the first name the host cannot store** -- `>` and the rest of
`< > : " / \ | ? *` -- and reports one line, five levels down, having
abandoned everything after it.  macOS takes those names, so a tree that
copies cleanly there can arrive 40% short here.  Verify with `*Count` on
both sides, per directory: the file counts are the only thing that
catches it.  ROS_PRIVATE#6.

## macOS: the launch, and the ticker

`run-macos.sh` is the launch line from `../README.md` with `-display
metal` in place of `-display dx11`; `riscos-pi4/MACOS.md` is the port's
record. It expects the ROM, the CMOS blob and (optionally) the card
image in `riscos-images/` beside the repo root, which `RISCOS_IMAGES`
overrides:

    riscos-pi4/tools/run-macos.sh
    DISPLAY_OPT=cocoa riscos-pi4/tools/run-macos.sh   # QEMU's own display
    DISPLAY_OPT=metal,scaling=nearest,scanlines=on riscos-pi4/tools/run-macos.sh

On an Intel Mac, build the four dependencies Homebrew cannot pour
there with `build-deps-macos.sh` before configure — MACOS.md §9 says
why and what it verified. The mailbox regression below needs no
`ld.lld` on a Mac either: assemble with
`clang -target armv7a-none-eabi -c` and pull the `.text` bytes out of
the ELF — the sources are position-contained.

`ticks.py` measures what the guest's 100 Hz ticker is actually worth, by
counting the system timer's compare-1 expiries over a settled desktop:

    python riscos-pi4/tools/ticks.py 30 30
    timer #1:  100.0/s over 30.0s   gap median 9.96 ms, p99 12.38 ms, worst 12.54 ms

It works on any host with the `log` trace backend, not only macOS, so the
two platforms' clocks can be compared with the same instrument.

## make-release.sh — the user release, as a Mac app

`make-release.sh` builds the end-user release: one self-contained
application, `RISCOSQEA72v<N>.app`, and a disk image to hand out. Inside
the app are the emulator with every Homebrew library it links copied
into `Contents/Frameworks` and its load commands rewritten (so it runs on
a Mac with no Homebrew), the stock 5.30 ROM with HostFS and HostFSFiler
spliced in by `mkrom.py`, the end-user disc as a zip the app unpacks into
`~/RISCOS` on its first run, and `app/launcher.zsh`, which is what a
double click starts. It is `run-app.sh`'s user persona — no QMP socket,
Apple Events the only control channel — with the paths settled for an
installed app.

    riscos-pi4/tools/make-release.sh 1
    FS_ZIP=/path/end_user_fs.zip riscos-pi4/tools/make-release.sh 2

The ROM and the disc come from the private repo by default (`ROS_PRIVATE`,
`ROM` and `FS_ZIP` override). The disc is the end-user zip minus whatever
`STRIP` names — by default the DDE, the Store and PackMan, the Pi's boot
partition image, Ghostscript, the unused themes, the manuals and the
games: everything that is not the desktop and its applications, and
whatever goes is taken off the Pinboard too — with its CMOS forced to
boot from HostFS. Everything lands
in `build-macos/release/`: the app, the `.dmg` (app, Read Me, Applications
link) and the cut-down disc as a zip of its own. `RELEASE.txt` inside the
app records what went in: commit, ROM and disc hashes, module versions,
minimum macOS, the libraries. Signing is ad-hoc unless `SIGN_ID` names a
Developer ID, and the minimum macOS is whatever the newest binary demands
(26.0 with today's Homebrew bottles).

Installed, the app logs to `~/Library/Logs/RISCOSQEA72/` and takes two
settings from `defaults`:

    defaults write com.github.albanread.RISCOSQEA72 mode 1920x1200
    defaults write com.github.albanread.RISCOSQEA72 disc ~/Elsewhere

A developer can still reach an installed app over QMP, because the
launcher passes its arguments on:

    open -n RISCOSQEA72v1.app --args -qmp unix:/tmp/q.sock,server,nowait

## mkbootfx.py — the boot screen

The ROM's BootFX module paints a splash JPEG and a progress bar from
the first mode change to the desktop, and a logo while the ROM
initialises; its "Raspberry" set is compiled in, so nothing in `!Boot`
can change it. `mkbootfx.py` builds our Acorn set from the masters in
`app/bootfx/` — `splash.svg` and `logo.svg` rendered with `rsvg-convert`
and made baseline JPEGs by `sips`, and three flat 640×40 sprites
(border, fill, bar) Squash-compressed with `compress -b 12`, which is
exactly the stream the ROM's own `Bar24` turns out to be:

    riscos-pi4/tools/mkbootfx.py
    1920x1080,c85      67367 bytes  (room in the ROM: 116220)
    Logo,c85            6766 bytes  (room in the ROM: 8141)
    Bar24,fca           4174 bytes  (room in the ROM: 23004)

`mkrom.py -r NAME=FILE` writes a file over a ResourceFS block in place
(the length words updated, the rest zeroed, the type taken from a `,xxx`
suffix), so the three fit where ROOL's were and nothing else in the
image moves. `rom.zsh` splices them into every launch by default
(`RISCOS_BOOTFX=` boots with ROOL's), and `make-release.sh` into the
app's ROM.

One thing this uncovered: a HostFS boot used to wipe the splash and
scatter fragments of FileSwitch's messages over the boot screen. That
was FileSwitch's Service_Reset banner: HostFS declared no start-up text,
and FileSwitch's fallback prints its "UntFS" message straight from the
compressed message file, VDU codes and all. HostFS 2.05 declares one
("HostFS 2.05"), as SDFS does ("Piccolo Systems SDFS").

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

On macOS Xcode has no `ld.lld`, and none is needed: `clang -target
armv7a-none-eabi -c` emits an ELF, and the sources are position-contained
(absolute peripheral immediates, PC-relative branches, no relocations), so
the flat binary is just the object's `.text` bytes — extract them with any
ELF reader (a dozen lines of Python over the section headers) and boot that.
Verify the build by checking the object has no `.rel.*` section before
extracting; `mboxtest.s` has the same property, with `MBOXB=0xFE00B800` as
its second substitution.
