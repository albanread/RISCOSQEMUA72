#!/usr/bin/env python3
"""Boot a RISC OS Pi ROM under QEMU and report where it gets to.

    python probe.py <machine> <riscos.img> [seconds] [-- extra qemu args]

    python probe.py raspi4b RISCOS.IMG 30 -- -cpu cortex-a72,aarch64=off
    python probe.py raspi2b RISCOS.IMG 30

Samples the PC six times over the run, writes a PNG screendump next to the ROM,
and dumps registers and a disassembly at the final PC. QEMU's own log (with
unimplemented-device and guest-error reporting) lands in probe-<machine>.log.

See docs/PI4-QEMU.md. QEMU dir overridable with the QEMU_DIR environment
variable; default is the user-local install used in that document.
"""
import json
import os
import re
import socket
import subprocess
import sys
import time

QEMU_DIR = os.environ.get(
    "QEMU_DIR", os.path.expandvars(r"%LOCALAPPDATA%\Programs\qemu"))
PORT = int(os.environ.get("QEMU_QMP_PORT", "4460"))
# 32-bit-only machines live in qemu-system-arm; the rest need the aarch64 binary
ARM32_MACHINES = ("raspi0", "raspi1ap", "raspi2b")


def main():
    argv = sys.argv[1:]
    extra = []
    if "--" in argv:
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    if len(argv) < 2:
        sys.exit(__doc__)
    mach, rom = argv[0], os.path.abspath(argv[1])
    wait = float(argv[2]) if len(argv) > 2 else 30.0
    out = os.path.dirname(rom) or "."

    binary = ("qemu-system-arm.exe" if mach in ARM32_MACHINES
              else "qemu-system-aarch64.exe")
    log = os.path.join(out, f"probe-{mach}.log")
    cmd = [os.path.join(QEMU_DIR, binary), "-M", mach, "-kernel", rom,
           "-display", "none", "-serial", "null",
           "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait",
           "-d", "unimp,guest_errors", "-D", log] + extra
    print(" ".join(cmd), flush=True)
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True)
    try:
        time.sleep(2)
        s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
        f = s.makefile("rw", encoding="utf-8", newline="\n")
        f.readline()                                   # QMP greeting

        def qmp(name, **args):
            req = {"execute": name}
            if args:
                req["arguments"] = args
            f.write(json.dumps(req) + "\n")
            f.flush()
            while True:
                line = f.readline()
                if not line:
                    return None
                o = json.loads(line)
                if "return" in o or "error" in o:
                    return o

        def hmp(text):
            return (qmp("human-monitor-command",
                        **{"command-line": text}) or {}).get("return", "")

        qmp("qmp_capabilities")
        pcs = []
        for i in range(6):
            time.sleep(wait / 6)
            regs = hmp("info registers")
            m = re.search(r"R15=([0-9a-f]{8})", regs)
            pcs.append(m.group(1) if m else "?")
            mode = re.search(r"PSR=\S+\s+(.*)", regs)
            print(f"t+{(i + 1) * wait / 6:5.0f}s  PC={pcs[-1]}  "
                  f"{mode.group(1).strip()[:40] if mode else ''}", flush=True)
        print(f"distinct PCs: {len(set(pcs))}", flush=True)

        png = os.path.join(out, f"shot-{mach}.png")
        qmp("screendump", filename=png, format="png")
        print(f"screendump -> {png}", flush=True)
        print("== registers ==\n" + hmp("info registers")[:500], flush=True)
        if pcs[-1] != "?":
            print(f"== disassembly at 0x{pcs[-1]} ==\n"
                  + hmp(f"x/16i 0x{pcs[-1]}")[:900], flush=True)
    finally:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()


if __name__ == "__main__":
    main()
