#!/usr/bin/env python3
"""Build a HostFS tree from a card, and verify every directory in it.

    mktree.py --card F:/RISCOSDEV/roms/sdimg/dde_dev.img --share F:/trees/dev

Copies a card's contents onto a host directory through HostFS, checks
each directory against the card, and descends into any that came up
short so the entry that will not copy is named rather than lost.  Prints
what is missing and exits non-zero if anything is.

Why this is not `*Copy SDFS::0.$ HostFS:$ ~C F R ~V` in a shell script:

  - **A recursive *Copy stops at the first name the host cannot store
    and abandons everything after it** (ROS_PRIVATE#6).  One error line,
    five levels down, and a tree 40% short that looks finished.
  - **`*Count` on HostFS under-reports.**  A directory of more than 63
    entries is truncated (#8): measured here, a complete 762-file
    `AcornC/C++` counted as 364.  So the card side is counted with
    `*Count` over SDFS, where it is right, and the host side by walking
    the host filesystem.  A HostFS count is never trusted for anything.
  - **Two name failures are silent on Windows.**  An untyped `NUL`
    becomes the null device, and a trailing dot or space is stripped so
    two names collide into one file.  Neither raises an error, so only a
    file-by-file count catches them.

The guest's answers come back through the share itself: `*Count x
{ > HostFS:$.<file> }` writes where the host can read it, rather than
onto a screen someone has to read off a screenshot.
"""

import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import keys                                            # noqa: E402
import rom                                             # noqa: E402

import devpaths

QEMU = devpaths.QEMU
KERNEL = devpaths.KERNEL
CMOS = devpaths.CMOS
MINGW_BIN = devpaths.MINGW_BIN

OUT = "___mktree"          # scratch file in the share, for guest output
OUT_NAMES = (OUT + ",ffd", OUT + ",fff", OUT)
COUNT_RE = re.compile(r"([\d,]+)\s+files counted,\s+total\s+([\d,]+)\s+bytes")


# ---------------------------------------------------------------- names

def host_of(leaf):
    """The host spelling of one RISC OS name.

    A literal '/' in a RISC OS name is '.' on the host, and the other way
    round -- the swap FSDESIGN-V1 section 6 describes.  Nothing else is
    mapped here: when the escaping for host-illegal characters lands
    (ROS_PRIVATE#6) it belongs in the emulator, so that a tree built by
    hand and a tree built by this agree.
    """
    return leaf.replace("/", ".")


def find_host(host_dir, leaf):
    """Where `leaf` landed on the host, typed or not, or None."""
    base = host_of(leaf)
    direct = os.path.join(host_dir, base)
    if os.path.exists(direct):
        return direct
    try:
        entries = os.listdir(host_dir)
    except OSError:
        return None
    for e in entries:                      # a typed file gains ",xxx"
        if e.startswith(base + ",") and len(e) == len(base) + 4:
            return os.path.join(host_dir, e)
    return None


def walk_count(path):
    """Files and bytes under a host path -- the authoritative side."""
    if path is None or not os.path.exists(path):
        return 0, 0
    if os.path.isfile(path):
        return 1, os.path.getsize(path)
    n = b = 0
    for root, _, files in os.walk(path):
        for f in files:
            try:
                b += os.path.getsize(os.path.join(root, f))
                n += 1
            except OSError:
                pass
    return n, b


# ---------------------------------------------------------------- guest

class Guest:
    """A machine with the card attached and the tree as its share."""

    def __init__(self, port, share):
        self.q = keys.Qmp(port)
        self.share = share

    def cmd(self, line, settle=1.0):
        self.q.text(line)
        self.q.tap("ret")
        time.sleep(settle)

    def _clear_out(self):
        for name in OUT_NAMES:
            p = os.path.join(self.share, name)
            if os.path.exists(p):
                os.remove(p)

    def capture(self, line, timeout=600):
        """Run a command with its output redirected into the share, and
        read it back on the host."""
        self._clear_out()
        self.cmd(f"{line} {{ > HostFS:$.{OUT} }}", settle=0.4)
        deadline = time.time() + timeout
        while time.time() < deadline:
            for name in OUT_NAMES:
                p = os.path.join(self.share, name)
                if os.path.exists(p):
                    time.sleep(0.4)        # let the write finish
                    with open(p, "rb") as fh:
                        text = fh.read().decode("latin-1")
                    os.remove(p)
                    return text.replace("\r", "\n")
            time.sleep(0.5)
        raise TimeoutError(line)

    def entries(self, guest_dir):
        """[(name, is_dir)] for a card directory, from *Ex.

        *Ex prints one entry per line; *Cat's columns get clipped at the
        screen edge, which is how a listing quietly loses entries.  A
        name containing a real space would break the split -- RISC OS
        discs use a hard space (&A0) for that, which does not.
        """
        found = []
        for line in self.capture(f"Ex {guest_dir}").splitlines():
            if line.startswith(("Dir.", "CSD", "Lib.", "URD", "*")):
                continue
            parts = line.split()
            if len(parts) < 3:
                continue
            found.append((parts[0], parts[2] == "Directory"))
        return found

    def count(self, guest_path):
        """(files, bytes) for a card path, counted on the card."""
        out = self.capture(f"Count {guest_path}")
        m = COUNT_RE.search(out)
        if not m:
            raise RuntimeError(f"unreadable Count for {guest_path}: {out!r}")
        return (int(m.group(1).replace(",", "")),
                int(m.group(2).replace(",", "")))

    def copy(self, src, dst, recurse, timeout=1800):
        """*Copy, and wait for the share to stop growing.

        There is no completion signal to wait for: with ~V the command
        prints nothing at all when it works, and one line when it does
        not.  Settling is what there is.
        """
        self.cmd(f"Copy {src} {dst} {'~C F R ~V' if recurse else '~C F ~V'}",
                 settle=0.4)
        last, stable = None, 0
        deadline = time.time() + timeout
        while time.time() < deadline:
            time.sleep(3)
            now = walk_count(self.share)
            if now == last:
                stable += 1
                if stable >= 3:
                    return
            else:
                last, stable = now, 0


# ---------------------------------------------------------------- build

def build(guest, rel, host_dir, report, log, depth=0, only=()):
    """Copy one card directory, verify it, and descend where it is short.

    `rel` is the path under `$`, empty at the root.  A directory that
    verifies is left alone; one that does not is re-driven a level
    deeper, which isolates the entry that cannot be copied instead of
    losing it and everything after it.
    """
    pad = "  " * depth
    guest_dir = "SDFS::0.$" + ("." + rel if rel else "")

    for name, is_dir in guest.entries(guest_dir):
        if depth == 0 and only and name not in only:
            continue
        child = f"{rel}.{name}" if rel else name
        want = guest.count(f"SDFS::0.$.{child}") if is_dir else None

        guest.copy(f"SDFS::0.$.{child}", f"HostFS:$.{child}", is_dir)
        got = walk_count(find_host(host_dir, name))

        if not is_dir:
            log(f"{pad}{name}  file, {got[1]:,} bytes")
            continue
        if got == want:
            log(f"{pad}{name}  {got[0]} files  ok")
            continue

        log(f"{pad}{name}  {got[0]}/{want[0]} files -- short, descending")
        hdir = os.path.join(host_dir, host_of(name))
        os.makedirs(hdir, exist_ok=True)
        build(guest, child, hdir, report, log, depth + 1)

        got = walk_count(find_host(host_dir, name))
        if got != want:
            report.append((f"SDFS::0.$.{child}", want, got))
            log(f"{pad}{name}  STILL SHORT {got[0]}/{want[0]} files")


def wait_desktop(guest, qemu, log, budget=420):
    """A settled screen.  Boots here run from 15 s to several minutes
    depending on what else is on the host, so the budget is generous."""
    shot = os.path.join(os.path.dirname(os.path.abspath(qemu)),
                        "mktree-poll.ppm")
    t0 = time.time()
    prev = stable = None
    while time.time() - t0 < budget:
        try:
            guest.q.execute("screendump", {"filename": shot})
            px = open(shot, "rb").read().split(b"\n", 3)[3]
        except Exception:
            time.sleep(4)
            continue
        if any(px[:48000]):
            if px == prev:
                if stable is None:
                    stable = time.time()
                if time.time() - stable >= 8:
                    log(f"desktop after {time.time() - t0:.0f}s")
                    return True
            else:
                prev, stable = px, None
        time.sleep(4)
    log("no settled desktop; carrying on anyway")
    return False


def launch(args, log):
    kernel = rom.rom_to_boot(args.kernel, hostfs=args.share)
    env = os.environ.copy()
    if os.path.isdir(MINGW_BIN):
        env["PATH"] = MINGW_BIN + os.pathsep + env.get("PATH", "")
    env["VMCH_TRACE"] = os.path.abspath(
        os.path.join(args.share, os.pardir, "mktree-vmch-trace.txt"))

    argv = [
        args.qemu, "-name", "riscos-mktree",
        "-M", "raspi4b", "-cpu", "cortex-a72,aarch64=off",
        "-kernel", kernel,
        "-device", f"loader,file={args.cmos},addr=0x510000,force-raw=on",
        # snapshot=on: a shared card image is never written to, which
        # also makes deleting BootHostFS below free.
        "-drive", f"file={args.card},if=sd,format=raw,snapshot=on",
        "-netdev", "user,id=n0",
        "-device", "usb-hub,bus=usb-bus.0,port=1",
        "-device", "usb-kbd,bus=usb-bus.0,port=1.1",
        "-device", "usb-tablet,bus=usb-bus.0,port=1.2",
        "-device", "usb-net,netdev=n0,rndis=off,bus=usb-bus.0,port=1.3",
        "-audiodev", "none,id=snd0",
        "-global", "bcm2835-vchiq.audiodev=snd0",
        "-display", "none", "-serial", "null",
        "-qmp", f"tcp:127.0.0.1:{args.port},server,nowait",
        "-global", f"bcm2838-peripherals.vmchannel-root={args.share}",
    ]
    flags = 0
    if os.name == "nt":
        flags = (subprocess.DETACHED_PROCESS
                 | subprocess.CREATE_NEW_PROCESS_GROUP)
    proc = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, creationflags=flags,
                            cwd=os.path.dirname(os.path.abspath(args.qemu)))
    log(f"machine {proc.pid} on qmp {args.port}, card {args.card}")
    return proc


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--card", required=True, help="card image to copy from")
    ap.add_argument("--share", required=True, help="host directory to build")
    ap.add_argument("--port", type=int, default=4480, help="QMP port")
    ap.add_argument("--qemu", default=QEMU)
    ap.add_argument("--kernel", default=KERNEL)
    ap.add_argument("--cmos", default=CMOS)
    ap.add_argument("--only", metavar="NAME", action="append", default=[],
                    help="only this top-level entry (repeatable)")
    ap.add_argument("--keep-up", action="store_true",
                    help="leave the machine running afterwards")
    args = ap.parse_args()

    args.share = os.path.abspath(args.share)
    os.makedirs(args.share, exist_ok=True)

    def log(msg):
        print(msg, flush=True)

    launch(args, log)
    guest = Guest(args.port, args.share)
    wait_desktop(guest, args.qemu, log)

    # The card's own HostFS would replace the ROM's: PreDesk.BootHostFS
    # is an unconditional RMLoad, and RMLoad wins over a ROM module of
    # the same name, so without this the tree is built by 1.01 and the
    # copies fail on operations only 2.01 has (FSDESIGN-V1 section 13,
    # R2).  The card is opened snapshot=on, so this never reaches it.
    guest.q.tap("f12", modifiers=["ctrl"])
    time.sleep(keys.SETTLE)
    guest.cmd("Delete SDFS::0.$.!Boot.Choices.Boot.PreDesk.BootHostFS", 2)
    guest.q.execute("system_reset")
    log("BootHostFS removed and reset, so the ROM's HostFS stands")
    wait_desktop(guest, args.qemu, log)
    guest.q.tap("f12", modifiers=["ctrl"])
    time.sleep(keys.SETTLE)

    report = []
    started = time.time()
    build(guest, "", args.share, report, log, only=tuple(args.only))

    log("")
    if report:
        log(f"INCOMPLETE -- {len(report)} path(s) could not be copied:")
        for path, want, got in report:
            log(f"  {path}: {got[0]}/{want[0]} files, "
                f"{got[1]:,}/{want[1]:,} bytes")
        log("")
        log("A path that will not copy is usually a name this host cannot")
        log("store.  The trace beside the share names it (ROS_PRIVATE#6).")
    else:
        total = walk_count(args.share)
        log(f"complete -- every directory matches the card: "
            f"{total[0]:,} files, {total[1] / 1e6:.1f} MB "
            f"in {time.time() - started:.0f}s")

    if not args.keep_up:
        try:
            guest.q.execute("quit", timeout=5)
        except Exception:
            pass                            # quit closes the socket under us
    return 1 if report else 0


if __name__ == "__main__":
    sys.exit(main())
