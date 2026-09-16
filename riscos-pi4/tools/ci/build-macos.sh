#!/bin/bash
# The macOS half of the compile gate: the README's build, from scratch,
# then the cheap static checks on the way past.  CI runs it on every
# push (.github/workflows/build.yml); it is the same script by hand.
#
# What it exists to catch is the class the week of 9-15 September
# landed more than once: code committed having compiled on one OS only.
# The metal front end and the POSIX sockets build here; dx11 does not,
# which is what the Windows twin is for.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BUILD="${CI_BUILD:-$ROOT/ci-build-macos}"

# The README's dependency list; brew is a no-op for what is poured.
brew install meson ninja pkgconf glib pixman capstone libslirp libpng

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
# The README's configure, with the front end forced on: a gate that
# silently skips the metal half because a header went missing is not
# a gate.
"$ROOT/configure" --target-list=aarch64-softmmu --enable-plugins \
    --disable-werror --disable-gtk --disable-sdl --disable-vnc \
    --disable-docs --disable-guest-agent --enable-capstone \
    --disable-spice --enable-slirp --enable-cocoa --enable-metal \
    --cc=/usr/bin/clang --cxx=/usr/bin/clang++ --objcc=/usr/bin/clang
ninja

# ---- static checks: everything the tree has that no compiler watches --

cd "$ROOT"

# The tools, byte-compiled.
python3 -m py_compile riscos-pi4/tools/*.py

# The shell scripts and launchers, syntax-checked by the shell their
# shebang names.
git ls-files 'riscos-pi4/**/*.sh' 'riscos-pi4/**/*.zsh' | while read -r f; do
    case "$(head -1 "$f")" in
    *zsh*) zsh -n "$f" ;;
    *)     bash -n "$f" ;;
    esac
    echo "syntax ok: $f"
done

# The blitter module, assembled for ARMv7 with clang's integrated
# assembler.  The other .s files (hostfs/dde/s, hostnet) are objasm
# syntax only the DDE reads, so they stay the DDE's business.
clang --target=armv7a-none-eabi -integrated-as -c -x assembler \
    -o /dev/null riscos-pi4/blitter/blitmod.s
echo "assembles: riscos-pi4/blitter/blitmod.s"

echo "macOS gate: all green"
