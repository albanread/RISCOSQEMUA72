# RISC OS 5 on an emulated Raspberry Pi 4

A QEMU fork that boots **RISC OS 5.30 on an emulated Cortex-A72 in 32-bit
mode** — `-M raspi4b` with the CPU in AArch32 — far enough to reach its
supervisor prompt with a framebuffer on screen.

![RISC OS 5.30 on an emulated Pi 4](first-boot.png)

Branch: `riscos-pi4`, ten commits on top of QEMU **v11.1.0**.

## Why

RISC OS 5's Raspberry Pi build targets a Cortex-A72 in AArch32. There was no
way to run it under emulation: stock QEMU boots the ROM into a hang inside the
HAL, and the ARMv4-class RiscPC emulators cannot execute ARMv8-A A32 code at
all. This fork closes that gap, so RISC OS work aimed at a Pi 4 can be done,
debugged and tested on a desktop.

## Status

Working: HAL and kernel bring-up, the MMU, secondary cores, the centisecond
tick and interrupt dispatch (~2,900 interrupts serviced per 30 seconds), I2C,
the VCHIQ connect handshake, and a framebuffer allocated through the VideoCore
property channel.

Not working yet: no keyboard (see below), no disc, one remaining data abort,
and `GET_EDID_BLOCK` / `SET_CLOCK_RATE` are unimplemented property tags.

## What it changes

Four of these are plain QEMU bugs with nothing RISC OS-specific about them, and
are candidates for upstream:

| Commit | Who else it affects |
| --- | --- |
| `hw/arm/raspi`: use the AArch32 secondary boot stub for 32-bit guests | **Any** 32-bit guest on `raspi3b`/`raspi4b` — the stub is chosen by SoC generation rather than CPU state, so secondaries execute an AArch64 stub as ARM |
| `hw/i2c/bcm2835_i2c`: allow byte access | Any guest that uses `STRB` on the FIFO, which is a byte port |
| `hw/i2c/bcm2835_i2c`: only start a transfer on ST, and buffer the TX FIFO | Any guest following the documented sequence — set A, set DLEN, fill FIFO, *then* set ST |
| `hw/arm/bcm2838`: connect the system timer to the GIC | Any guest using the system timer through the GIC on `raspi4b`; its compare outputs only ever reached the legacy interrupt controller |

The rest are missing devices and unanswered firmware calls:

- `hw/misc`: BCM2835 mailbox **channel 0** (power management) — defined since
  the mailbox was first modelled, never given a peer
- `hw/misc`: a **VCHIQ peer** for mailbox channel 3, plus the VC→ARM and
  ARM→VC doorbells
- `hw/misc/bcm2835_property`: the touch and GPIO virtual buffer tags, and the
  GPIO state tags
- `hw/arm/bcm2838`: an unimplemented-device stub over the PCIe root complex,
  so probing it reads as "link down" instead of taking an external abort
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
  mingw-w64-x86_64-{gcc,glib2,pixman,zlib,pkgconf,ninja,meson,python,capstone,libpng}

mkdir build && cd build
../configure --target-list=aarch64-softmmu --enable-plugins --disable-werror \
    --disable-gtk --disable-sdl --disable-vnc --disable-docs \
    --disable-guest-agent --enable-capstone --disable-spice
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

```bash
qemu-system-aarch64 -M raspi4b -cpu cortex-a72,aarch64=off \
    -kernel RISCOS.IMG -display none -serial null \
    -qmp tcp:127.0.0.1:4455,server,nowait
```

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

The new device models added here — `hw/misc/bcm2835_mbox_power.c` and
`hw/misc/bcm2835_vchiq.c`, with their headers — carry GPL-2.0-or-later notices
matching the surrounding Raspberry Pi code. `hw/i2c/bcm2835_i2c.c` is MIT
(© 2024 Rayhan Faizel) and keeps its original notice; only its contents are
modified.

No RISC OS Open material is included in this repository — no ROM images, no
disc images, no documentation.
