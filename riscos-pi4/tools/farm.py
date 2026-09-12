#!/usr/bin/env python3
"""Four named RISC OS machines that cannot tread on each other.

    farm.py create              make (or repair) all four instances
    farm.py up alpha            start one
    farm.py up all              start all four
    farm.py status              which are running, on which port
    farm.py shot alpha          screendump to its screen/ directory
    farm.py hmp alpha "info registers"
    farm.py down bravo          stop one politely, then firmly
    farm.py reset charlie       throw away its disc writes, keep the share
    farm.py ls                  names, ports and paths

Why a farm rather than four copies of run.py: three things are shared by
default and every one of them is a way for two machines to corrupt each
other's work.

  - **The disc.**  Each instance gets its own qcow2 overlay over the one
    read-only card image, so writes are private and starting a new machine
    costs a few hundred kilobytes rather than two gigabytes.
  - **The HostFS share.**  This is how a compiler gets binaries into the
    guest and results back out, so two machines sharing one host directory
    would overwrite each other's build outputs.  Each gets its own.
  - **The control port.**  QMP is how anything drives the machine; one port
    means one machine.  run.py's 4461 is deliberately left alone so a hand
    session can still use it while the farm runs.

Headless by default.  Four windows is not a thing anyone wants, and driving
a window means posting messages at it, which moves the real mouse pointer.
QMP does screendumps and input-send-event perfectly well with no display at
all.  Pass --display dx11 when you actually want to watch one.
"""

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

QEMU = r"F:\RISCOSDEV\qemu\build\qemu-system-aarch64.exe"
QEMU_IMG = r"F:\RISCOSDEV\qemu\build\qemu-img.exe"
KERNEL = r"F:\RISCOSDEV\roms\pi\v530\RISCOS.IMG"
CMOS = r"F:\RISCOSDEV\roms\pi\rool-cmos-unplug.bin"

# The turnkey DDE image: after a cold boot `cc -o hi c.hello` compiles,
# links and runs with no setup.  Held read-only; every instance writes to
# its own overlay.
BASE_IMAGE = r"F:\RISCOSDEV\roms\sdimg\dde_dev.img"

# Outside both repositories: this is machine state, not source, and a
# 2 GiB-backed overlay has no business in git.
FARM = r"F:\RISCOSDEV\qemu-farm"

# The build's executables link the MinGW runtime DLLs and die silently
# without them on PATH.
MINGW_BIN = r"F:\RISCOSDEV\msys64\mingw64\bin"

# Named rather than numbered, because "charlie is wedged" is a sentence and
# "instance 2 is wedged" is a lookup.  4461 belongs to run.py.
INSTANCES = [
    ("alpha", 4471),
    ("bravo", 4472),
    ("charlie", 4473),
    ("delta", 4474),
]


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
    }


def port_of(name):
    for n, p in INSTANCES:
        if n == name:
            return p
    raise SystemExit(f"no instance called {name!r}; try: farm.py ls")


def resolve(which):
    if which in ("all", None):
        return [n for n, _ in INSTANCES]
    if which not in [n for n, _ in INSTANCES]:
        raise SystemExit(f"no instance called {which!r}; try: farm.py ls")
    return [which]


# --------------------------------------------------------------------- QMP


def qmp_connect(port, timeout=2.0):
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    f = s.makefile("rb")
    f.readline()                                   # greeting
    s.sendall(b'{"execute":"qmp_capabilities"}\n')
    f.readline()
    return s, f


def qmp_execute(port, command, arguments=None, timeout=30.0):
    """One QMP command. Raises if the machine is not listening."""
    s, f = qmp_connect(port, timeout=timeout)
    try:
        request = {"execute": command}
        if arguments:
            request["arguments"] = arguments
        s.sendall(json.dumps(request).encode() + b"\n")

        deadline = time.time() + timeout
        while time.time() < deadline:
            s.settimeout(max(0.1, deadline - time.time()))
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
        s.close()


def is_up(port):
    try:
        s, _ = qmp_connect(port, timeout=0.6)
        s.close()
        return True
    except OSError:
        return False


# ------------------------------------------------------------------ create


def create(args):
    os.makedirs(FARM, exist_ok=True)

    for name, port in INSTANCES:
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
                fh.write(f"{name}\rQMP port {port}\r")

        with open(p["config"], "w", encoding="utf-8") as fh:
            json.dump({
                "name": name,
                "qmp_port": port,
                "base_image": BASE_IMAGE,
                "kernel": KERNEL,
                "cmos": CMOS,
                "overlay": p["overlay"],
                "share": p["share"],
            }, fh, indent=2)

        print(f"{name:<8} port {port}  overlay {made}  {p['dir']}")

    print()
    print("Base image is shared read-only; each overlay holds only that "
          "machine's writes.")
    return 0


# ---------------------------------------------------------------------- up


def command_line(name, port, display, audiodev):
    p = paths(name)
    return [
        QEMU,
        "-name", f"riscos-{name}",
        "-M", "raspi4b",
        "-cpu", "cortex-a72,aarch64=off",
        "-kernel", KERNEL,
        "-device", f"loader,file={CMOS},addr=0x510000,force-raw=on",
        "-drive", f"file={p['overlay']},if=sd,format=qcow2",
        "-netdev", "user,id=n0",
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
        "-qmp", f"tcp:127.0.0.1:{port},server,nowait",
        "-global", f"bcm2838-peripherals.vmchannel-root={p['share']}",
    ]


def up(args):
    failures = 0

    for name in resolve(args.which):
        port = port_of(name)
        p = paths(name)

        if not os.path.exists(p["overlay"]):
            print(f"{name:<8} no overlay; run: farm.py create")
            failures += 1
            continue

        if is_up(port):
            print(f"{name:<8} already up on {port}")
            continue

        argv = command_line(name, port, args.display, args.audiodev)
        log = open(p["log"], "ab", buffering=0)
        log.write(f"\n=== {time.strftime('%Y-%m-%d %H:%M:%S')} "
                  f"{' '.join(argv)}\n".encode())

        # Detached, so the machine outlives this script.
        flags = 0
        if os.name == "nt":
            flags = (subprocess.DETACHED_PROCESS
                     | subprocess.CREATE_NEW_PROCESS_GROUP)

        process = subprocess.Popen(
            argv, env=child_env(), stdout=log, stderr=log,
            stdin=subprocess.DEVNULL, creationflags=flags,
            cwd=p["dir"])

        # QEMU opens the QMP socket before it runs a guest instruction, so
        # this says "the process is alive and listening", not "RISC OS has
        # booted". Booting the desktop takes tens of seconds.
        for _ in range(60):
            time.sleep(0.5)
            if is_up(port):
                break
            if process.poll() is not None:
                break

        if is_up(port):
            print(f"{name:<8} up   pid {process.pid}  qmp {port}")
        else:
            print(f"{name:<8} FAILED to listen on {port}; see {p['log']}")
            failures += 1

    return 1 if failures else 0


# -------------------------------------------------------------------- down


def down(args):
    for name in resolve(args.which):
        port = port_of(name)

        if not is_up(port):
            print(f"{name:<8} not running")
            continue

        try:
            qmp_execute(port, "quit", timeout=5)
        except (OSError, RuntimeError, TimeoutError):
            pass                       # quit closes the socket under us

        for _ in range(20):
            time.sleep(0.25)
            if not is_up(port):
                break

        if is_up(port):
            print(f"{name:<8} did not quit; killing by window name")
            subprocess.run(
                ["taskkill", "/FI", f"WINDOWTITLE eq riscos-{name}", "/F"],
                capture_output=True)
            time.sleep(1)

        print(f"{name:<8} {'down' if not is_up(port) else 'STILL UP'}")

    return 0


# ------------------------------------------------------------------ status


def status(args):
    print(f"{'name':<8} {'qmp':>5}  {'state':<9} {'overlay':>9}  share")
    for name, port in INSTANCES:
        p = paths(name)
        live = is_up(port)

        size = "-"
        if os.path.exists(p["overlay"]):
            size = f"{os.path.getsize(p['overlay']) / (1 << 20):.0f} MB"

        state = "running" if live else (
            "stopped" if os.path.exists(p["overlay"]) else "not made")

        print(f"{name:<8} {port:>5}  {state:<9} {size:>9}  {p['share']}")
    return 0


def ls(args):
    for name, port in INSTANCES:
        p = paths(name)
        print(f"{name}")
        print(f"    qmp      127.0.0.1:{port}")
        print(f"    overlay  {p['overlay']}")
        print(f"    share    {p['share']}")
        print(f"    log      {p['log']}")
    return 0


# -------------------------------------------------------------- shot / hmp


def shot(args):
    for name in resolve(args.which):
        port = port_of(name)
        if not is_up(port):
            print(f"{name:<8} not running")
            continue
        p = paths(name)
        stamp = time.strftime("%Y%m%d-%H%M%S")
        out = os.path.join(p["screen"], f"{stamp}.ppm")
        qmp_execute(port, "screendump", {"filename": out})
        print(f"{name:<8} {out}")
    return 0


def hmp(args):
    port = port_of(args.which)
    if not is_up(port):
        raise SystemExit(f"{args.which} is not running")
    print(qmp_execute(port, "human-monitor-command",
                      {"command-line": args.command}))
    return 0


# ------------------------------------------------------------------- reset


def reset(args):
    for name in resolve(args.which):
        port = port_of(name)
        if is_up(port):
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
