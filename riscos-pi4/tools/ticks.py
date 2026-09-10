#!/usr/bin/env python3
"""Measure the guest's centisecond ticker: rate and worst gap.

RISC OS is interrupt-driven, so what the clock is worth is not the rate
the host can nominally hit but the rate the guest actually sees.  This
boots the machine headless and counts the system timer's compare-1
expiries -- the interrupt the 100 Hz ticker waits on -- reporting the
rate and the gap distribution over a settled desktop.

    ticks.py [settle-seconds] [measure-seconds]

The Windows front end's figure, from riscos-pi4/README.md, is 100.0 a
second with a worst gap of 11.6 ms; riscos-pi4/MACOS.md has the macOS
number and what was decided from it.

    RISCOS_IMAGES  where RISCOS.IMG, cmos.bin and card.img live
    QEMU_BIN       the built emulator
"""
import os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
IMAGES = os.environ.get("RISCOS_IMAGES", os.path.join(ROOT, "riscos-images"))
Q = os.environ.get("QEMU_BIN",
                   os.path.join(ROOT, "build-macos", "qemu-system-aarch64"))
SETTLE = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
WINDOW = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0

cmd = [Q, "-M", "raspi4b", "-cpu", "cortex-a72,aarch64=off",
       "-kernel", os.path.join(IMAGES, "RISCOS.IMG"),
       "-device", "loader,file=%s,addr=0x510000,force-raw=on" % os.path.join(IMAGES, "cmos.bin"),
       "-drive", "file=%s,if=sd,format=raw,snapshot=on" % os.path.join(IMAGES, "card.img"),
       "-netdev", "user,id=n0",
       "-device", "usb-hub,bus=usb-bus.0,port=1",
       "-device", "usb-kbd,bus=usb-bus.0,port=1.1",
       "-device", "usb-tablet,bus=usb-bus.0,port=1.2",
       "-device", "usb-net,netdev=n0,rndis=off,bus=usb-bus.0,port=1.3",
       "-global", "bcm2835-vsyncgen.hz=30",
       "-display", "none",
       "-trace", "enable=bcm2835_systmr_timer_expired"]

p = subprocess.Popen(cmd, stderr=subprocess.PIPE, stdout=subprocess.DEVNULL,
                     bufsize=1, text=True)
pat = re.compile(r"timer #(\d+) expired")
t0 = time.monotonic()
stamps = {}
try:
    for line in p.stderr:
        mo = pat.search(line)
        if not mo:
            continue
        now = time.monotonic()
        if now - t0 < SETTLE:
            continue
        if now - t0 > SETTLE + WINDOW:
            break
        stamps.setdefault(int(mo.group(1)), []).append(now)
finally:
    p.terminate()
    try:
        p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()

print("settled %.0fs, measured %.0fs\n" % (SETTLE, WINDOW))
for tid in sorted(stamps):
    ts = stamps[tid]
    if len(ts) < 3:
        print("timer #%d: %d expiries" % (tid, len(ts)))
        continue
    span = ts[-1] - ts[0]
    gaps = [(b - a) * 1000.0 for a, b in zip(ts, ts[1:])]
    gaps.sort()
    print("timer #%d: %6.1f/s over %.1fs   "
          "gap median %.2f ms, p99 %.2f ms, worst %.2f ms"
          % (tid, (len(ts) - 1) / span, span,
             gaps[len(gaps) // 2], gaps[int(len(gaps) * 0.99)], gaps[-1]))
