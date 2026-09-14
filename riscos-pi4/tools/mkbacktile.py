#!/usr/bin/env python3
"""mkbacktile.py -- the sprite that hands the desktop's background to the host.

The Metal display's backdrop layer (ui/metal.m, MACOS.md "The backdrop
layer") shows through every guest pixel that is tagged "below" and is
the backdrop key colour.  The tag lives in the transfer byte of a 32bpp
pixel -- RISC OS's supremacy byte, which everything the OS draws leaves
at 0:

    bits 7-6  layer tag: 10 below, 01 above (reserved), 00/11 nothing
    bits 5-0  reserved, 0

This writes a small 32bpp sprite whose every pixel is the key colour,
#B7C0B4 (the Acorn sage), with the transfer byte 0x80.  Tiled as the
pinboard backdrop it paints the tag across the whole background:

    Backdrop -Tile Boot:Resources.!ThemeDefs.Themes.Acorn.BackTile

The kernel's sprite plot and the Wimp's block copies keep all 32 bits,
so the tag survives redraws and window moves (verified 14 Sep 2026).
With no backdrop layer -- another front end, or backdrop=off -- the
tile is simply the sage ground.  Keep KEY and TAG in step with
METAL_BACKDROP_KEY and the decode shader in ui/metal.m.

    mkbacktile.py --out BackTile,ff9 [--size 32]
"""
import argparse
import struct

KEY = (0xB7, 0xC0, 0xB4)      # R, G, B: the Acorn theme's sage
TAG_BELOW = 0x80              # transfer byte: layer tag 10, reserved bits 0


def tile(size):
    r, g, b = KEY
    word = struct.pack('<I', r | (g << 8) | (b << 16) | (TAG_BELOW << 24))
    body = word * (size * size)
    hdr = bytearray(44)
    struct.pack_into('<i', hdr, 0, 44 + len(body))          # to the next sprite
    struct.pack_into('<12s', hdr, 4, b'backtile')
    struct.pack_into('<7i', hdr, 16,
                     size - 1,                              # width in words - 1
                     size - 1,                              # height - 1
                     0, 31,                                 # first and last bit used
                     44, 44,                                # image; mask = image: none
                     1 | (90 << 1) | (90 << 14) | (6 << 27))  # 32bpp, 90 dpi
    sprite = bytes(hdr) + body
    return struct.pack('<III', 1, 16, 16 + len(sprite)) + sprite


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', required=True, help='the sprite file to write (,ff9)')
    ap.add_argument('--size', type=int, default=32, help='tile side in pixels (default 32)')
    a = ap.parse_args()
    data = tile(a.size)
    open(a.out, 'wb').write(data)
    print(f'mkbacktile: {a.out}: {a.size}x{a.size} 32bpp, #{KEY[0]:02X}{KEY[1]:02X}{KEY[2]:02X}, '
          f'transfer byte &{TAG_BELOW:02X}, {len(data)} bytes')


if __name__ == '__main__':
    main()
