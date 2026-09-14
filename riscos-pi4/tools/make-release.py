#!/usr/bin/env python3
"""make-release.py -- the end-user Windows release: one setup program.

    make-release.py [N]          N is the release number (default 1)

Builds, under build/release:

    <NAME>v<N>-setup.exe   the installer: creates the user's folder,
                           unpacks the reduced guest into it, installs the
                           emulator and a desktop shortcut, per user and
                           with no administrator rights
    stage/                 what went into it, for inspection

The Windows half of make-release.sh.  That script builds a self-contained
.app; Windows has no equivalent container, so the same pieces are laid out
as a program directory plus a user data directory, and an Inno Setup
script ties them together:

    %LOCALAPPDATA%\\Programs\\<NAME>v<N>    the emulator, its DLLs, the ROM
                                           and the launcher
    %USERPROFILE%\\<NAME>                   the guest's disc, which is also
                                           its HostFS share

The disc is deliberately somewhere the user can find: the share *is* the
machine's filing system, so getting files in and out means opening that
folder.  Hiding it under AppData would make the one thing they need to
reach the hardest to find.

Needs Inno Setup 6 (ISCC.exe) for the last step; everything before it
runs without.
"""

import argparse
import fnmatch
import io
import os
import re
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rom                                             # noqa: E402

ROOT = os.path.dirname(HERE)                           # riscos-pi4/
REPO = os.path.dirname(ROOT)

DEFAULTS = {
    "name": "RISCOSQEA72",
    "qemu": os.path.join(REPO, "build", "qemu-system-aarch64.exe"),
    "rom": r"F:\RISCOSDEV\roms\pi\v530\RISCOS.IMG",
    "fs_zip": r"F:\RISCOSDEV\ROS_PRIVATE\dist\end_user_fs.zip",
    "out": os.path.join(REPO, "build", "release"),
    "mingw": r"F:\RISCOSDEV\msys64\mingw64\bin",
}

# What to leave off the disc.  Kept in step with make-release.sh's
# STRIP_DEFAULT: the DDE above all, which is licensed to us and not ours
# to hand out, then everything that is not the desktop and its
# applications.
STRIP_DEFAULT = [
    "Apps/DDE", "Documents/DDE",
    "Apps/!Store", "!Boot/Choices/PlingStore", "Apps/!PackMan",
    "!Boot/Loader,fc8",
    "!Boot/Resources/!Ghostscr",
    "!Boot/Resources/!ThemeDefs/Themes/Iyonix",
    "!Boot/Resources/!ThemeDefs/Themes/Sovereign",
    "!Boot/Resources/!ThemeDefs/Themes/Raspberry",
    "!Boot/Resources/!ThemeDefs/Themes/Ursula",
    "!Boot/Resources/!ThemeDefs/Themes/Morris4",
    "Documents/Books", "Documents/Manuals", "Documents/Images",
    "Documents/OvationPro", "Documents/UserGuide", "Documents/PipeDream",
    "Documents/Other", "Documents/Music",
    "Diversions",
    "Utilities/!DPlngScan",
]


def log(msg):
    print(msg, flush=True)


def step(msg):
    print(f"\n== {msg}", flush=True)


def tree_size(path):
    n = b = 0
    for root, _, files in os.walk(path):
        for f in files:
            try:
                b += os.path.getsize(os.path.join(root, f))
                n += 1
            except OSError:
                pass
    return n, b


# ------------------------------------------------------------------ disc

def build_disc(fs_zip, dest, strip):
    """Unpack the end-user disc and cut it down.

    Python's zipfile rather than tar: GNU tar reads "F:/..." as a remote
    host, and zipfile also lets the names be checked first.
    """
    step(f"disc: unpacking {os.path.basename(fs_zip)}")
    os.makedirs(dest, exist_ok=True)
    with zipfile.ZipFile(fs_zip) as z:
        names = z.namelist()
        bad = [n for n in names
               if any(c in n.split("/")[-1] for c in '<>:"|?*')]
        if bad:
            # The zip is built on a Mac, where these names are legal.  On
            # NTFS they are not, and extraction would fail partway --
            # which is ROS_PRIVATE#6 arriving by a different road.
            raise SystemExit(
                f"make-release: {len(bad)} name(s) in the zip cannot be "
                f"stored on this host, e.g. {bad[0]}")
        z.extractall(dest)
    n, b = tree_size(dest)
    log(f"   {n:,} files, {b / 1e6:.1f} MB")

    step("disc: stripping what the user release does not ship")
    removed = 0
    for pattern in strip:
        for root, dirs, files in os.walk(dest):
            rel = os.path.relpath(root, dest).replace(os.sep, "/")
            rel = "" if rel == "." else rel + "/"
            for name in list(dirs) + list(files):
                if fnmatch.fnmatch(rel + name, pattern):
                    target = os.path.join(root, name)
                    if os.path.isdir(target):
                        shutil.rmtree(target, ignore_errors=True)
                        if name in dirs:
                            dirs.remove(name)
                    else:
                        os.remove(target)
                    removed += 1
    n, b = tree_size(dest)
    log(f"   removed {removed} path(s) -> {n:,} files, {b / 1e6:.1f} MB")

    # The settings the machine boots from.  The launcher hands this file to
    # the loader directly and RISC OS writes back to it through HostFS, so
    # *Configure survives a restart -- which is why FileSystem is forced
    # here, once, rather than regenerated at every launch as rom.zsh does
    # on the Mac.  There is no Python on the user's machine to do that.
    step("disc: settings, with FileSystem HostFS")
    cmos = os.path.join(dest, "CMOS,ff2")
    base = cmos if os.path.exists(cmos) else os.path.join(
        os.path.dirname(DEFAULTS["rom"]), "..", "rool-cmos-unplug.bin")
    subprocess.run([sys.executable, os.path.join(HERE, "mkcmos.py"),
                    "--symbols", os.path.join(HERE, "cmos-symbols-530.json"),
                    "--base", os.path.abspath(base),
                    "--filesystem", str(rom.HOSTFS_FS_NUMBER),
                    "-o", cmos], check=True, capture_output=True)
    log(f"   CMOS,ff2 {os.path.getsize(cmos)} bytes")
    return n, b



# ---------------------------------------------------------------- network

# The guest side of slirp, fixed.  slirp is always 10.0.2.0/24 with the host
# at .2, so there is nothing for DHCP to discover -- and RISC OS rejects
# slirp's offer anyway: it retries four times over 20 seconds, then falls
# back to a 169.254 link-local address with no gateway and no resolver,
# which is why name lookups failed.  Configuring the interface directly
# removes the wait and the fallback both.  Measured: 49s to the desktop
# before, 28s after, and no DHCP packet on the wire at all.
GUEST_IP, GUEST_MASK, GUEST_GW = "10.0.2.15", "255.255.255.0", "10.0.2.2"
GUEST_DNS = "208.67.222.222 208.67.220.220"      # OpenDNS
GUEST_DOMAIN = "lan"


def configure_network(disc):
    """Point Choices:Internet at slirp, statically."""
    step("disc: network, static against slirp")
    ch = os.path.join(disc, "!Boot", "Choices", "Internet")
    startup = os.path.join(ch, "Startup,feb")
    user = os.path.join(ch, "User,feb")
    if not os.path.exists(startup):
        log("   no Internet choices in this disc, skipped")
        return

    text = io.open(startup, encoding="latin-1", newline="").read()
    if "DHCPExecute" in text:
        text = text.replace("Set Inet$EtherIPAddr dhcp",
                            f"Set Inet$EtherIPAddr {GUEST_IP}")
        text = text.replace("Set Inet$EtherIPMask default",
                            f"Set Inet$EtherIPMask {GUEST_MASK}")
        old = ('IF "<Wimp$State>" = "commands" THEN Echo Contacting DHCP '
               'server for Ethernet over USB interface\n'
               'DHCPExecute -e -b -w -p ej0\n'
               'CheckError\n'
               'If "<Inet$Gateway>" <> "" Then do /Inet:bin.route -e add '
               'default <Inet$Gateway>\n'
               'CheckError\n')
        new = (f'IfConfig -e ej0 {GUEST_IP} netmask {GUEST_MASK}\n'
               'CheckError\n'
               f'Set Inet$Gateway {GUEST_GW}\n'
               f'do /Inet:bin.route -e add default {GUEST_GW}\n'
               'CheckError\n')
        if old in text:
            text = text.replace(old, new, 1)
            io.open(startup, "w", encoding="latin-1", newline="").write(text)
            log(f"   {GUEST_IP}/{GUEST_MASK} via {GUEST_GW}, no DHCP")
        else:
            log("   WARNING: DHCP block not in the expected form, left alone")
    else:
        log("   already static")

    u = io.open(user, encoding="latin-1", newline="").read()
    if "Inet$Resolvers" in u:
        u = re.sub(r"Set Inet\$Resolvers .*",
                   f"Set Inet$Resolvers {GUEST_DNS}", u, count=1)
    else:
        anchor = "Set Inet$ResolverRetries 3\n"
        if anchor not in u:
            log("   WARNING: could not place the resolver setting")
            return
        u = u.replace(anchor, anchor +
                      f"Set Inet$Resolvers {GUEST_DNS}\n"
                      f"Set Inet$LocalDomain {GUEST_DOMAIN}\n", 1)
    io.open(user, "w", encoding="latin-1", newline="").write(u)
    log(f"   resolvers {GUEST_DNS}")


# ------------------------------------------------------------------- app

def build_app(args, app_dir):
    """The program directory: emulator, its DLLs, the ROM, the launcher."""
    os.makedirs(app_dir, exist_ok=True)

    step("app: the emulator and the DLLs it links")
    shutil.copy2(args.qemu, app_dir)
    dlls = mingw_dlls(args.qemu, args.mingw)
    for dll in dlls:
        shutil.copy2(dll, app_dir)
    log(f"   qemu-system-aarch64.exe + {len(dlls)} DLL(s)")

    step("app: the ROM, with HostFS, its filer and the boot screen")
    # bootfx defaults to app/bootfx, so the release boots the Acorn screen
    # exactly as the Mac release does.
    spliced = rom.rom_to_boot(args.rom, hostfs="release", out_dir=app_dir,
                              log=lambda m: log("   " + m))
    final = os.path.join(app_dir, "RISCOS.IMG")
    if os.path.abspath(spliced) != os.path.abspath(final):
        shutil.move(spliced, final)
    log(f"   RISCOS.IMG {os.path.getsize(final) / 1e6:.1f} MB")
    return final


def mingw_dlls(exe, mingw_bin):
    """Every MinGW DLL the emulator links, found with ldd under MSYS2."""
    bash = r"F:\RISCOSDEV\msys64\usr\bin\bash.exe"
    if not os.path.exists(bash):
        raise SystemExit("make-release: MSYS2 bash not found, cannot list DLLs")
    posix = exe.replace("\\", "/")
    if posix[1:2] == ":":
        posix = "/" + posix[0].lower() + posix[2:]
    out = subprocess.run(
        [bash, "-lc", f"ldd '{posix}'"],
        capture_output=True, text=True,
        env={**os.environ, "MSYSTEM": "MINGW64"}).stdout
    found = []
    for line in out.splitlines():
        if "=>" not in line:
            continue
        path = line.split("=>", 1)[1].strip().split(" (")[0]
        if "/mingw64/" not in path.replace("\\", "/"):
            continue
        found.append(os.path.join(mingw_bin, os.path.basename(path)))
    return sorted(set(found))


# -------------------------------------------------------------- launcher

def build_launcher(app_dir, name):
    """Compile the launcher stub.

    A .cmd would flash a console window and look like a script; the Mac
    release compiles a stub for the same reason.  This one is a GUI-subsystem
    exe, so nothing appears but the emulator's own window.
    """
    step("launcher: compiling the stub")
    src = os.path.join(ROOT, "app", "win", "launcher.c")
    exe = os.path.join(app_dir, f"{name}.exe")
    gcc = os.path.join(DEFAULTS["mingw"], "gcc.exe")
    env = {**os.environ,
           "PATH": DEFAULTS["mingw"] + os.pathsep + os.environ.get("PATH", "")}
    # -mwindows: no console window behind the emulator's own.
    # -municode: wWinMain, so the paths are wide characters throughout.
    subprocess.run([gcc, "-O2", "-mwindows", "-municode",
                    f'-DAPP_NAME=L"{name}"', src,
                    "-o", exe, "-lshell32"], check=True, env=env)
    log(f"   {os.path.basename(exe)} {os.path.getsize(exe) / 1024:.0f} KB")
    return exe


# ------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("release", nargs="?", type=int, default=1)
    ap.add_argument("--name", default=DEFAULTS["name"])
    ap.add_argument("--qemu", default=DEFAULTS["qemu"])
    ap.add_argument("--rom", default=DEFAULTS["rom"])
    ap.add_argument("--fs-zip", default=DEFAULTS["fs_zip"])
    ap.add_argument("--out", default=DEFAULTS["out"])
    ap.add_argument("--mingw", default=DEFAULTS["mingw"])
    ap.add_argument("--no-strip", action="store_true",
                    help="ship the disc as it is")
    ap.add_argument("--stage-only", action="store_true",
                    help="lay the pieces out, do not run Inno Setup")
    args = ap.parse_args()

    full = f"{args.name}v{args.release}"
    stage = os.path.join(args.out, "stage")
    app_dir = os.path.join(stage, "app")
    disc_dir = os.path.join(stage, "disc")

    if os.path.exists(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)

    build_app(args, app_dir)
    build_launcher(app_dir, args.name)
    files, size = build_disc(args.fs_zip, disc_dir,
                             [] if args.no_strip else STRIP_DEFAULT)
    configure_network(disc_dir)

    if args.stage_only:
        step("staged, not packaged (--stage-only)")
        return 0

    setup = compile_installer(args, full, stage, app_dir, disc_dir)
    step("done")
    log(f"   {setup}")
    log(f"   {os.path.getsize(setup) / 1e6:.1f} MB, disc {files:,} files "
        f"/ {size / 1e6:.1f} MB")
    return 0


ISCC_CANDIDATES = [
    os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs",
                 "Inno Setup 6", "ISCC.exe"),
    r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
    r"C:\Program Files\Inno Setup 6\ISCC.exe",
]


def find_iscc():
    for path in ISCC_CANDIDATES:
        if path and os.path.exists(path):
            return path
    raise SystemExit(
        "make-release: Inno Setup 6 not found. winget install "
        "--id JRSoftware.InnoSetup --scope user")


def compile_installer(args, full, stage, app_dir, disc_dir):
    """Substitute the .iss template and run the Inno compiler."""
    step("installer: compiling with Inno Setup")
    iscc = find_iscc()
    template = os.path.join(ROOT, "app", "win", "setup.iss.in")
    with open(template, encoding="utf-8") as fh:
        text = fh.read()

    _, discbytes = tree_size(disc_dir)
    # A stable AppId keeps an upgrade replacing the old entry rather than
    # adding a second one to Add/Remove Programs.
    fields = {
        "@NAME@": args.name,
        "@DISPLAY@": "RISC OS on QEMU (A72)",
        "@VERSION@": str(args.release),
        "@PUBLISHER@": "RISC OS QEMU project",
        "@APPID@": "8E5B1C64-7B4E-4C1E-9F3A-RISCOSQEA72",
        "@OUTDIR@": os.path.abspath(args.out),
        "@STAGE@": os.path.abspath(stage),
        "@DISCBYTES@": str(discbytes),
    }
    for key, value in fields.items():
        text = text.replace(key, value)

    iss = os.path.join(stage, "setup.iss")
    with open(iss, "w", encoding="utf-8") as fh:
        fh.write(text)

    result = subprocess.run([iscc, iss], capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout[-3000:] + result.stderr[-2000:])
        raise SystemExit("make-release: Inno Setup failed")
    return os.path.join(args.out, f"{full}-setup.exe")


if __name__ == "__main__":
    sys.exit(main())
