#!/usr/bin/env python3
"""Four named RISC OS machines that cannot tread on each other.

    farm.py create              make (or repair) all four instances
    farm.py up alpha            start one
    farm.py up all              start all four
    farm.py status              which are running
    farm.py shot alpha          screendump to its screen/ directory
    farm.py hmp alpha "info registers"
    farm.py down bravo          stop one politely, then firmly
    farm.py reset charlie       throw away its disc writes, keep the share
    farm.py ls                  names and paths

Why a farm rather than four copies of run.py: three things are shared by
default and every one of them is a way for two machines to corrupt each
other's work.

  - **The disc.**  Each instance gets its own qcow2 overlay over the one
    read-only card image, so writes are private and starting a new machine
    costs a few hundred kilobytes rather than two gigabytes.
  - **The HostFS share.**  This is how a compiler gets binaries into the
    guest and results back out, so two machines sharing one host directory
    would overwrite each other's build outputs.  Each gets its own.
  - **The control socket.**  QMP is how anything drives the machine; one
    socket means one machine.  A unix socket in each instance directory,
    not a loopback TCP port: QMP has no authentication, and the guest can
    reach a loopback port -- with HostNet in one Socket_Connect, through
    slirp's 10.0.2.2 even without it (ROS_PRIVATE#34).

Headless by default.  Four windows is not a thing anyone wants, and driving
a window means posting messages at it, which moves the real mouse pointer.
QMP does screendumps and input-send-event perfectly well with no display at
all.  Pass --display dx11 when you actually want to watch one.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time

import devpaths
import qmpunix
import rom

HERE = os.path.dirname(os.path.abspath(__file__))

# Where the emulator and the licensed blobs are: devpaths, so every
# machine sets RISCOS_* variables instead of editing this file.
QEMU = devpaths.QEMU
QEMU_IMG = devpaths.QEMU_IMG
KERNEL = devpaths.KERNEL
CMOS = devpaths.CMOS

# The turnkey DDE image: after a cold boot `cc -o hi c.hello` compiles,
# links and runs with no setup.  Held read-only; every instance writes to
# its own overlay.
BASE_IMAGE = devpaths.BASE_IMAGE

# Outside both repositories: this is machine state, not source, and a
# 2 GiB-backed overlay has no business in git.
FARM = devpaths.FARM

# The build's executables link the MinGW runtime DLLs and die silently
# without them on PATH.
MINGW_BIN = devpaths.MINGW_BIN

# Named rather than numbered, because "charlie is wedged" is a sentence and
# "instance 2 is wedged" is a lookup.
INSTANCES = ["alpha", "bravo", "charlie", "delta"]


def child_env():
    env = os.environ.copy()
    if os.path.isdir(MINGW_BIN):
        env["PATH"] = MINGW_BIN + os.pathsep + env.get("PATH", "")
    return env


def instance_dir(name):
    return os.path.join(FARM, name)


def paths(name):
    d = instance_dir(name)
    return {
        "dir": d,
        "config": os.path.join(d, "instance.json"),
        "overlay": os.path.join(d, "overlay.qcow2"),
        "share": os.path.join(d, "share"),
        "logs": os.path.join(d, "logs"),
        "screen": os.path.join(d, "screen"),
        "log": os.path.join(d, "logs", "qemu.log"),
        "qmp": os.path.join(d, "qmp.sock"),
    }


def resolve(which):
    if which in ("all", None):
        return list(INSTANCES)
    if which not in INSTANCES:
        raise SystemExit(f"no instance called {which!r}; try: farm.py ls")
    return [which]


# --------------------------------------------------------------------- QMP


def qmp_connect(sock, timeout=2.0):
    f = qmpunix.connect(sock, timeout=timeout)
    f.readline()                                   # greeting
    f.write(b'{"execute":"qmp_capabilities"}\n')
    f.readline()
    return f


def qmp_execute(sock, command, arguments=None, timeout=30.0):
    """One QMP command. Raises if the machine is not listening."""
    f = qmp_connect(sock, timeout=timeout)
    try:
        request = {"execute": command}
        if arguments:
            request["arguments"] = arguments
        f.write(json.dumps(request).encode() + b"\n")

        deadline = time.time() + timeout
        while time.time() < deadline:
            f.settimeout(max(0.1, deadline - time.time()))
            line = f.readline()
            if not line:
                raise RuntimeError("QMP closed")
            try:
                message = json.loads(line)
            except ValueError:
                continue
            if "return" in message:
                return message["return"]
            if "error" in message:
                raise RuntimeError(message["error"]["desc"])
        raise TimeoutError(command)
    finally:
        f.close()


def is_up(sock):
    try:
        f = qmp_connect(sock, timeout=0.6)
        f.close()
        return True
    except OSError:
        return False


# ------------------------------------------------------------------ create


def create(args):
    os.makedirs(FARM, exist_ok=True)

    for name in INSTANCES:
        p = paths(name)
        for key in ("dir", "share", "logs", "screen"):
            os.makedirs(p[key], exist_ok=True)

        if not os.path.exists(p["overlay"]):
            subprocess.run(
                [QEMU_IMG, "create", "-f", "qcow2", "-F", "raw",
                 "-b", BASE_IMAGE, p["overlay"]],
                check=True, env=child_env(),
                stdout=subprocess.DEVNULL)
            made = "created"
        else:
            made = "kept"

        # A file the guest can see immediately, so the share is obviously
        # the right one when you are looking at four identical desktops.
        marker = os.path.join(p["share"], "WhoAmI,fff")
        if not os.path.exists(marker):
            with open(marker, "w", newline="\r") as fh:
                fh.write(f"{name}\rQMP socket {p['qmp']}\r")

        with open(p["config"], "w", encoding="utf-8") as fh:
            json.dump({
                "name": name,
                "qmp": p["qmp"],
                "base_image": BASE_IMAGE,
                "kernel": KERNEL,
                "cmos": CMOS,
                "overlay": p["overlay"],
                "share": p["share"],
            }, fh, indent=2)

        print(f"{name:<8} {p['qmp']}  overlay {made}  {p['dir']}")

    print()
    print("Base image is shared read-only; each overlay holds only that "
          "machine's writes.")
    return 0


# ---------------------------------------------------------------------- up


def command_line(name, display, audiodev, kernel, cmos, card=True):
    p = paths(name)
    argv = [
        QEMU,
        "-name", f"riscos-{name}",
        "-M", "raspi4b",
        "-cpu", "cortex-a72,aarch64=off",
        "-kernel", kernel,
        "-device", f"loader,file={cmos},addr=0x510000,force-raw=on",
        "-netdev", "user,id=n0,domainname=lan",
        "-device", "usb-hub,bus=usb-bus.0,port=1",
        "-device", "usb-kbd,bus=usb-bus.0,port=1.1",
        "-device", "usb-tablet,bus=usb-bus.0,port=1.2",
        "-device", "usb-net,netdev=n0,rndis=off,bus=usb-bus.0,port=1.3",
        # Sound needs both halves: a backend, and the vchiq peer told to
        # use it. Four machines sharing one sound card is fine -- they
        # mix -- but --audiodev none is there for a quiet farm.
        "-audiodev", f"{audiodev},id=snd0",
        "-global", "bcm2835-vchiq.audiodev=snd0",
        "-display", display,
        "-serial", "null",
        "-qmp", f"unix:{p['qmp']},server,nowait",
        "-global", f"bcm2838-peripherals.vmchannel-root={p['share']}",
    ]
    # No card at all: the share is the whole machine.  It needs to hold a
    # !Boot for that to mean anything -- see tools/README.md.
    if card:
        argv += ["-drive", f"file={p['overlay']},if=sd,format=qcow2"]
    return argv


def up(args):
    failures = 0

    for name in resolve(args.which):
        p = paths(name)

        if not args.no_card and not os.path.exists(p["overlay"]):
            print(f"{name:<8} no overlay; run: farm.py create")
            failures += 1
            continue

        if is_up(p["qmp"]):
            print(f"{name:<8} already up ({p['qmp']})")
            continue

        # Which ROM and CMOS this machine boots.  run.py follows the Mac
        # rule that a share brings HostFS into the ROM; the farm cannot,
        # because every instance has a share and the base image already
        # soft-loads its own HostFS from PreDesk -- two versions of one
        # module in one machine, which nobody has tried.  So here it is
        # opt-in, and --boot hostfs turns it on because booting off the
        # share needs the module in the ROM to get there.
        rom_hostfs = args.rom_hostfs or args.boot == "hostfs"
        try:
            kernel = rom.rom_to_boot(
                KERNEL, args.modules,
                hostfs=p["share"] if rom_hostfs else None)
            # Per instance: four machines have four shares, so four CMOS
            # blobs, and a shared output path would be a race.
            cmos = rom.cmos_to_boot(CMOS, args.boot, hostfs=p["share"],
                                    out_dir=p["dir"])
        except rom.RomError as exc:
            print(f"{name:<8} {exc}")
            failures += 1
            continue

        argv = command_line(name, args.display, args.audiodev,
                            kernel, cmos, card=not args.no_card)
        log = open(p["log"], "ab", buffering=0)
        log.write(f"\n=== {time.strftime('%Y-%m-%d %H:%M:%S')} "
                  f"{' '.join(argv)}\n".encode())

        # A crashed machine leaves its socket file behind, and a unix
        # bind on an existing path fails, so clear the way first.
        if os.path.exists(p["qmp"]):
            os.remove(p["qmp"])

        # Detached, so the machine outlives this script.
        flags = 0
        if os.name == "nt":
            flags = (subprocess.DETACHED_PROCESS
                     | subprocess.CREATE_NEW_PROCESS_GROUP)

        # Its own doorbell trace.  Four machines appending to one file,
        # with no machine name and no timestamp on a line, cannot be read
        # afterwards: you cannot tell whose failure you are looking at.
        env = child_env()
        env["VMCH_TRACE"] = os.path.join(p["logs"], "vmch-trace.txt")

        process = subprocess.Popen(
            argv, env=env, stdout=log, stderr=log,
            stdin=subprocess.DEVNULL, creationflags=flags,
            cwd=p["dir"])

        # QEMU opens the QMP socket before it runs a guest instruction, so
        # this says "the process is alive and listening", not "RISC OS has
        # booted". Booting the desktop takes tens of seconds.
        for _ in range(60):
            time.sleep(0.5)
            if is_up(p["qmp"]):
                break
            if process.poll() is not None:
                break

        if is_up(p["qmp"]):
            print(f"{name:<8} up   pid {process.pid}  qmp {p['qmp']}")
        else:
            print(f"{name:<8} FAILED to listen on {p['qmp']}; "
                  f"see {p['log']}")
            failures += 1

    return 1 if failures else 0


# -------------------------------------------------------------------- down


def down(args):
    for name in resolve(args.which):
        p = paths(name)
        sock = p["qmp"]

        if not is_up(sock):
            print(f"{name:<8} not running")
            continue

        try:
            qmp_execute(sock, "quit", timeout=5)
        except (OSError, RuntimeError, TimeoutError):
            pass                       # quit closes the socket under us

        for _ in range(20):
            time.sleep(0.25)
            if not is_up(sock):
                break

        if is_up(sock):
            print(f"{name:<8} did not quit; killing by window name")
            subprocess.run(
                ["taskkill", "/FI", f"WINDOWTITLE eq riscos-{name}", "/F"],
                capture_output=True)
            time.sleep(1)

        print(f"{name:<8} {'down' if not is_up(sock) else 'STILL UP'}")

    return 0


# ------------------------------------------------------------------ status


def status(args):
    print(f"{'name':<8} {'state':<9} {'overlay':>9}  share")
    for name in INSTANCES:
        p = paths(name)
        live = is_up(p["qmp"])

        size = "-"
        if os.path.exists(p["overlay"]):
            size = f"{os.path.getsize(p['overlay']) / (1 << 20):.0f} MB"

        state = "running" if live else (
            "stopped" if os.path.exists(p["overlay"]) else "not made")

        print(f"{name:<8} {state:<9} {size:>9}  {p['share']}")
    return 0


def ls(args):
    for name in INSTANCES:
        p = paths(name)
        print(f"{name}")
        print(f"    qmp      {p['qmp']}")
        print(f"    overlay  {p['overlay']}")
        print(f"    share    {p['share']}")
        print(f"    log      {p['log']}")
    return 0


# -------------------------------------------------------------- shot / hmp


def shot(args):
    for name in resolve(args.which):
        p = paths(name)
        if not is_up(p["qmp"]):
            print(f"{name:<8} not running")
            continue
        stamp = time.strftime("%Y%m%d-%H%M%S")
        out = os.path.join(p["screen"], f"{stamp}.ppm")
        qmp_execute(p["qmp"], "screendump", {"filename": out})
        print(f"{name:<8} {out}")
    return 0


def hmp(args):
    p = paths(args.which)
    if not is_up(p["qmp"]):
        raise SystemExit(f"{args.which} is not running")
    print(qmp_execute(p["qmp"], "human-monitor-command",
                      {"command-line": args.command}))
    return 0


# ------------------------------------------------------------------- reset


def reset(args):
    for name in resolve(args.which):
        p = paths(name)
        if is_up(p["qmp"]):
            print(f"{name:<8} is running; stop it first (farm.py down {name})")
            continue

        p = paths(name)
        if os.path.exists(p["overlay"]):
            os.remove(p["overlay"])
        subprocess.run(
            [QEMU_IMG, "create", "-f", "qcow2", "-F", "raw",
             "-b", BASE_IMAGE, p["overlay"]],
            check=True, env=child_env(), stdout=subprocess.DEVNULL)

        # The share is host-side work, not machine state; throwing away the
        # disc should not throw away what you were building.
        print(f"{name:<8} disc reset to the base image; share left alone")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("create").set_defaults(func=create)
    sub.add_parser("status").set_defaults(func=status)
    sub.add_parser("ls").set_defaults(func=ls)

    p_up = sub.add_parser("up")
    p_up.add_argument("which", nargs="?", default="all")
    p_up.add_argument("--display", default="none",
                      help="none (default) or dx11 to watch one")
    p_up.add_argument("--audiodev", default="dsound",
                      help="host audio driver (dsound default on Windows, "
                           "coreaudio on macOS, none for a quiet farm)")
    p_up.add_argument("--boot", choices=("hostfs",), metavar="SOURCE",
                      help="'hostfs' boots the instance's own share "
                           "instead of its overlay; implies --rom-hostfs")
    p_up.add_argument("--rom-hostfs", action="store_true",
                      help="splice HostFS and its icon-bar filer into the "
                           "ROM (the base image soft-loads its own from "
                           "PreDesk, so this is off by default)")
    p_up.add_argument("--modules", nargs="*", default=[], metavar="FILE",
                      help="further modules spliced into the ROM, in "
                           "initialisation order")
    p_up.add_argument("--no-card", action="store_true",
                      help="attach no SD card: the instance's share is the "
                           "whole machine (it must hold a !Boot)")
    p_up.set_defaults(func=up)

    for cmd, fn in (("down", down), ("reset", reset), ("shot", shot)):
        p = sub.add_parser(cmd)
        p.add_argument("which", nargs="?", default="all")
        p.set_defaults(func=fn)

    p_hmp = sub.add_parser("hmp")
    p_hmp.add_argument("which")
    p_hmp.add_argument("command")
    p_hmp.set_defaults(func=hmp)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
