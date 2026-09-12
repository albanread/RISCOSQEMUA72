#!/bin/bash
# Build the QEMU dependencies that Homebrew cannot provide on Intel
# macOS 26 (Tahoe), into a prefix outside the repositories.
#
# Why this exists: on this platform brew has no pourable bottles for
# pcre2, glib, libslirp or capstone, and it refuses every source build
# while an outdated Xcode sits in /Applications — even with
# DEVELOPER_DIR pointed at the Command Line Tools, because brew's
# Xcode detection falls back to the default bundle path when
# xcode-select names the CLT (Homebrew os/mac/xcode.rb, `prefix`).
# The CLT itself is a complete toolchain, so we build with it here and
# leave brew to pour what it can: meson, ninja, pkg-config, libpng,
# pixman, gettext, libffi.
#
# Everything is built static: the emulator executable carries no
# dependency on the prefix, so the .app bundle stays self-contained
# next to the brew .dylibs it already ships beside.
#
# Idempotent: a package whose stamp file exists is skipped.  Versions
# are the ones Homebrew itself pours for the rest, so the set stays
# coherent; override with e.g. GLIB_VERSION=2.88.3 ./build-deps-macos.sh
# and clean a package with `rm -rf deps-macos-intel/stamps/<name>`.
set -euo pipefail

PCRE2_VERSION=${PCRE2_VERSION:-10.48}
GLIB_VERSION=${GLIB_VERSION:-2.88.3}
LIBSLIRP_VERSION=${LIBSLIRP_VERSION:-4.9.4}
CAPSTONE_VERSION=${CAPSTONE_VERSION:-5.0.9}

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PREFIX=${RISCOS_DEPS_PREFIX:-"$ROOT/../deps-macos-intel"}
SRC="$PREFIX/src"
STAMPS="$PREFIX/stamps"
mkdir -p "$SRC" "$STAMPS" "$PREFIX/lib/pkgconfig"

# The CLT toolchain, not whatever xcode-select names (see header).
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Library/Developer/CommandLineTools}
CC=/usr/bin/clang
CXX=/usr/bin/clang++

# brew's keg-only gettext (headers, static libintl) has no .pc file;
# libffi has one.  Both are needed by glib.
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:/usr/local/opt/libffi/lib/pkgconfig"
export CFLAGS="-O2 -I$PREFIX/include -I/usr/local/opt/gettext/include"
export LDFLAGS="-L$PREFIX/lib -L/usr/local/opt/gettext/lib"

stamp() { [ -f "$STAMPS/$1" ]; }
done_pkg() { touch "$STAMPS/$1"; }
note() { printf '\n=== %s ===\n' "$1"; }

fetch() { # fetch <url> <tarball-name>
    local url=$1 name=$2
    [ -f "$SRC/$name" ] && return 0
    curl -L --fail --retry 3 -o "$SRC/$name.part" "$url"
    mv "$SRC/$name.part" "$SRC/$name"
}
unpack() { # unpack <tarball> <dir-inside>
    local dir
    dir=$(basename "${2%/}")
    [ -d "$SRC/$dir" ] && return 0
    tar -xf "$SRC/$1" -C "$SRC"
}

# ---------------------------------------------------------------- pcre2
if ! stamp pcre2; then
    note "pcre2 $PCRE2_VERSION (cmake, static)"
    fetch "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-$PCRE2_VERSION/pcre2-$PCRE2_VERSION.tar.gz" \
          "pcre2-$PCRE2_VERSION.tar.gz"
    unpack "pcre2-$PCRE2_VERSION.tar.gz" "pcre2-$PCRE2_VERSION"
    cmake -S "$SRC/pcre2-$PCRE2_VERSION" -B "$SRC/pcre2-build" \
          -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_C_COMPILER="$CC" -DCMAKE_OSX_ARCHITECTURES=x86_64 \
          -DCMAKE_INSTALL_LIBDIR=lib \
          -DBUILD_SHARED_LIBS=OFF \
          -DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2_16=OFF -DPCRE2_BUILD_PCRE2_32=OFF \
          -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_BUILD_TESTS=OFF
    cmake --build "$SRC/pcre2-build" --parallel
    cmake --install "$SRC/pcre2-build"
    done_pkg pcre2
fi

# ----------------------------------------------------------------- glib
if ! stamp glib; then
    note "glib $GLIB_VERSION (meson, static)"
    fetch "https://download.gnome.org/sources/glib/${GLIB_VERSION%.*}/glib-$GLIB_VERSION.tar.xz" \
          "glib-$GLIB_VERSION.tar.xz"
    unpack "glib-$GLIB_VERSION.tar.xz" "glib-$GLIB_VERSION"
    meson setup "$SRC/glib-build" "$SRC/glib-$GLIB_VERSION" \
          --prefix="$PREFIX" --buildtype=release --default-library=static \
          -Dtests=false -Dnls=auto
    ninja -C "$SRC/glib-build"
    meson install -C "$SRC/glib-build"
    done_pkg glib
fi

# ------------------------------------------------------------- libslirp
if ! stamp libslirp; then
    note "libslirp $LIBSLIRP_VERSION (meson, static)"
    fetch "https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v$LIBSLIRP_VERSION/libslirp-v$LIBSLIRP_VERSION.tar.gz" \
          "libslirp-v$LIBSLIRP_VERSION.tar.gz"
    unpack "libslirp-v$LIBSLIRP_VERSION.tar.gz" "libslirp-v$LIBSLIRP_VERSION"
    meson setup "$SRC/libslirp-build" "$SRC/libslirp-v$LIBSLIRP_VERSION" \
          --prefix="$PREFIX" --buildtype=release --default-library=static
    ninja -C "$SRC/libslirp-build"
    meson install -C "$SRC/libslirp-build"
    done_pkg libslirp
fi

# ------------------------------------------------------------- capstone
if ! stamp capstone; then
    note "capstone $CAPSTONE_VERSION (cmake, static — capstone 5 dropped meson)"
    fetch "https://github.com/capstone-engine/capstone/releases/download/$CAPSTONE_VERSION/capstone-$CAPSTONE_VERSION.tar.xz" \
          "capstone-$CAPSTONE_VERSION.tar.xz"
    unpack "capstone-$CAPSTONE_VERSION.tar.xz" "capstone-$CAPSTONE_VERSION"
    cmake -S "$SRC/capstone-$CAPSTONE_VERSION" -B "$SRC/capstone-build" \
          -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_C_COMPILER="$CC" -DCMAKE_OSX_ARCHITECTURES=x86_64 \
          -DCMAKE_INSTALL_LIBDIR=lib \
          -DBUILD_SHARED_LIBS=OFF \
          -DCAPSTONE_BUILD_TESTS=OFF -DCAPSTONE_BUILD_CSTOOL=OFF
    cmake --build "$SRC/capstone-build" --parallel
    cmake --install "$SRC/capstone-build"
    done_pkg capstone
fi

# ---------------------------------------------------------------- report
note "deps prefix ready: $PREFIX"
for pc in libpcre2-8 glib-2.0 slirp capstone; do
    printf '%-12s %s\n' "$pc" "$(pkg-config --modversion "$pc")"
done
cat <<EOS

Point QEMU's configure at it:

  export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:/usr/local/opt/libffi/lib/pkgconfig"

(the build-macos/ configure invocation in MACOS.md does this when
RISCOS_DEPS_PREFIX is not already in PKG_CONFIG_PATH).
EOS
