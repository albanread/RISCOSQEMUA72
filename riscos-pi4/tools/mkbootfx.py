#!/usr/bin/env python3
"""mkbootfx.py -- the boot screen: the three BootFX resources that
make-release.sh splices into the ROM in place of ROOL's Raspberry Pi set
(mkrom.py -r).  Reads the masters in riscos-pi4/app/bootfx/ and writes,
beside them:

    1920x1080,c85   the splash, from splash.svg -- rendered with
                    rsvg-convert, made a baseline JPEG by sips, and
                    stripped of the APP1 (EXIF) segment sips adds
    Logo,c85        the logo BootFX centres on the black text screen
                    while the ROM initialises, from logo.svg (164x164)
    Bar24,fca       the progress bar: three flat 640x40 32bpp sprites
                    named border, fill and bar (BootFX/Docs/Spec.txt),
                    Squash-compressed

Each must fit the ROM block it replaces -- 116,220, 8,141 and 23,004
bytes -- so the sizes are checked here, not discovered at splice time.

A Squash file is a 20-byte header (SQSH, unsquashed length, load, exec,
0) in front of a Unix compress stream, 12-bit codes, block mode: the
ROM's own Bar24 decompresses with `compress -d` once the header is off,
so `compress -b 12` makes ours.
"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.normpath(os.path.join(HERE, '..', 'app', 'bootfx'))
LIMITS = {'1920x1080,c85': 116220, 'Logo,c85': 8141, 'Bar24,fca': 23004}

INK, BODY, GREEN = (0x23, 0x26, 0x2A), (0xD9, 0xD6, 0xCC), (0x3C, 0x6E, 0x47)
BAR_W, BAR_H, RING = 640, 40, 2


def run(*cmd):
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode:
        sys.exit(f'mkbootfx: {cmd[0]} failed: {r.stderr.decode(errors="replace").strip()}')
    return r.stdout


def jpeg(svg, w, h, quality, out):
    """svg -> png (rsvg-convert) -> baseline JPEG (sips), APPn stripped."""
    png = out + '.png'
    run('rsvg-convert', '-w', str(w), '-h', str(h), svg, '-o', png)
    run('sips', '-s', 'format', 'jpeg', '-s', 'formatOptions', str(quality), png, '--out', out)
    os.remove(png)
    data = open(out, 'rb').read()
    # keep SOI, APP0 (JFIF) and everything from DQT on; drop the APP1..15
    # segments (EXIF, ICC) that a decoder in a ROM has no business parsing
    i, kept = 2, bytearray(data[:2])
    while i < len(data):
        marker = data[i + 1]
        if marker == 0xDA:                      # start of scan: the rest is image
            kept += data[i:]
            break
        seg = 2 + struct.unpack('>H', data[i + 2:i + 4])[0]
        if not (0xE1 <= marker <= 0xEF):
            kept += data[i:i + seg]
        i += seg
    if b'\xff\xc0' not in kept:
        sys.exit(f'mkbootfx: {out} is not a baseline JPEG (SpriteExtend needs SOF0)')
    open(out, 'wb').write(kept)
    return len(kept)


def sprite(name, w, h, pixel):
    """One 32bpp 90dpi sprite with a 1bpp mask; pixel(x, y) -> (r,g,b) or None."""
    hdr = bytearray(44)
    struct.pack_into('<12s', hdr, 4, name.encode('latin-1'))
    image, maskrows = bytearray(), bytearray()
    mrow = (w + 31) // 32 * 4
    for y in range(h):
        mask = bytearray(mrow)
        for x in range(w):
            p = pixel(x, y)
            if p is None:
                image += b'\0\0\0\0'
            else:
                r, g, b = p
                image += struct.pack('<I', r | (g << 8) | (b << 16))
                mask[x >> 3] |= 1 << (x & 7)
        maskrows += mask
    total = 44 + len(image) + len(maskrows)
    struct.pack_into('<i', hdr, 0, total)
    struct.pack_into('<i', hdr, 16, w - 1)          # width in words - 1 (32bpp: one word per pixel)
    struct.pack_into('<i', hdr, 20, h - 1)
    struct.pack_into('<i', hdr, 24, 0)
    struct.pack_into('<i', hdr, 28, 31)
    struct.pack_into('<i', hdr, 32, 44)
    struct.pack_into('<i', hdr, 36, 44 + len(image))
    struct.pack_into('<i', hdr, 40, 1 | (90 << 1) | (90 << 14) | (6 << 27))   # type 6, 90x90 dpi
    return bytes(hdr) + bytes(image) + bytes(maskrows)


def sprite_file(sprites):
    first = 16
    return struct.pack('<III', len(sprites), first, first + sum(map(len, sprites))) + b''.join(sprites)


def squash(data, load=0xFFFFF900, exe=0):
    stream = run('compress', '-b', '12', '-c', '-', input=data) if False else \
        subprocess.run(['compress', '-b', '12', '-c'], input=data, capture_output=True, check=True).stdout
    return struct.pack('<4sIIII', b'SQSH', len(data), load, exe, 0) + stream


def bar():
    inside = lambda x, y: RING <= x < BAR_W - RING and RING <= y < BAR_H - RING
    border = sprite('border', BAR_W, BAR_H, lambda x, y: None if inside(x, y) else INK)
    fill = sprite('fill', BAR_W, BAR_H, lambda x, y: BODY if inside(x, y) else None)
    full = sprite('bar', BAR_W, BAR_H, lambda x, y: GREEN if inside(x, y) else None)
    return squash(sprite_file([border, fill, full]))


def main():
    os.chdir(ART)
    made = {
        '1920x1080,c85': jpeg('splash.svg', 1920, 1080, 82, '1920x1080,c85'),
        'Logo,c85': jpeg('logo.svg', 164, 164, 85, 'Logo,c85'),
    }
    data = bar()
    open('Bar24,fca', 'wb').write(data)
    made['Bar24,fca'] = len(data)
    bad = False
    for name, size in made.items():
        limit = LIMITS[name]
        print(f'{name:16} {size:7d} bytes  (room in the ROM: {limit})'
              + ('' if size <= limit else '  TOO BIG'))
        bad |= size > limit
    if bad:
        sys.exit(1)


if __name__ == '__main__':
    main()
