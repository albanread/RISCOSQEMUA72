#!/usr/bin/env python3
"""mkrom — splice our own modules into a stock RISC OS ROM image.

The kernel initialises ROM modules from a chain that starts at
SysModules_Info+4 and is linked by size words: the word four bytes
before each module holds that module's length plus the next size word,
and a zero word terminates it (Kernel/s/ModHand walks it exactly this
way, with no checksum and no bounds validation).  So appending at the
terminator is the one operation the format was built for, and a module
appended there initialises after every stock module — FileSwitch
included — which is what a filing system needs.

The image is not grown.  Between the terminator and the end-of-ROM
footers there is slack (268 KiB on the 5.31 image), holding the credits
text and then 0xFF padding.  The splicer writes the new modules over
the terminator, moves the credits forward by the same amount, lets the
padding absorb the difference, and preserves everything from the
extended footer onward byte-for-byte at its original offset.  No header
field is touched: the kernel locates the end-of-ROM footers from a
build-time constant, not the OSIm header, and the HAL reads image_size
only for the CMOS import address and the relocate-to-RAM copy — so with
image_size unchanged, every consumer of the image stays correct.
(If the slack is ever exhausted, growing the image means updating
OSHdr_ImageSize AND OSHdr_CompressedSize — the HAL copies
CompressedSize, not ImageSize, when flags&1 — and re-placing the CMOS
blob.  That is deliberately not implemented.)

Usage:
    mkrom.py base.img -m module [-m module ...] -o out.img
    mkrom.py base.img --list
"""

import argparse
import hashlib
import os
import struct
import sys

OS_HDR = 0x10000          # file offset of the OSIm header
MAGIC = 0x6D49534F        # 'OSIm'


def read_header(data):
    magic, flags, image_size = struct.unpack_from('<III', data, OS_HDR)
    if magic != MAGIC:
        raise SystemExit(f'not a RISC OS image: OSIm magic missing '
                         f'(got {magic:#x})')
    return flags, image_size


def walk_chain(data, start):
    """The kernel's module-chain walk, from Kernel/s/ModHand.

    Returns (titles, terminator_offset) where terminator_offset is the
    file offset of the zero size word.  None if the walk degenerates.
    """
    mods, m = [], start
    while m + 4 <= len(data):
        length = struct.unpack_from('<I', data, m - 4)[0]
        if length == 0:
            return mods, m
        if length < 0x20 or m + length > len(data):
            return None
        t_off = struct.unpack_from('<I', data, m + 0x10)[0]
        if not 0x10 < t_off < length:
            return None
        t = m + t_off
        e = data.find(b'\0', t, t + 90)
        if e <= t:
            return None
        s = data[t:e]
        if not all(32 <= c < 127 for c in s) or len(s) < 2:
            return None
        mods.append(s.decode())
        m += length
    return None


def find_chain(data):
    """Find the chain start the way a wrong-length word would be found:
    try every offset, keep the longest walk that reaches an exact zero
    word through printable titles."""
    best = None
    for s in range(OS_HDR + 0x40, min(len(data), 0x80000), 4):
        r = walk_chain(data, s)
        if r and len(r[0]) >= 40 and (best is None or len(r[0]) > len(best[0])):
            best = r
    if not best:
        raise SystemExit('no module chain walks — not a RISC OS ROM image?')
    return best


def find_tail(data):
    """Offset from which the ROM tail must be preserved verbatim.

    The extended ROM footer's header word sits 24 bytes before the end;
    its low half is the length of the tag structure that precedes it
    (Kernel/s/Middle's ExtendedROMFooter layout).  If it doesn't parse,
    fall back to protecting just the standard 20-byte footer.
    """
    end = len(data)
    word = struct.unpack_from('<I', data, end - 24)[0]
    ext_len = word & 0xFFFF
    if 0 < ext_len < 0x1000 and end - 24 - ext_len > 0:
        return end - 24 - ext_len
    return end - 20


def module_title(body):
    """Title of a RISC OS relocatable module, or None if malformed."""
    if len(body) < 0x20:
        return None
    t_off = struct.unpack_from('<I', body, 0x10)[0]
    if not 0x10 < t_off < len(body):
        return None
    t = t_off
    e = body.find(b'\0', t, t + 90)
    if e <= t:
        return None
    s = body[t:e]
    if not all(32 <= c < 127 for c in s) or len(s) < 2:
        return None
    return s.decode()


def splice(base, module_paths):
    """Append modules to the chain; returns the new image bytes."""
    _flags, image_size = read_header(base)
    rom_end = OS_HDR + image_size
    if len(base) != rom_end:
        raise SystemExit(f'image is {len(base):#x} bytes but the header '
                         f'says {rom_end:#x} — padded base images only')

    # term is the walk-stop offset: the zero terminator word sits at
    # term-4 (the convention BOOTDESIGN and mkcmos use).
    titles, term = find_chain(base)
    tail = find_tail(base)

    # bytes that live after the terminator and are not padding: the
    # credits text.  They move forward by exactly what we insert.
    shift_end = term
    while shift_end < tail and base[shift_end] not in (0x00, 0xFF):
        shift_end += 1

    free = tail - term                # credits + padding
    bodies = []
    for p in module_paths:
        body = open(p, 'rb').read()
        title = module_title(body)
        if title is None:
            raise SystemExit(f'{p}: not a relocatable module')
        print(f'mkrom: + {title} ({len(body)} bytes) from {p}')
        bodies.append((title, body))

    # per module: [size word][body], then the zero terminator.  The size
    # word covers the module plus the next size word.
    insert = b''.join(struct.pack('<I', len(b) + 4) + b for _, b in bodies) \
        + struct.pack('<I', 0)
    needed = len(insert) - 4          # net growth inside the slack
    if needed > free:
        raise SystemExit(f'{needed} bytes needed, {free} of slack: '
                         'image would have to grow — not supported')

    out = bytearray()
    out += base[:term - 4]            # everything up to the old zero word
    out += insert                     # our modules, then a new zero word
    out += base[term:shift_end]       # credits, moved forward
    out += b'\xFF' * (tail - len(out))  # padding absorbs the rest
    out += base[tail:]                # footers, byte-for-byte, same place
    assert len(out) == len(base), 'image size changed'
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('base', help='stock RISCOS.IMG')
    ap.add_argument('-m', '--module', action='append', default=[],
                    help='module file to append (repeatable; order = '
                         'initialisation order)')
    ap.add_argument('-o', '--output', help='output image (default: stdout)')
    ap.add_argument('--list', action='store_true',
                    help='list the module chain and exit')
    args = ap.parse_args()

    base = open(args.base, 'rb').read()
    _flags, image_size = read_header(base)
    titles, term = find_chain(base)
    tail = find_tail(base)

    if args.list:
        for i, t in enumerate(titles):
            print(f'{i:3d} {t}')
        print(f'{len(titles)} modules, chain ends {term:#x}, '
              f'footers at {tail:#x}, slack {tail - term - 4} bytes')
        return

    if not args.module:
        ap.error('no modules given (use -m)')

    out = splice(base, args.module)
    new_titles, _ = find_chain(out)
    old_n = len(titles)
    print(f'mkrom: chain {old_n} -> {len(new_titles)} modules '
          f'({", ".join(new_titles[old_n:])})')
    if out[tail:] != base[tail:]:
        raise SystemExit('internal error: ROM tail changed')
    h = hashlib.sha256(out).hexdigest()[:16]
    print(f'mkrom: image size unchanged at {len(out):#x}, sha256 {h}')

    if args.output:
        open(args.output, 'wb').write(out)
        print(f'mkrom: wrote {args.output}')
    else:
        sys.stdout.buffer.write(out)


if __name__ == '__main__':
    main()
