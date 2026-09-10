#!/usr/bin/env python3
"""The canonical launch, wrapped: snapshots as a one-word option.

    run.py                     boot from scratch (writes land in a qcow2
                               overlay, never on the card image)
    run.py --snapshot desktop  cold-start from that snapshot (<1s)
    run.py --save desktop      boot, save machine + disc state under that
                               name once the desktop is up, keep running

The overlay (<image>-overlay.qcow2) is created on first use beside the
card image, so a snapshot carries both the machine and the disc.  The
window's system menu gains "Load snapshot", which rewinds to the
snapshot named "desktop" however the emulator was started.

Paths assume the development tree layout: QEMU build at <repo>/build,
ROM and card image under F:/RISCOSDEV/roms.  QMP listens on 4461;
everything is overridable with --qemu, --kernel, --cmos, --image.
"""

import argparse
import json
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
QEMU = r"F:\RISCOSDEV\qemu\build\qemu-system-aarch64.exe"
QEMU_IMG = r"F:\RISCOSDEV\qemu\build\qemu-img.exe"
KERNEL = r"F:\RISCOSDEV\roms\pi\v530\RISCOS.IMG"
CMOS = r"F:\RISCOSDEV\roms\pi\rool-cmos-unplug.bin"
IMAGE = r"F:\RISCOSDEV\roms\sdimg\ro530-1875M.img"
# The build's executables link the MinGW runtime DLLs.
MINGW_BIN = r"F:\RISCOSDEV\msys64\mingw64\bin"


def child_env():
    env = os.environ.copy()
    if os.path.isdir(MINGW_BIN):
        env["PATH"] = MINGW_BIN + os.pathsep + env.get("PATH", "")
    return env


def overlay_for(image):
    root, ext = os.path.splitext(image)
    return root + "-overlay.qcow2"


def ensure_overlay(qemu_img, image, overlay):
    if os.path.exists(overlay):
        return
    print(f"creating overlay {overlay} over {image}", flush=True)
    subprocess.run([qemu_img, "create", "-f", "qcow2", "-F", "raw",
                    "-b", image, overlay], check=True, env=child_env())


def command_line(args, overlay):
    argv = [
        args.qemu,
        "-M", "raspi4b",
        "-cpu", "cortex-a72,aarch64=off",
        "-kernel", args.kernel,
        "-device", f"loader,file={args.cmos},addr=0x510000,force-raw=on",
        "-drive", f"file={overlay},if=sd,format=qcow2",
        "-netdev", "user,id=n0",
        "-device", "usb-hub,bus=usb-bus.0,port=1",
        "-device", "usb-kbd,bus=usb-bus.0,port=1.1",
        "-device", "usb-tablet,bus=usb-bus.0,port=1.2",
        "-device", "usb-net,netdev=n0,rndis=off,bus=usb-bus.0,port=1.3",
        "-display", "dx11",
        "-serial", "null",
        "-qmp", "tcp:127.0.0.1:4461,server,nowait",
    ]
    if args.snapshot:
        argv += ["-loadvm", args.snapshot]
    return argv


def qmp_call(cmd, timeout=30):
    """One HMP command over QMP; returns its reply as text."""
    s = socket.create_connection(("127.0.0.1", 4461), timeout=timeout)
    f = s.makefile("rb")
    f.readline()
    s.sendall(b'{"execute":"qmp_capabilities"}\n')
    f.readline()
    s.sendall(json.dumps({"execute": "human-monitor-command",
                          "arguments": {"command-line": cmd}}).encode() + b"\n")
    while True:
        s.settimeout(timeout)
        line = f.readline()
        if not line:
            s.close()
            raise RuntimeError("QMP closed")
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        if "return" in msg:
            s.close()
            return msg["return"]
        if "error" in msg:
            s.close()
            raise RuntimeError(msg["error"]["desc"])


def wait_for_desktop(qemu_path, deadline_s=300):
    """Poll screendumps until the desktop is painted."""
    poll = os.path.join(os.path.dirname(os.path.abspath(qemu_path)),
                        "run-poll.ppm")
    deadline = time.time() + deadline_s
    while time.time() < deadline:
        try:
            qmp_call("screendump run-poll.ppm", timeout=5)
            body = open(poll, "rb").read()
            # any image content at all: boot messages count, then desktop
            if any(body.split(b"\n", 3)[3][:48000]):
                return True
        except (OSError, RuntimeError):
            pass
        time.sleep(0.5)
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--snapshot", metavar="NAME",
                    help="cold-start from this snapshot")
    ap.add_argument("--save", metavar="NAME",
                    help="save machine + disc state under this name once "
                         "the desktop is up, then keep running")
    ap.add_argument("--qemu", default=QEMU)
    ap.add_argument("--qemu-img", default=QEMU_IMG)
    ap.add_argument("--kernel", default=KERNEL)
    ap.add_argument("--cmos", default=CMOS)
    ap.add_argument("--image", default=IMAGE)
    args = ap.parse_args()

    overlay = overlay_for(args.image)
    ensure_overlay(args.qemu_img, args.image, overlay)

    argv = command_line(args, overlay)
    print(" ".join(argv), flush=True)
    # The emulator writes its debug log, screendumps and window
    # screenshots to its CWD: keep that the build tree, as everywhere
    # else in the tooling.
    proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL,
                            cwd=os.path.dirname(os.path.abspath(args.qemu)),
                            env=child_env())

    if args.save:
        if wait_for_desktop(args.qemu):
            qmp_call(f"savevm {args.save}")
            print(f"saved snapshot '{args.save}'; emulator keeps running",
                  flush=True)
        else:
            print("no desktop appeared; nothing saved", flush=True)


if __name__ == "__main__":
    sys.exit(main())
