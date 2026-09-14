#!/usr/bin/env python3
"""Which ROM image and CMOS a launcher boots — the portable half.

The Python port of rom.zsh, which run-macos.sh and run-app.sh source.
run.py and farm.py import this, so a Windows machine and the farm start
the same way a Mac does, off the same mkrom.py and mkcmos.py.

    rom_to_boot(kernel, ...)   -> the image to pass to -kernel
    cmos_to_boot(cmos, ...)    -> the blob to pass to the loader

Kept deliberately close to rom.zsh: same defaults, same rule that a share
brings HostFS and its filer with it, same "remade every launch" for the
CMOS and same content-addressed cache for the ROM.  The one difference is
where output lands.  rom.zsh writes beside the stock image because a Mac
keeps both in one images directory; here the ROM, the CMOS and a farm
instance are three different places, so both functions take an explicit
out_dir.  That is not tidiness — four farm machines share one roms
directory, and a per-instance CMOS written to a shared path would be four
machines racing to write one file.
"""

import hashlib
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RISCOS_PI4 = os.path.dirname(HERE)

# The builds committed beside their sources, as rom.zsh defaults to.
DEFAULT_HOSTFS_MODULES = [
    os.path.join(RISCOS_PI4, "hostfs", "dde", "HostFS,ffa"),
    os.path.join(RISCOS_PI4, "hostfs", "filer", "HostFSFiler,ffa"),
]

# HostFS's filing system number, from hostfs/dde/s.head.
HOSTFS_FS_NUMBER = 220

# The boot screen.  BootFX keeps three files in ResourceFS, and mkrom.py -r
# overwrites them in place; app/bootfx holds the Acorn set that replaces
# ROOL's Raspberry Pi one.  Same defaults as rom.zsh, so a Windows machine
# and a Mac boot the same picture.
DEFAULT_BOOTFX = os.path.join(RISCOS_PI4, "app", "bootfx")
BOOTFX_FILES = ("1920x1080,c85", "Logo,c85", "Bar24,fca")


class RomError(Exception):
    """Anything that should stop a launch with a readable reason."""


def module_title(path):
    """The module's title, from the offset at +16 of its header."""
    with open(path, "rb") as fh:
        blob = fh.read()
    offset = struct.unpack_from("<I", blob, 16)[0]
    return blob[offset:blob.index(b"\0", offset)].decode("latin-1")


def _run(argv):
    result = subprocess.run(argv, capture_output=True, text=True)
    if result.returncode != 0:
        raise RomError((result.stderr or result.stdout).strip()
                       or " ".join(argv))
    return result.stdout


def bootfx_resources(bootfx):
    """The boot-screen files to overwrite, as (resource name, file) pairs.

    `bootfx` is a directory; None means the default set and "" means leave
    ROOL's picture alone.  A file that is not there is skipped rather than
    an error, so a partial set still works.
    """
    if bootfx == "":
        return []
    where = DEFAULT_BOOTFX if bootfx is None else bootfx
    pairs = []
    for name in BOOTFX_FILES:
        path = os.path.join(where, name)
        if os.path.exists(path):
            pairs.append((f"Resources.BootFX.{name.split(',')[0]}", path))
    return pairs


def rom_to_boot(kernel, modules=(), hostfs=None, hostfs_modules=None,
                out_dir=None, log=print, bootfx=None):
    """The image to boot: the stock one, or one with modules spliced in.

    modules         files spliced into the ROM, in initialisation order
    hostfs          a share.  A share brings HostFS and its icon-bar filer
                    with it, ahead of `modules`, so no machine starts with
                    a share and no filing system to reach it
    hostfs_modules  override those builds; [] splices none.  A module whose
                    title is already in `modules` stands instead
    """
    modules = list(modules)
    for path in modules:
        if not os.path.exists(path):
            raise RomError(f"module: missing: {path}")

    if hostfs:
        extra = (DEFAULT_HOSTFS_MODULES if hostfs_modules is None
                 else list(hostfs_modules))
        titles = {module_title(m) for m in modules}
        add = []
        for path in extra:
            if not os.path.exists(path):
                raise RomError(f"hostfs module: missing: {path}")
            if module_title(path) not in titles:
                add.append(path)
        modules = add + modules

    resources = bootfx_resources(bootfx)
    if not modules and not resources:
        return kernel

    # Cached under a hash of the stock ROM and every module's contents, so
    # a module edit rebuilds it and a relaunch does not.
    digest = hashlib.sha256()
    for path in [kernel] + modules + [f for _, f in resources]:
        with open(path, "rb") as fh:
            digest.update(hashlib.sha256(fh.read()).digest())
    key = digest.hexdigest()[:16]

    out_dir = out_dir or os.path.dirname(os.path.abspath(kernel))
    out = os.path.join(out_dir, f"RISCOS-{key}.IMG")
    what = f"{len(modules)} module(s)" + (", the boot screen"
                                          if resources else "")
    if os.path.exists(out):
        log(f"rom: cached {os.path.basename(out)} ({what})")
        return out

    log(f"rom: splicing {what} -> {os.path.basename(out)}")
    # Written aside and renamed, because four farm machines starting at once
    # compute the same key and would otherwise write the same file together.
    tmp = f"{out}.{os.getpid()}.tmp"
    argv = [sys.executable, os.path.join(HERE, "mkrom.py"), kernel,
            "-o", tmp]
    for path in modules:
        argv += ["-m", path]
    for name, path in resources:
        argv += ["-r", f"{name}={path}"]
    try:
        _run(argv)
        os.replace(tmp, out)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    return out


def cmos_to_boot(cmos, boot=None, hostfs=None, out_dir=None, log=print):
    """The CMOS blob to load: the stock one, or one that boots the share.

    boot    "hostfs" boots from the share; None boots as `cmos` says.  A
            card stays attached either way, and reachable as SDFS::0.

    The blob comes from the share's own CMOS,ff2 — which HostFS rewrites
    after every *Configure, as SDCMOS does on a card — or, the first time,
    from `cmos`, and the share is seeded with it.  Either way FileSystem
    HostFS is forced into it, so the launch option always wins.
    """
    if not boot:
        return cmos
    if boot != "hostfs":
        raise RomError(f"boot: {boot!r} is not a boot source "
                       "(hostfs, or unset)")
    if not hostfs:
        raise RomError("boot=hostfs: a share is needed to boot from one")

    saved = None
    for name in ("CMOS,ff2", "CMOS,fe4"):
        candidate = os.path.join(hostfs, name)
        if os.path.exists(candidate):
            saved = candidate
            break

    out_dir = out_dir or os.path.dirname(os.path.abspath(cmos))
    out = os.path.join(out_dir, "cmos-hostfs.bin")
    # Remade every launch: it is cheap, and the share may have changed.
    _run([sys.executable, os.path.join(HERE, "mkcmos.py"),
          "--symbols", os.path.join(HERE, "cmos-symbols-530.json"),
          "--base", saved or cmos,
          "--filesystem", str(HOSTFS_FS_NUMBER),
          "-o", out])

    if saved:
        log(f"cmos: from the share's {os.path.basename(saved)}")
    else:
        shutil.copyfile(out, os.path.join(hostfs, "CMOS,ff2"))
        log("cmos: FileSystem HostFS; the share now keeps it, as CMOS,ff2")
    return out
