#!/bin/bash
# The Windows half of the compile gate.  Run inside an MSYS2 MINGW64
# shell with the README's packages installed; CI installs them
# (.github/workflows/build.yml), a machine that already builds the
# emulator has them, and this script does not pacman anything so it
# stays runnable offline of that.
#
# dx11 is forced on for the same reason metal is on the Mac: this leg
# exists to compile ui/dx11.cpp, and a gate that quietly skipped it
# because a header went missing would prove nothing.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BUILD="${CI_BUILD:-$ROOT/ci-build-windows}"

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
"$ROOT/configure" --target-list=aarch64-softmmu --enable-plugins \
    --disable-werror --disable-gtk --disable-sdl --disable-vnc \
    --disable-docs --disable-guest-agent --enable-capstone \
    --disable-spice --enable-slirp --enable-dx11
ninja

# ---- static checks: the ones that need no macOS tools -----------------

cd "$ROOT"

python -m py_compile riscos-pi4/tools/*.py

git ls-files 'riscos-pi4/**/*.sh' 'riscos-pi4/**/*.zsh' | while read -r f; do
    case "$(head -1 "$f")" in
    # zsh scripts are the macOS gate's to check: MSYS2 has no zsh, and
    # parsing them with bash proves nothing either way.
    *zsh*) echo "skipped (zsh; the macOS gate checks it): $f" ;;
    *)     bash -n "$f"; echo "syntax ok: $f" ;;
    esac
done

echo "Windows gate: all green"
