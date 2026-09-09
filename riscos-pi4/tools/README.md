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
