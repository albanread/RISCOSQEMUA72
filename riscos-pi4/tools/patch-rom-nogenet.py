#!/usr/bin/env python3
"""Neutralise the EtherGENET module in a RISC OS 5.30 Pi ROM image.

    python patch-rom-nogenet.py RISCOS.IMG RISCOS-nogenet.IMG

The ROOL 5.30 ROM carries EtherGENET, the driver for the Pi 4's on-chip
GENET Ethernet MAC. The driver starts late, from the disc image's network
setup, and walks its device list unconditionally: with no GENET hardware
(and no device tree reaching a -kernel guest) the device pointer is NULL
and init dies at `LDR pc, [r0, #0x18]` with r0 = 0 -- a data abort that
stops the whole boot, leaving only

    Error: DataAbort:Abort on data transfer at &FC3FB800 (Error Number
    &80000002)

at the top of the screen.

The clean fix is the CMOS unplug bit for the module's ROM chunk
(`mkcmos.py --unplug 106` in RISC OS 5.30), but that needs a copy of the
RISC OS sources for the CMOS symbol addresses. This tool is the
source-free equivalent: it finds the EtherGENET module header in the image
by title (so a ROM rebuild that moves it still works) and

  * replaces the module's init with `MOV r0,#0 ; MOV pc,lr` -- init that
    succeeds at doing nothing, so the kernel does not complain either, and
  * zeroes the service and SWI handler entries, which the kernel then
    skips outright.

Everything else in the image stays byte-for-byte identical. RISC OS 5.30
does not checksum the running copy against the image, so no checksum
repair is needed; that was verified by booting a patched image to the
desktop.

Tested against the 5.30 RPi stable ROM ("EtherGENET\t0.30" era). When the
GENET MAC is modelled for real, stop using the patched image.
"""
import struct
import sys


def find_module(data, title):
    """Return the file offset of the ROM module header titled `title`."""
    needle = title.encode() + b"\0"
    pos = data.find(needle)
    while pos != -1:
        # The header has the title's offset at +0x10, and the string sits
        # a few dozen bytes in. Scan back for a plausible header whose
        # title field lands exactly on this string.
        for base in range(pos & ~3, max(0, (pos & ~3) - 0x8000), -4):
            init, die, svc = struct.unpack_from("<III", data, base + 4)
            title_off = struct.unpack_from("<I", data, base + 0x10)[0]
            if not (0x20 < init < 0x20000 and 0x20 < die < 0x20000):
                continue
            if svc > 0x20000 or not (0x14 < title_off < 0x20000):
                continue
            if base + title_off == pos:
                return base
        pos = data.find(needle, pos + 1)
    return None


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <RISCOS.IMG> <out.img>")

    src, dst = sys.argv[1], sys.argv[2]
    data = bytearray(open(src, "rb").read())
    base = find_module(bytes(data), "EtherGENET")
    if base is None:
        sys.exit("EtherGENET module not found -- not a 5.30 Pi ROM?")

    def word(off):
        return struct.unpack_from("<I", data, base + off)[0]

    print(f"EtherGENET header at {base:#x}: init=+{word(4):#x} "
          f"service=+{word(0xC):#x} swi=+{word(0x20):#x}")

    struct.pack_into("<II", data, base + word(4), 0xE3A00000, 0xE1A0F00E)
    struct.pack_into("<I", data, base + 0x0C, 0)   # no service handler
    struct.pack_into("<I", data, base + 0x20, 0)   # no SWI handler
    struct.pack_into("<I", data, base + 0x24, 0)   # no SWI decoder

    open(dst, "wb").write(bytes(data))
    print(f"wrote {dst} ({len(data)} bytes) -- EtherGENET is inert")


if __name__ == "__main__":
    main()
