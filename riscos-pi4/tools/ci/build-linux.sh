#!/bin/bash
# The Linux half of the compile gate: a GTK build, the front end
# neither other leg compiles.  CI runs it on every push
# (.github/workflows/build.yml); by hand it is the same script, apt
# being a no-op for what is installed.
#
# Linux is where the generic POSIX surface gets built -- everything
# that is macOS's here and Windows' there, and nothing that is only
# the DDE's.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BUILD="${CI_BUILD:-$ROOT/ci-build-linux}"

sudo apt-get update -qq
sudo apt-get install -y -qq \
    ninja-build meson python3 python3-venv python3-pip pkg-config flex bison \
    libglib2.0-dev libpixman-1-dev zlib1g-dev libcapstone-dev \
    libslirp-dev libpng-dev libgtk-3-dev zsh

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
# The README's configure with the GTK front end on instead of off:
# this leg exists to compile it.
"$ROOT/configure" --target-list=aarch64-softmmu --enable-plugins \
    --disable-werror --disable-sdl --disable-vnc --disable-docs \
    --disable-guest-agent --enable-capstone --disable-spice \
    --enable-slirp --enable-gtk
ninja

# ---- static checks: the same pass as the macOS gate --------------------

cd "$ROOT"

python3 -m py_compile riscos-pi4/tools/*.py

git ls-files 'riscos-pi4/**/*.sh' 'riscos-pi4/**/*.zsh' | while read -r f; do
    case "$(head -1 "$f")" in
    *zsh*) zsh -n "$f" ;;
    *)     bash -n "$f" ;;
    esac
    echo "syntax ok: $f"
done

clang --target=armv7a-none-eabi -integrated-as -c -x assembler \
    -o /dev/null riscos-pi4/blitter/blitmod.s
echo "assembles: riscos-pi4/blitter/blitmod.s"

echo "Linux gate: all green"
