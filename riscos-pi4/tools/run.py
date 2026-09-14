#!/usr/bin/env python3
"""The canonical launch, wrapped: snapshots as a one-word option.

    run.py                     boot from scratch (writes land in a qcow2
                               overlay, never on the card image)
    run.py --snapshot desktop  cold-start from that snapshot (<1s)
    run.py --save desktop      boot, save machine + disc state under that
                               name once the desktop is up, keep running
    run.py --hostfs DIR --boot hostfs
                               boot the share's own !Boot instead of the
                               card's; the card stays on, as SDFS::0

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

import rom

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
    # Two ways to reach a network, and never both.  With --hostnet the
    # guest has no stack to drive a NIC with, so attaching one would be an
    # emulated card nothing ever opens; without it, slirp and usb-net as
    # before.  The doorbell is off unless asked for, so a machine that has
    # not been switched over is untouched.
    net = (["-global", "hostnet.sockets=on"] if args.hostnet else
           ["-netdev", "user,id=n0,domainname=lan",
            # domainname: RISC OS asks for option 15 in its parameter
            # list.  It does not fix DHCP, but it is one less thing the
            # guest asked for and did not get.
            "-device", "usb-net,netdev=n0,rndis=off,bus=usb-bus.0,port=1.3"])

    argv = [
        args.qemu,
        "-M", "raspi4b",
        "-cpu", "cortex-a72,aarch64=off",
        "-kernel", args.kernel,
        "-device", f"loader,file={args.cmos},addr=0x510000,force-raw=on",
        "-device", "usb-hub,bus=usb-bus.0,port=1",
        "-device", "usb-kbd,bus=usb-bus.0,port=1.1",
        "-device", "usb-tablet,bus=usb-bus.0,port=1.2",
        # After the hub: a device on port 1.3 needs the hub to exist
        # first, or QEMU stops with "usb port 1.3 not found".
        *net,
        # Sound needs both halves: a backend, and the vchiq peer told to
        # use it. Without them the guest's sound loop still turns and you
        # simply hear nothing, which reads as sound being unimplemented.
        "-audiodev", f"{args.audiodev},id=snd0",
        "-global", "bcm2835-vchiq.audiodev=snd0",
        "-display", "dx11" + (",backdrop=" + args.backdrop
                              if args.backdrop else ""),
        "-serial", "null",
        "-qmp", "tcp:127.0.0.1:4461,server,nowait",
    ]
    # No card at all is a real configuration now, not a broken one: a share
    # can hold !Boot and the DDE and be the whole machine.
    if overlay:
        argv += ["-drive", f"file={overlay},if=sd,format=qcow2"]
    if args.snapshot:
        argv += ["-loadvm", args.snapshot]
    if args.hostfs:
        argv += ["-global",
                 "bcm2838-peripherals.vmchannel-root=" + args.hostfs]
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


def post_escape():
    """Press Escape in the emulator window: the stock boot's Internet
    startup waits on DHCP until it appears or Escape — without this the
    machine sits on the waiting screen, which is not a desktop."""
    import ctypes
    u32 = ctypes.windll.user32
    hwnd = u32.FindWindowW("qemu-dx11", None)
    if hwnd:
        lp = 1 | (0x01 << 16)          # scan 0x01, one repeat
        u32.PostMessageW(hwnd, 0x0100, 0x1B, lp)
        time.sleep(0.05)
        u32.PostMessageW(hwnd, 0x0101, 0x1B,
                         lp | (1 << 30) | (1 << 31))


def frames_alike(a, b, step=97, tol=8, allow=0.005):
    """Whether two screendumps show the same picture.

    Sampled, and tolerant of a level or two, because a screendump is now
    the composited display rather than the guest's framebuffer: a moving
    backdrop (backdrop=acorn-live) changes almost every pixel slightly on
    every frame without the guest having drawn anything at all, and an
    exact comparison would never call the screen settled."""
    if b is None or len(a) != len(b):
        return False
    n = bad = 0
    for i in range(0, len(a), step):
        n += 1
        if abs(a[i] - b[i]) > tol:
            bad += 1
    return bool(n) and bad <= allow * n


def wait_for_desktop(qemu_path, deadline_s=300):
    """The real desktop, not the first lit pixel: the window's log must
    show a pipeline heartbeat for the desktop mode (800x600 32bpp), and
    the frame must then be stable for two seconds (the DHCP wait screen
    is also 800x600x32, but it does not go quiet on its own)."""
    builddir = os.path.dirname(os.path.abspath(qemu_path))
    poll = os.path.join(builddir, "run-poll.ppm")
    log = os.path.join(builddir, "dx11-debug.txt")
    deadline = time.time() + deadline_s
    escaped = False
    prev = None
    stable_since = None

    while time.time() < deadline:
        try:
            qmp_call("screendump run-poll.ppm", timeout=5)
            body = open(poll, "rb").read().split(b"\n", 3)[3]
        except (OSError, RuntimeError):
            time.sleep(0.5)
            continue

        # Brighter than the window's own clear colour (13,13,20), which
        # is what the composited dump shows before the first guest frame.
        if max(body[:48000]) > 40:
            if not escaped:
                escaped = True
                post_escape()          # skip the DHCP wait, once
        else:
            prev = None
            stable_since = None
            time.sleep(0.5)
            continue

        if frames_alike(body, prev):
            if stable_since is None:
                stable_since = time.time()
            if time.time() - stable_since >= 2.0:
                try:
                    hb = open(log, "r", errors="replace").read()
                    if "800x600 bpp 32" in hb:
                        return True
                except OSError:
                    pass
        else:
            prev = body
            stable_since = None
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
    ap.add_argument("--audiodev", default="dsound",
                    help="host audio driver for the guest's sound: dsound "
                         "(default) on Windows, coreaudio on macOS, "
                         "wav,path=FILE to capture it, none for silence")
    ap.add_argument("--qemu", default=QEMU)
    ap.add_argument("--qemu-img", default=QEMU_IMG)
    ap.add_argument("--kernel", default=KERNEL)
    ap.add_argument("--cmos", default=CMOS)
    ap.add_argument("--image", default=IMAGE)
    ap.add_argument("--hostfs", metavar="DIR",
                    help="serve this host directory as HostFS:$ through "
                         "the doorbell device; a share also brings HostFS "
                         "and its icon-bar filer into the ROM")
    ap.add_argument("--boot", choices=("hostfs",), metavar="SOURCE",
                    help="'hostfs' boots the share's own !Boot instead of "
                         "the card's; the card stays attached as SDFS::0")
    ap.add_argument("--modules", nargs="*", default=[], metavar="FILE",
                    help="further modules spliced into the ROM before "
                         "boot, in initialisation order")
    ap.add_argument("--hostnet", action="store_true",
                    help="the guest's sockets are served by the host: splice "
                         "HostNet, unplug the ROM's own networking, and "
                         "attach no emulated NIC (ROS_PRIVATE "
                         "design/HOSTNET.md)")
    ap.add_argument("--backdrop", metavar="SCENE",
                    help="a layer the host draws beneath the desktop, "
                         "showing through the pixels the guest tags "
                         "'below': acorn, acorn-live, tile:FILE, "
                         "picture:FILE, or none.  Off unless given")
    ap.add_argument("--no-card", action="store_true",
                    help="attach no SD card at all: the share is the whole "
                         "machine (needs --hostfs, and --boot hostfs to "
                         "have anything to boot)")
    args = ap.parse_args()

    # A snapshot restores RAM, and the ROM lives in RAM: -kernel is loaded
    # and then overwritten by the restored image, so what is in ROM was
    # decided when the snapshot was taken, not here.
    if args.snapshot:
        if args.boot:
            raise SystemExit("--boot chooses what to boot; --snapshot skips "
                             "the boot entirely. Use one or the other.")
        if args.hostfs or args.modules:
            print("note: --snapshot restores the ROM the snapshot was taken "
                  "with, so a machine saved without HostFS in ROM comes back "
                  "without it. Re-save from a cold boot to change that.",
                  flush=True)

    # Which ROM and CMOS this launch boots: the same rules, and the same
    # two builders, that run-macos.sh gets from rom.zsh.
    try:
        args.kernel = rom.rom_to_boot(args.kernel, args.modules,
                                      hostfs=args.hostfs,
                                      hostnet=args.hostnet)
        args.cmos = rom.cmos_to_boot(args.cmos, args.boot,
                                     hostfs=args.hostfs,
                                     hostnet=args.hostnet)
    except rom.RomError as exc:
        raise SystemExit(str(exc))

    if args.no_card:
        if not args.hostfs:
            raise SystemExit("--no-card leaves nothing to run: give "
                             "--hostfs a share to be the machine")
        overlay = None
    else:
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
