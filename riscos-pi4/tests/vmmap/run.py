#!/usr/bin/env python3
"""run.py -- run one VMTest on a fresh headless machine and keep the evidence.

    run.py svcread   --target ram --size 16384 --slot 64 --ramfs 64
    run.py usrtouch  --target ram --size 16384 --slot 64 --ramfs 64
    run.py svcwrite  --target hostfs --size 8192 --slot 64 --vmch-trace
    run.py ramstress --target ram --size 16384 --slot 128 --ramfs 512 --rounds 2

Each run unpacks a scratch copy of a release disc, puts vmtest.bas (numbered)
and a boot task that runs it on the disc, and boots the release's ROM in the
emulator with no display.  The guest-abort trace points (arm_a32_data_abort,
arm_a32_prefetch_abort) record every abort the guest takes, with the PC, the
mode it came from and the fault status and address; VMCH_TRACE records the
host's page-table walks for HostFS transfers.  When the task finishes, or the
timeout passes, the harness takes a screenshot and the CPU registers, stops
the machine and writes everything to build-macos/vmmap-results/<run>/, then
deletes the scratch disc unless --keep is given.

The disc boots the ROM's own network stack with HostNet moved to
Modules/Disabled, and the emulated card on a slirp restricted from the host's
network: nothing reaches out, and HostNet's traffic stays out of the traces.
"""

import argparse
import glob
import io
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
QEMU = os.path.join(REPO, "build-macos", "qemu-system-aarch64")
RESULTS = os.path.join(REPO, "build-macos", "vmmap-results")

MODES = {0x10: "USR", 0x11: "FIQ", 0x12: "IRQ", 0x13: "SVC", 0x16: "MON",
         0x17: "ABT", 0x1a: "HYP", 0x1b: "UND", 0x1f: "SYS"}

# ARMv7 short-descriptor fault status (FS = DFSR[10] : DFSR[3:0])
SHORT_FS = {
    0b00001: "alignment", 0b00100: "icache", 0b01100: "L1 ext on walk",
    0b01110: "L2 ext on walk", 0b00101: "section translation",
    0b00111: "page translation", 0b00011: "section access flag",
    0b00110: "page access flag", 0b01001: "section domain",
    0b01011: "page domain", 0b01101: "section permission",
    0b01111: "page permission", 0b01000: "sync external",
}


def newest_app():
    apps = sorted(glob.glob(os.path.join(REPO, "build-macos", "release",
                                         "RISCOSQEA72v*.app")),
                  key=os.path.getmtime)
    return apps[-1] if apps else None


def numbered(src):
    """BASIC text with line numbers, blank lines dropped."""
    out, n = [], 0
    for line in open(src, encoding="latin-1").read().splitlines():
        if line.strip():
            n += 10
            out.append(f"{n} {line}")
    return "\n".join(out) + "\n"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class QMP:
    def __init__(self, port, timeout=20):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.f = self.sock.makefile("rw")
        self._recv()
        self.cmd("qmp_capabilities")

    def _recv(self):
        while True:
            line = self.f.readline()
            if not line:
                raise ConnectionError("QMP closed")
            msg = json.loads(line)
            if "event" not in msg:
                return msg

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.f.flush()
        return self._recv()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def decode_fsr(fsr):
    if fsr & (1 << 9):                       # LPAE format
        return f"LPAE fs=0x{fsr & 0x3f:02x}"
    fs = (fsr & 0xf) | ((fsr >> 10) & 1) << 4
    kind = SHORT_FS.get(fs, f"fs=0b{fs:05b}")
    rw = "write" if fsr & (1 << 11) else "read"
    return f"{kind}, {rw}, domain {(fsr >> 4) & 0xf}"


TRACE_RE = re.compile(r"arm_a32_(data|prefetch)_abort cpu (\d+) pc 0x([0-9a-f]+) "
                      r"cpsr 0x([0-9a-f]+) [DI]FSR 0x([0-9a-f]+) [DI]FAR 0x([0-9a-f]+)")


def summarise_aborts(trace_path, out_path):
    """Group every guest abort by kind, mode, fault type and PC."""
    groups, firsts, total = Counter(), {}, 0
    svc_app = []
    if os.path.exists(trace_path):
        for line in open(trace_path, errors="replace"):
            m = TRACE_RE.search(line)
            if not m:
                continue
            total += 1
            kind, _, pc, cpsr, fsr, far = m.groups()
            pc, cpsr, fsr, far = (int(x, 16) for x in (pc, cpsr, fsr, far))
            mode = MODES.get(cpsr & 0x1f, hex(cpsr & 0x1f))
            key = (kind, mode, decode_fsr(fsr), pc)
            groups[key] += 1
            firsts.setdefault(key, far)
            if kind == "data" and mode != "USR" and far < 0x20000000:
                svc_app.append((pc, mode, fsr, far))
    with open(out_path, "w") as out:
        out.write(f"{total} guest aborts\n\n")
        out.write("count  kind      mode  pc          first far   fault\n")
        for (kind, mode, fault, pc), n in groups.most_common():
            out.write(f"{n:5d}  {kind:8s}  {mode:4s}  0x{pc:08x}  "
                      f"0x{firsts[(kind, mode, fault, pc)]:08x}  {fault}\n")
        out.write(f"\n{len(svc_app)} privileged-mode data aborts on "
                  "application space (far < &20000000):\n")
        for pc, mode, fsr, far in svc_app[:200]:
            out.write(f"  pc 0x{pc:08x} {mode} far 0x{far:08x} "
                      f"{decode_fsr(fsr)}\n")
    return total, len(svc_app)


WIMPFORTH = os.path.join(os.path.dirname(REPO), "wimpforth")
WF_ABSOLUTE = {"FWIN32", "FWIN", "FCONS32", "FCONS", "kernel32", "kernel"}


def wimpforth_setup(args, disc):
    """!WimpForth from a git revision, typed by suffix, with --forth lines
    appended to the config it FLOADs at start-up, launched as the case did
    it: WimpTask FWIN32 in the Next slot.  Done when TESTSAVE appears."""
    app = os.path.join(disc, "!WimpForth")
    os.makedirs(app)
    tar = subprocess.run(["git", "-C", WIMPFORTH, "archive", args.wimpforth_rev],
                         capture_output=True, check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(tar)) as t:
        for m in t.getmembers():
            if not m.isfile() or "/" in m.name:
                continue
            leaf = m.name
            if leaf == "!RUN":
                suffix = ",feb"
            elif leaf == "!SPRITES":
                suffix = ",ffd"
            elif leaf in WF_ABSOLUTE:
                suffix = ",ff8"
            elif leaf.lower().endswith((".jpg", ".jpeg", ".zip", ".png")):
                continue
            else:
                suffix = ",fff"
            data = t.extractfile(m).read()
            if leaf == "config":
                data = data.rstrip(b"\n") + b"\n" + "\n".join(args.forth).encode("latin-1") + b"\n"
            if leaf == "MAKEWIN":
                # MAKEWIN2: the same build without its closing "fsave FWIN"
                # and "bye", so a later --forth line runs in the built image.
                keep = [l for l in data.split(b"\n")
                        if l.strip().lower() not in (b"fsave fwin", b"bye")]
                with open(os.path.join(app, "MAKEWIN2,fff"), "wb") as f:
                    f.write(b"\n".join(keep))
            with open(os.path.join(app, leaf + suffix), "wb") as f:
                f.write(data)
    task = "\n".join([
        "| VMTest harness: WimpForth as the case launched it",
        "Dir HostFS:$.!WimpForth",
        f"WimpSlot -next {args.slot}M",
        "WimpTask FWIN32",
    ]) + "\n"
    print(f"  !WimpForth @ {args.wimpforth_rev}, config += {args.forth}, "
          f"Next slot {args.slot}M")
    return task, os.path.join(app, "TESTSAVE,ff8")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("test", help="svcread, svcwrite, usrtouch, ramstress")
    ap.add_argument("--target", choices=("ram", "hostfs"), default="ram")
    ap.add_argument("--size", type=int, default=16384, help="buffer KB")
    ap.add_argument("--slot", type=int, default=64, help="WimpSlot MB")
    ap.add_argument("--ramfs", type=int, default=0,
                    help="RAM disc MB to ask for (default: size*2 for ram)")
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--app", default=None,
                    help="release .app for the ROM and disc (default newest)")
    ap.add_argument("--qemu", default=QEMU)
    ap.add_argument("--no-gvfill", action="store_true",
                    help="boot without the GVFill blitter module")
    ap.add_argument("--vmch-trace", action="store_true")
    ap.add_argument("--no-abort-trace", action="store_true")
    ap.add_argument("--dint", action="store_true", help="also -d int")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--settle", type=int, default=0,
                    help="seconds to keep running after the done marker")
    ap.add_argument("--wimpforth-rev", default="81d59bf",
                    help="wimpforth git revision for the wimpforth test "
                         "(default: the pristine import, before the fsave "
                         "workaround)")
    ap.add_argument("--forth", action="append", default=[],
                    help="a line appended to WimpForth's config "
                         "(repeatable), e.g. --forth 'fsave TESTSAVE'")
    args = ap.parse_args()

    app = args.app or newest_app()
    if not app:
        sys.exit("run.py: no release app in build-macos/release; pass --app")
    res = os.path.join(app, "Contents", "Resources")
    rom, disczip = os.path.join(res, "RISCOS.IMG"), os.path.join(res, "Disc.zip")
    for p in (rom, disczip, args.qemu):
        if not os.path.exists(p):
            sys.exit(f"run.py: missing {p}")

    stamp = time.strftime("%Y%m%d-%H%M%S")
    name = (f"{stamp}-{args.test}-{args.target}-{args.size}K"
            f"{'-nogvfill' if args.no_gvfill else ''}")
    results = os.path.join(RESULTS, name)
    os.makedirs(results)
    scratch = tempfile.mkdtemp(prefix="vmmap-")
    disc = os.path.join(scratch, "disc")

    print(f"run {name}\n  app {app}\n  scratch {scratch}")
    with zipfile.ZipFile(disczip) as z:
        z.extractall(disc)
    # Modules/Disabled is where a disc module goes to be switched off:
    # PreDesk.HostModules loads only the top of Modules/.  HostNet always
    # goes there; GVFill (the blitter module) too with --no-gvfill.
    mods = os.path.join(disc, "Modules")
    for leaf, off in (("HostNet,ffa", True), ("GVFill,ffa", args.no_gvfill)):
        if off and os.path.exists(os.path.join(mods, leaf)):
            os.makedirs(os.path.join(mods, "Disabled"), exist_ok=True)
            shutil.move(os.path.join(mods, leaf),
                        os.path.join(mods, "Disabled", leaf))
            print(f"  {leaf} moved to Modules/Disabled")
    vmt = os.path.join(disc, "VMTest")
    os.makedirs(vmt)
    with open(os.path.join(vmt, "VMTest,fff"), "w", encoding="latin-1",
              newline="\n") as f:
        f.write(numbered(os.path.join(HERE, "vmtest.bas")))
    if args.test == "wimpforth":
        task, done = wimpforth_setup(args, disc)
    elif args.target == "ram":
        target = "RAM::RamDisc0.$"
        ramfs = args.ramfs or max(8, (args.size * 2) // 1024 + 8)
    else:
        os.makedirs(os.path.join(vmt, "data"))
        target = "HostFS:$.VMTest.data"
        ramfs = args.ramfs
    if args.test != "wimpforth":
        task = "\n".join([
            "| VMTest harness (a scratch disc; never shipped)",
            f"Set VMT$Test {args.test}",
            f"Set VMT$Target {target}",
            f"Set VMT$Size {args.size}",
            f"Set VMT$RamFS {ramfs}",
            f"Set VMT$Rounds {args.rounds}",
            f"WimpSlot -min {args.slot}M -max {args.slot}M",
            "X BASIC -quit HostFS:$.VMTest.VMTest { > HostFS:$.VMTest.out }",
            "Echo done { > HostFS:$.VMTest.done }",
        ]) + "\n"
        done = os.path.join(vmt, "done,ffd")
    with open(os.path.join(disc, "!Boot", "Choices", "Boot", "Tasks",
                           "ZZVMTest,feb"), "w", newline="\n") as f:
        f.write(task)
    cmos = os.path.join(scratch, "cmos.bin")
    shutil.copyfile(os.path.join(disc, "CMOS,ff2"), cmos)

    port = free_port()
    trace = os.path.join(scratch, "qemu-trace.log")
    cmd = [args.qemu, "-M", "raspi4b", "-cpu", "cortex-a72,aarch64=off",
           "-kernel", rom,
           "-device", f"loader,file={cmos},addr=0x510000,force-raw=on",
           "-netdev", "user,id=n0,restrict=on,domainname=lan",
           "-device", "usb-hub,bus=usb-bus.0,port=1",
           "-device", "usb-kbd,bus=usb-bus.0,port=1.1",
           "-device", "usb-tablet,bus=usb-bus.0,port=1.2",
           "-device", "usb-net,netdev=n0,rndis=off,bus=usb-bus.0,port=1.3",
           "-audiodev", "none,id=snd0", "-global", "bcm2835-vchiq.audiodev=snd0",
           "-display", "none", "-serial", "null", "-name", name,
           "-qmp", f"tcp:127.0.0.1:{port},server,nowait",
           "-global", f"bcm2838-peripherals.vmchannel-root={disc}",
           "-D", trace]
    if not args.no_abort_trace:
        cmd += ["-trace", "enable=arm_a32_data_abort",
                "-trace", "enable=arm_a32_prefetch_abort"]
    if args.dint:
        cmd += ["-d", "int"]
    env = dict(os.environ)
    if args.vmch_trace:
        env["VMCH_TRACE"] = os.path.join(scratch, "vmch-trace.log")
    with open(os.path.join(results, "command.txt"), "w") as f:
        f.write(" ".join(cmd) + "\n\n" + task)

    stdout = open(os.path.join(scratch, "qemu-stdout.log"), "w")
    proc = subprocess.Popen(cmd, stdout=stdout, stderr=subprocess.STDOUT,
                            env=env, cwd=scratch)
    start = time.time()
    status = "TIMEOUT"
    try:
        while time.time() - start < args.timeout:
            if os.path.exists(done):
                status = "DONE"
                time.sleep(args.settle)
                break
            if proc.poll() is not None:
                status = f"EXITED ({proc.returncode})"
                break
            time.sleep(1)
        elapsed = time.time() - start
        if proc.poll() is None:
            try:
                q = QMP(port)
                q.cmd("screendump", filename=os.path.join(results, "screen.png"),
                      format="png")
                regs = q.cmd("human-monitor-command",
                             **{"command-line": "info registers"})
                with open(os.path.join(results, "cpu-registers.txt"), "w") as f:
                    f.write(regs.get("return", str(regs)))
                q.cmd("quit")
                q.close()
            except (OSError, ConnectionError, ValueError) as e:
                print(f"  QMP: {e}")
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    finally:
        stdout.close()

    for f in glob.glob(os.path.join(vmt, "*,ffd")) + glob.glob(os.path.join(vmt, "*,fff")):
        base = os.path.basename(f).split(",")[0]
        if base != "VMTest":
            shutil.copyfile(f, os.path.join(results, base + ".txt"))
    for f in ("qemu-trace.log", "qemu-stdout.log", "vmch-trace.log"):
        if os.path.exists(os.path.join(scratch, f)):
            shutil.copyfile(os.path.join(scratch, f), os.path.join(results, f))
    total, svc_app = summarise_aborts(os.path.join(results, "qemu-trace.log"),
                                      os.path.join(results, "aborts.txt"))

    print(f"  status {status} after {elapsed:.0f}s; {total} guest aborts, "
          f"{svc_app} privileged aborts on application space")
    for f in ("log", "regs", "where"):
        p = os.path.join(results, f + ".txt")
        if os.path.exists(p):
            print(f"  --- {f}")
            for line in open(p, encoding="latin-1").read().replace("\r", "\n").splitlines():
                if line.strip():
                    print("   ", line)
    print(f"  results {results}")
    if args.keep:
        print(f"  kept {scratch}")
    else:
        shutil.rmtree(scratch, ignore_errors=True)
    return 0 if status == "DONE" else 1


if __name__ == "__main__":
    sys.exit(main())
