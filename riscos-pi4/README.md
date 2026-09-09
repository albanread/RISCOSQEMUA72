# RISC OS 5 on an emulated Raspberry Pi 4

![RECENT EXPERIMENT](https://img.shields.io/badge/RECENT_EXPERIMENT-c1121f?style=for-the-badge&labelColor=c1121f)

> [!CAUTION]
> **RECENT EXPERIMENT.** Days old, everything in flux, and nothing here is
> finished. It boots the ROOL SD image to the desktop with a keyboard, a mouse
> and a network, and the screen is 640×256 because nobody has answered the
> EDID question yet.
>
> The badge goes green when this is the fastest RISC OS A72 emulator there is,
> with an integrated debugging environment — and it works. Until then, treat
> everything below as a progress report rather than a product.


A QEMU fork that boots **RISC OS 5.30 on an emulated Cortex-A72 in 32-bit
mode** — `-M raspi4b` with the CPU in AArch32 — from the RISC OS Open SD
image to the desktop, with a USB keyboard, a mouse and an Ethernet-over-USB
interface that takes a DHCP lease from QEMU's own network during the boot
sequence. Power-on to an idle, networked desktop is about 27 seconds on an
i7-12700.

![RISC OS 5.30 booted from the ROOL SD image: NetSurf on the Welcome page](sd-desktop.png)

Before the disc and the keyboard it looked like this, and before that like
this, which is where most of the work went:

![The desktop from ROM alone](desktop.png)

![Reaching the supervisor prompt](first-boot.png)

Branch: `riscos-pi4`, on top of QEMU **v11.1.0**.

## Why

RISC OS 5's Raspberry Pi build targets a Cortex-A72 in AArch32. There was no
way to run it under emulation: stock QEMU boots the ROM into a hang inside the
HAL, and the ARMv4-class RiscPC emulators cannot execute ARMv8-A A32 code at
all. This fork closes that gap, so RISC OS work aimed at a Pi 4 can be done,
debugged and tested on a desktop.

## Status

Working: HAL and kernel bring-up, the MMU, secondary cores, the centisecond
tick and interrupt dispatch, I2C, the VCHIQ connect handshake, a framebuffer
allocated through the VideoCore property channel; **SDFS** from a card image
on EMMC2, and the `!Boot` sequence of the ROOL image running off it; **USB**
through the DWC2 controller — keyboard and mouse, on the root port or behind
a hub — which on a Pi 4 means the FIQ path RISC OS drives it from; and
**networking**, RISC OS's `EtherUSB` binding a CDC-Ethernet `usb-net` on
QEMU's user-mode network. Twelve seconds from power-on to the end of
PreDesk; 27 to an idle desktop with the network up.

Not working yet: the screen is 640×256, because `GET_EDID_BLOCK` is still
unanswered and RISC OS falls back to its smallest mode. `SET_CLOCK_RATE` is
still NYI. There is no way to get files into a running guest except through
the card image.

## What it changes

Four of these are plain QEMU bugs with nothing RISC OS-specific about them, and
are candidates for upstream:

| Commit | Who else it affects |
| --- | --- |
| `hw/arm/raspi`: use the AArch32 secondary boot stub for 32-bit guests | **Any** 32-bit guest on `raspi3b`/`raspi4b` — the stub is chosen by SoC generation rather than CPU state, so secondaries execute an AArch64 stub as ARM |
| `hw/i2c/bcm2835_i2c`: allow byte access | Any guest that uses `STRB` on the FIFO, which is a byte port |
| `hw/i2c/bcm2835_i2c`: only start a transfer on ST, and buffer the TX FIFO | Any guest following the documented sequence — set A, set DLEN, fill FIFO, *then* set ST |
| `hw/arm/bcm2838`: connect the system timer to the GIC | Any guest using the system timer through the GIC on `raspi4b`; its compare outputs only ever reached the legacy interrupt controller |
| `hw/arm/bcm2838`: put the SD card on EMMC2 | Any `raspi4b` guest that uses the card — the BCM2711 keeps it on EMMC2 and Linux's device tree says so too; QEMU had it on the GPIO block's legacy mux |
| `hw/intc/arm_gic`: legacy nFIQ inputs, through the GICv2 interrupt-signal bypass | Any SoC that wires an older interrupt controller into a GIC-400's legacy inputs; the BCM2711 does, and RISC OS relies on it |
| `hw/usb/hcd-dwc2`: keep `GINTSTS.HCHINT` in step with `HAINTMSK`, and the IRQ level per device | Any guest whose driver defers a channel interrupt by masking it — the FIQ state machine RISC OS inherited from the Pi Linux driver does |
| `hw/usb/dev-network`: `rndis=off`, a CDC-only `usb-net` | Any guest whose USB stack announces a device in its first configuration only |

The rest are missing devices and unanswered firmware calls:

- `hw/misc`: BCM2835 mailbox **channel 0** (power management) — defined since
  the mailbox was first modelled, never given a peer
- `hw/misc`: a **VCHIQ peer** for mailbox channel 3, plus the VC→ARM and
  ARM→VC doorbells
- `hw/misc/bcm2835_property`: the touch and GPIO virtual buffer tags, and the
  GPIO state tags
- `hw/arm/bcm2838`: an unimplemented-device stub over the PCIe root complex,
  so probing it reads as "link down" instead of taking an external abort
- `hw/intc/bcm2838_ic`: the **BCM2711 legacy interrupt controller** — the
  per-core IRQ and FIQ banks at ARMC+0x200 from the datasheet — and the DWC2
  line split so that USB reaches it as well as the GIC. RISC OS runs its USB
  host controller from the FIQ handler and enables that FIQ here; QEMU had
  the address shadowed twice over
- `scripts/symlink-install-tree`: survive a build host without symlink
  permission — a local Windows workaround, **not** for upstream

## The design principle

> Rather than emulating VideoCore, emulate the functions needed by RISC OS. Be
> as soft and fake as possible and emulate hardware only if we are desperate.
> We probably do need interrupts though — the whole system is running off
> timers.

Real interrupts where the OS genuinely depends on them; functional shims
everywhere else. The VCHIQ peer is the clearest example: about 250 lines that
complete the connect handshake and then *refuse* every service the guest opens.
It models no hardware. It answers questions.

That refusal is deliberate rather than lazy — accepting the sound service walks
the guest into another blocking wait, while declining leaves the GPU mode path
switched off so display setup falls back to the property channel, which QEMU
already models completely.

## Building on Windows

There is no supported non-MSYS2 route. In an MSYS2 **MINGW64** shell:

```bash
pacman -S --needed base-devel git \
  mingw-w64-x86_64-{gcc,glib2,pixman,zlib,pkgconf,ninja,meson,python,capstone,libpng,libslirp}

mkdir build && cd build
../configure --target-list=aarch64-softmmu --enable-plugins --disable-werror \
    --disable-gtk --disable-sdl --disable-vnc --disable-docs \
    --disable-guest-agent --enable-capstone --disable-spice --enable-slirp
ninja
```

Two things that will bite you:

- **Do not drop capstone.** Without it the monitor's `x/i` reports "Asm output
  not supported on this arch", and disassembly is most of what you need here.
  libpng is needed for `screendump` in PNG rather than PPM.
- The built binary needs `<msys64>/mingw64/bin` on `PATH` for its glib and
  pixman DLLs. Without it Windows terminates it silently — no error, no output.

## Running

The ROM is **not** included: it is RISC OS Open's, and free to download is not
the same as free to redistribute. Get the "RPi ROM stable" zip from
<https://www.riscosopen.org/content/downloads/raspberry-pi> and take
`RISCOS.IMG` out of it.

The SD image is theirs too: the "RISC OS Pi" zip from the same page unpacks
to a raw card image. QEMU wants a card of 2 GiB or under to be an exact power
of two, so pad it (`truncate -s 2G card.img`); it also wants it writable, and
`snapshot=on` keeps the file pristine across runs.

```bash
python riscos-pi4/tools/mkcmos.py --riscos-src <path>/BCM2835/RiscOS \
    --rom RISCOS.IMG --unplug 106 --filesystem 192 -o cmos.bin

qemu-system-aarch64 -M raspi4b -cpu cortex-a72,aarch64=off \
    -kernel RISCOS.IMG \
    -device loader,file=cmos.bin,addr=0x510000,force-raw=on \
    -drive file=card.img,if=sd,format=raw,snapshot=on \
    -netdev user,id=n0 \
    -device usb-hub,bus=usb-bus.0,port=1 \
    -device usb-kbd,bus=usb-bus.0,port=1.1 \
    -device usb-mouse,bus=usb-bus.0,port=1.2 \
    -device usb-net,netdev=n0,rndis=off,bus=usb-bus.0,port=1.3 \
    -qmp tcp:127.0.0.1:4455,server,nowait
```

Three things about that line:

- `--filesystem 192` makes RISC OS boot from SDFS. Leave it and the `-drive`
  out and it boots to the desktop from ROM alone, with nothing behind the SD
  icon.
- The DWC2 controller has one root port, hence the hub. A lone `usb-kbd` can
  sit on `port=1` directly. Name the ports: a bare `-device usb-kbd` lands
  behind an automatic hub, which works but hides what you are testing.
- The ROOL image's `Choices:Internet.Startup` runs `DHCPExecute -w ej0` and
  waits for that Ethernet-over-USB interface until it appears or Escape is
  pressed — stock RISC OS behaviour on a Pi with no network. `usb-net` with
  `rndis=off` *is* `ej0`: RISC OS's `EtherUSB` binds its CDC configuration,
  and slirp's DHCP server answers during the boot. Without it, press Escape
  and the desktop arrives with the usual "Machine startup has not completed
  successfully" box.

**Both parts of the first pair are needed to reach the desktop.** A Raspberry Pi has no CMOS
chip, so the HAL takes its settings from a blob the firmware leaves in memory
after the OS image — and under emulation QEMU *is* the firmware. Without one
the HAL blanks the settings and the machine boots unconfigured; without
`--unplug 106` the EtherGENET driver dereferences a null pointer and RISC OS
prints a data abort on its own console. `mkcmos.py` derives both the blob and
the address to load it at from RISC OS's own headers and ROM image.

`-cpu cortex-a72,aarch64=off` is the load-bearing part. `-M raspi4b` hard-codes
its CPU, so it is widely assumed `-cpu` does nothing there — but the *property*
still lands, and without it the ROM's ARM32 vector table is decoded as AArch64
and executed as garbage.

`riscos-pi4/tools/` has a boot probe that samples the PC and takes a
screendump over QMP, two bare-metal instruction-rate benchmarks, and a
self-contained regression test for the mailbox channel-0 fix that does not need
RISC OS at all. See `tools/README.md`.

`riscos-pi4/DESIGN.md` is the full record: what was measured, what was tried,
which hypotheses were wrong, and why each fix is shaped the way it is.

## Licence

QEMU is GPL-2.0-or-later and this fork inherits that; see `COPYING` and
`LICENSE` in the repository root, which are unchanged.

The new device models added here — `hw/misc/bcm2835_mbox_power.c`,
`hw/misc/bcm2835_vchiq.c` and `hw/intc/bcm2838_ic.c`, with their headers —
carry GPL-2.0-or-later notices matching the surrounding Raspberry Pi code.
`hw/i2c/bcm2835_i2c.c` is MIT (© 2024 Rayhan Faizel) and keeps its original
notice; only its contents are modified. The changes to `hw/intc/arm_gic*`,
`hw/usb/hcd-dwc2.c` and `hw/usb/dev-network.c` are under those files' own
GPL notices, unchanged.

No RISC OS Open material is included in this repository — no ROM images, no
disc images, no documentation.
