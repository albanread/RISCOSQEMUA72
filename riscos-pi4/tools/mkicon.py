#!/usr/bin/env python3
"""
mkicon.py -- draw the app icon (riscos-pi4/app/AppIcon.icns).

    mkicon.py

The icon is a cog on a rounded-square tile, drawn entirely from
geometry in this file: no imported artwork, no third-party material,
safe to ship in the public app.  The cog nods to RISC OS's own cog
branding without copying it -- our tooth geometry, our colours
(silver cog on a deep navy gradient).

Rendered at 1024 with 4x4 supersampling, halved down to every size an
.iconset wants, packed with iconutil.  PNG encoding is self-contained;
no PIL dependency.
"""

import math
import struct
import subprocess
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
OUT_ICNS = ROOT / "riscos-pi4" / "app" / "AppIcon.icns"

# Tile
CORNER = 0.224                 # rounded-square radius / side
TILE_TOP = (48, 63, 112)       # navy gradient
TILE_BOT = (23, 32, 66)
RIM = 0.80                     # hairline border darkening

# Cog (all radii in units of the tile side)
R_OUT = 0.355                  # tooth tip radius
R_ROOT = 0.272                 # root radius between teeth
R_HOLE = 0.125                 # centre bore
TEETH = 8
TOOTH_TOP = 0.19               # tooth half-width at the tip, as a
TOOTH_ROOT = 0.37              #   fraction of one tooth pitch (0..0.5)
COG = (234, 228, 217)          # warm silver


def clamp(x):
    return 0.0 if x < 0 else (1.0 if x > 1 else x)


def smooth(e0, e1, x):
    t = clamp((x - e0) / (e1 - e0))
    return t * t * (3 - 2 * t)


def render(S):
    """The composed tile as S x S RGBA."""
    px = bytearray(S * S * 4)
    rad = CORNER * S
    bor = max(1, S // 256)
    ss = 4
    offs = [(i + 0.5) / ss for i in range(ss)]
    half = S / 2.0

    for y in range(S):
        for x in range(S):
            r_acc = g_acc = b_acc = a_acc = 0.0
            for oy in offs:
                for ox in offs:
                    sx, sy = x + ox, y + oy
                    # rounded-square coverage test
                    dx = max(rad - sx, sx - (S - rad), 0)
                    dy = max(rad - sy, sy - (S - rad), 0)
                    if dx * dx + dy * dy > rad * rad:
                        continue
                    cx, cy = sx - half, sy - half
                    r = math.hypot(cx, cy) / S
                    # tooth pitch phase: d in [0, 0.5], 0 = tooth centre
                    p = math.atan2(cy, cx) / (2 * math.pi) * TEETH
                    d = abs(p - round(p))
                    flank = smooth(TOOTH_TOP, TOOTH_ROOT, d)
                    edge = R_ROOT + (R_OUT - R_ROOT) * (1 - flank)
                    if R_HOLE < r < edge:
                        # cog surface: top light, bore and rim occlusion,
                        # tooth flanks turned slightly away from the light
                        ny = cy / (r * S)
                        l = 0.78 + 0.11 * (1 - ny)
                        if r < R_HOLE * 1.45:
                            l *= 0.70 + 0.30 * smooth(R_HOLE, R_HOLE * 1.45, r)
                        t = (r - (R_OUT - 0.032)) / 0.032
                        if t > 0:
                            l *= 1 - 0.16 * clamp(t)
                        l *= 1 - 0.18 * flank
                        cr = COG[0] * l
                        cg = COG[1] * l
                        cb = COG[2] * l
                    else:
                        t = sy / S
                        cr = TILE_TOP[0] + (TILE_BOT[0] - TILE_TOP[0]) * t
                        cg = TILE_TOP[1] + (TILE_BOT[1] - TILE_TOP[1]) * t
                        cb = TILE_TOP[2] + (TILE_BOT[2] - TILE_TOP[2]) * t
                        # soft drop shadow: the cog silhouette, dilated
                        # a touch, offset down, feathered
                        cy2 = cy + 0.010 * S
                        r2 = math.hypot(cx, cy2) / S
                        p2 = math.atan2(cy2, cx) / (2 * math.pi) * TEETH
                        d2 = abs(p2 - round(p2))
                        e2 = (R_ROOT + (R_OUT - R_ROOT)
                              * (1 - smooth(TOOTH_TOP, TOOTH_ROOT, d2)) + 0.006)
                        sh = 1 - smooth(e2 - 0.014, e2 + 0.014, r2)
                        cr *= 1 - 0.32 * sh
                        cg *= 1 - 0.32 * sh
                        cb *= 1 - 0.32 * sh
                    r_acc += cr
                    g_acc += cg
                    b_acc += cb
                    a_acc += 255
            n = ss * ss
            o = (y * S + x) * 4
            cov = a_acc / n
            if cov <= 0:
                continue
            mul = 1.0
            if cov >= 254 and (x < bor or y < bor
                               or x >= S - bor or y >= S - bor):
                mul = RIM
            px[o] = min(255, int(r_acc / n * mul))
            px[o + 1] = min(255, int(g_acc / n * mul))
            px[o + 2] = min(255, int(b_acc / n * mul))
            px[o + 3] = int(cov)
    return bytes(px)


def halve(src, S):
    out = bytearray((S // 2) * (S // 2) * 4)
    for y in range(S // 2):
        for x in range(S // 2):
            o = (y * (S // 2) + x) * 4
            acc = [0, 0, 0, 0]
            for dy in (0, 1):
                for dx in (0, 1):
                    so = ((y * 2 + dy) * S + x * 2 + dx) * 4
                    for i in range(4):
                        acc[i] += src[so + i]
            out[o:o + 4] = bytes(c // 4 for c in acc)
    return bytes(out)


def png_write(path, w, h, rgba):
    rows = bytearray()
    stride = w * 4
    for y in range(h):
        rows.append(0)
        rows += rgba[y * stride:(y + 1) * stride]

    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
           + chunk(b"IEND", b""))
    Path(path).write_bytes(png)


def main():
    S = 1024
    tile = render(S)
    sizes = {}
    while S >= 16:
        sizes[S] = tile
        tile = halve(tile, S)
        S //= 2

    iconset = ROOT / "build-macos" / "AppIcon.iconset"
    iconset.mkdir(parents=True, exist_ok=True)
    for f in iconset.iterdir():
        f.unlink()
    for size, name in [(16, "icon_16x16.png"), (32, "icon_16x16@2x.png"),
                       (32, "icon_32x32.png"), (64, "icon_32x32@2x.png"),
                       (128, "icon_128x128.png"), (256, "icon_128x128@2x.png"),
                       (256, "icon_256x256.png"), (512, "icon_256x256@2x.png"),
                       (512, "icon_512x512.png"), (1024, "icon_512x512@2x.png")]:
        png_write(iconset / name, size, size, sizes[size])
    # previews for eyeballing, not part of the iconset
    png_write(ROOT / "build-macos" / "icon-preview-256.png", 256, 256, sizes[256])
    png_write(ROOT / "build-macos" / "icon-preview-32.png", 32, 32, sizes[32])

    subprocess.run(["iconutil", "-c", "icns", str(iconset),
                    "-o", str(OUT_ICNS)], check=True)
    print(f"written {OUT_ICNS} ({OUT_ICNS.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
