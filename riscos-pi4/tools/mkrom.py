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
import re
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


# The self-relocation code a Norcroft build carries when it has absolute
# relocations (link -rmf with cmhg's header or the C stubs): at
# initialisation it walks its table and patches its own image in place.
# RMLoaded that is fine; from ROM the first store is a data abort before
# the module has done anything, and a headless machine then sits at the
# supervisor prompt looking hung (HostFS 1.01 in a ROM: "Abort on data
# transfer at &FC4BFC84", 17 Sep 2026).  The preamble is fixed enough to
# recognise:
#     SUB   R11, PC, #16          E24FB010
#     ADD   R0, PC, #table        E28F0xxx
#     SUBS  R1, R11, R1           E05B1001
#     MOVEQ PC, LR                01A0F00E
RELOC_SUB_R11 = b'\x10\xb0\x4f\xe2'
RELOC_SUBS_MOVEQ = b'\x01\x10\x5b\xe0\x0e\xf0\xa0\x01'


def self_relocates(body):
    """Offset of the module's self-relocation preamble, or None."""
    at = body.find(RELOC_SUB_R11)
    while at >= 0:
        if at % 4 == 0 and body[at + 8:at + 16] == RELOC_SUBS_MOVEQ:
            return at
        at = body.find(RELOC_SUB_R11, at + 1)
    return None


def find_resource(data, name):
    """Locate a ResourceFS file block in the image by its full name, e.g.
    'Resources.BootFX.1920x1080'.  A block is a 5-word header (offset to
    the next block, load, exec, length, attributes), the zero-terminated
    name padded to a word, a size word (length + 4) and the data.  Returns
    (header offset, data offset, length, block size, load address).
    """
    needle = name.encode('latin-1') + b'\0'
    i = -1
    while True:
        i = data.find(needle, i + 1)
        if i < 0:
            raise SystemExit(f'mkrom: resource {name} not in the image')
        hdr = i - 20
        if hdr < 0:
            continue
        nxt, load, _exec, length, _attr = struct.unpack_from('<5I', data, hdr)
        d = ((i + len(needle) + 3) & ~3) + 4
        if not (24 < nxt < 0x1000000 and d + length <= hdr + nxt + 4):
            continue
        if struct.unpack_from('<I', data, d - 4)[0] != length + 4:
            continue
        return hdr, d, length, nxt, load


def replace_resources(image, pairs):
    """Overwrite ResourceFS files in place: NAME=FILE pairs.  The new file
    must fit the block (up to the room the block has, not just the old
    length); the length word and the size word are updated and the rest
    of the block zeroed.  A FILE named with a RISC OS type suffix (',c85')
    also sets the block's file type.  Nothing moves, so every other offset
    in the image stays where it was.
    """
    out = bytearray(image)
    for name, path in pairs:
        hdr, d, length, nxt, load = find_resource(out, name)
        room = hdr + nxt - d
        new = open(path, 'rb').read()
        if len(new) > room:
            raise SystemExit(f'mkrom: {path} is {len(new)} bytes but '
                             f'{name} has room for {room}')
        note = ''
        m = re.search(r',([0-9a-fA-F]{3})$', os.path.basename(path))
        if m:
            t = int(m.group(1), 16)
            load = (load & 0xFFF000FF) | (t << 8)     # 0xFFFtttdd
            struct.pack_into('<I', out, hdr + 4, load)
            note = f', type &{t:03X}'
        struct.pack_into('<I', out, hdr + 12, len(new))
        struct.pack_into('<I', out, d - 4, len(new) + 4)
        out[d:d + room] = new + b'\0' * (room - len(new))
        print(f'mkrom: {name} <- {path} ({len(new)} of {room} bytes{note})')
    return bytes(out)


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
        at = self_relocates(body)
        if at is not None:
            raise SystemExit(
                f'{p}: {title} patches its own image at initialisation '
                f'(self-relocation code at +{at:#x}), which from ROM is a '
                f'data abort at the first store.  RMLoad it instead, or '
                f'build it without absolute relocations -- no cmhg, no '
                f'stubs, as hostfs/dde/Build,feb does')
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
    ap.add_argument('-r', '--resource', action='append', default=[],
                    metavar='NAME=FILE',
                    help='replace a ResourceFS file in place, e.g. '
                         'Resources.BootFX.1920x1080=splash,c85 (repeatable)')
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

    if not args.module and not args.resource:
        ap.error('nothing to do (use -m and/or -r)')

    pairs = []
    for spec in args.resource:
        if '=' not in spec:
            ap.error(f'-r wants NAME=FILE, not {spec!r}')
        pairs.append(tuple(spec.split('=', 1)))

    out = splice(base, args.module) if args.module else base
    if args.module:
        new_titles, _ = find_chain(out)
        old_n = len(titles)
        print(f'mkrom: chain {old_n} -> {len(new_titles)} modules '
              f'({", ".join(new_titles[old_n:])})')
    if pairs:
        out = replace_resources(out, pairs)
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
