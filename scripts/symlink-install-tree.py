#!/usr/bin/env python3

from pathlib import PurePath
import errno
import json
import os
import shlex
import subprocess
import sys

def destdir_join(d1: str, d2: str) -> str:
    if not d1:
        return d2
    # c:\destdir + c:\prefix must produce c:\destdir\prefix
    return str(PurePath(d1, *PurePath(d2).parts[1:]))

introspect = os.environ.get('MESONINTROSPECT')
out = subprocess.run([*shlex.split(introspect), '--installed'],
                     stdout=subprocess.PIPE, check=True).stdout
skip_links = False
for source, dest in json.loads(out).items():
    bundle_dest = destdir_join('qemu-bundle', dest)
    path = os.path.dirname(bundle_dest)
    try:
        os.makedirs(path, exist_ok=True)
    except BaseException as e:
        print(f'error making directory {path}', file=sys.stderr)
        raise e
    if skip_links:
        continue
    try:
        os.symlink(source, bundle_dest)
    except BaseException as e:
        if isinstance(e, OSError) and e.errno == errno.EEXIST:
            continue
        if os.name == 'nt' and isinstance(e, OSError):
            #
            # Creating a symbolic link on Windows needs either Developer Mode
            # or Administrator rights, and these links point at build outputs
            # that do not exist yet, so a copy cannot stand in for them.
            #
            # qemu-bundle only exists so that binaries run from the build tree
            # can find their data files; without it, pass -L <srcdir>/pc-bios.
            # That is a fair trade against requiring elevation to build.
            #
            print('warning: cannot create symbolic links (enable Developer '
                  'Mode to fix); skipping qemu-bundle. Run QEMU from the '
                  'build tree with -L <srcdir>/pc-bios if it needs a blob.',
                  file=sys.stderr)
            skip_links = True
            continue
        print(f'error making symbolic link {dest}', file=sys.stderr)
        raise e
