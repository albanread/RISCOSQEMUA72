#!/usr/bin/env python3
"""Verify the dx11 window's framebuffer decoders on synthetic modes.

RISC OS only ever programs 8/16/32 bpp, but the firmware contract covers
1/2/4/24 as well.  The HMP command `synthfb BPP [XRES [YRES]]` (see
hw/display/bcm2835_fb.c) reconfigures the framebuffer to any depth and
fills it with a deterministic pattern:

    buffer byte i  = (i + (i >> 8)) & 0xff
    palette p      = R=p, G=p*3 & 255, B=p*7 & 255   (0x00BBGGRR, LE)

This script freezes the guest (so nothing draws over the pattern), forces
each mode, takes the window's own screenshot (PrintScreen: the decoded
surface), recomputes the expected image with the same rules the HLSL
decoders implement, and compares the two pixel by pixel.

Usage: synthfb-test.py [bpp ...]      (default: 24 4 2 1 8 16 32)

Talks to the canonical launch: QMP on 127.0.0.1:4461, window class
"qemu-dx11".  The window's screenshots land in the emulator's CWD as
dx11-screenshot-NNN.png; the newest one is read each time.
"""

import ctypes
import glob
import json
import os
import socket
import struct
import sys
import time
import zlib

QMP = ("127.0.0.1", 4461)
BUILD = r"F:\RISCOSDEV\qemu\build"
MODE = 640, 480          # synthetic size: not one the desktop uses


def hmp(cmd):
    s = socket.create_connection(QMP, timeout=20)
    f = s.makefile("rb")
    f.readline()
    s.sendall(b'{"execute":"qmp_capabilities"}\n')
    f.readline()
    s.sendall(json.dumps({"execute": "human-monitor-command",
                          "arguments": {"command-line": cmd}}).encode() + b"\n")
    r = json.loads(f.readline())["return"]
    s.close()
    return r


def qmp(cmd):
    s = socket.create_connection(QMP, timeout=20)
    f = s.makefile("rb")
    f.readline()
    s.sendall(b'{"execute":"qmp_capabilities"}\n')
    f.readline()
    s.sendall(json.dumps({"execute": cmd}).encode() + b"\n")
    r = json.loads(f.readline())
    s.close()
    return r


def window_shot():
    """Press PrintScreen in the dx11 window; return the newest PNG."""
    hwnd = ctypes.windll.user32.FindWindowW("qemu-dx11", None)
    if not hwnd:
        raise SystemExit("no dx11 window")
    before = max(glob.glob(os.path.join(BUILD, "dx11-screenshot-*.png")),
                 key=os.path.getmtime, default=None)
    # PrintScreen arrives as WM_KEYUP only; scan 0x54, extended
    lp = (1 << 30) | (1 << 31) | (0x54 << 16) | 0x01000000
    ctypes.windll.user32.PostMessageW(hwnd, 0x0101, 0x2C, lp)
    for _ in range(40):
        time.sleep(0.25)
        newest = max(glob.glob(os.path.join(BUILD, "dx11-screenshot-*.png")),
                     key=os.path.getmtime, default=None)
        if newest != before or (before is None and newest):
            return newest
    raise SystemExit("no screenshot appeared")


def decode_png(path):
    png = open(path, "rb").read()
    pos, idat, w, h, ct = 8, b"", 0, 0, 0
    while pos < len(png):
        ln = struct.unpack(">I", png[pos:pos + 4])[0]
        tag, data = png[pos + 4:pos + 8], png[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if tag == b"IHDR":
            w, h, _, ct = struct.unpack(">IIBB", data[:10])
        elif tag == b"IDAT":
            idat += data
    raw = zlib.decompress(idat)
    bpp = {2: 3, 6: 4}[ct]
    stride = w * bpp
    out = bytearray(w * h * 3)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xff
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xff
        elif f == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xff
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b, c = prev[i], prev[i - bpp] if i >= bpp else 0
                e = a + b - c
                pa, pb, pc = abs(e - a), abs(e - b), abs(e - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xff
        for x in range(w):
            o, s = (y * w + x) * 3, x * bpp
            out[o:o + 3] = line[s:s + 3]
        prev = line
    return w, h, out


def pattern_byte(i):
    return (i + (i >> 8)) & 0xff


def palette(p):
    return p, (p * 3) & 0xff, (p * 7) & 0xff


def expected(xres, yres, bpp):
    """Mirror of the HLSL decoders, see ui/dx11.cpp ps_main()."""
    pitch = (xres * bpp + 7) >> 3
    img = bytearray(xres * yres * 3)
    for y in range(yres):
        for x in range(xres):
            i = y * pitch + (x * bpp >> 3)
            b0 = pattern_byte(i)
            if bpp == 32:
                r, g, b = (pattern_byte(i + 1), pattern_byte(i + 2), b0)  # pixo=1
                # pixo=1 means RGB: bytes are r,g,b in order
                r, g, b = b0, pattern_byte(i + 1), pattern_byte(i + 2)
            elif bpp == 24:
                r, g, b = b0, pattern_byte(i + 1), pattern_byte(i + 2)
            elif bpp == 16:
                w16 = b0 | (pattern_byte(i + 1) << 8)
                r = ((w16 >> 11) & 31) * 255 // 31
                g = ((w16 >> 5) & 63) * 255 // 63
                b = (w16 & 31) * 255 // 31
            else:
                idx = (b0 >> ((x * bpp) & 7)) & ((1 << bpp) - 1)
                r, g, b = palette(idx)
            o = (y * xres + x) * 3
            img[o:o + 3] = r, g, b
    return img


def check(bpp):
    xres, yres = MODE
    hmp(f"synthfb {bpp} {xres} {yres}")
    time.sleep(1.0)               # let the dx11 pipeline rebuild
    shot = window_shot()
    w, h, img = decode_png(shot)
    if (w, h) != (xres, yres):
        print(f"bpp {bpp:2}: FAIL size {w}x{h}, expected {xres}x{yres}")
        return False
    want = expected(xres, yres, bpp)
    bad = sum(1 for i in range(0, len(img), 3)
              if img[i] != want[i] or img[i + 1] != want[i + 1]
              or img[i + 2] != want[i + 2])
    pct = 100.0 * bad / (xres * yres)
    print(f"bpp {bpp:2}: {'PASS' if bad == 0 else 'FAIL'}"
          f" ({bad} of {xres * yres} pixels differ, {pct:.2f}%)")
    return bad == 0


def main():
    depths = [int(a) for a in sys.argv[1:]] or [24, 4, 2, 1, 8, 16, 32]
    print("freezing guest so nothing draws over the pattern")
    qmp("stop")
    try:
        ok = all([check(bpp) for bpp in depths])
    finally:
        qmp("cont")
        # put the desktop back where it was
        hmp("synthfb 32 800 600")
    print("ALL PASS" if ok else "FAILURES PRESENT")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
