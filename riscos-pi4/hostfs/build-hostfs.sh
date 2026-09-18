#!/bin/sh
# Build HostFS and HostFSFiler on the host: no DDE, no emulator, no guest.
#
# The DDE build (dde/Build,feb and filer/Build,feb) is the one that has
# shipped: objasm, Norcroft cc and link -rmf, run inside RISC OS on a share.
# It works, and it needs a licensed toolchain and a booted machine to run a
# compiler in.  This is the same two source files through a toolchain that
# runs here:
#
#     s.head      -> rosasm --elf   (ObjAsm, assembled by rosasm, as ELF)
#     c.hostfs    -> clang          (freestanding ARM, no library at all)
#     the two     -> roscc --module (linked at base 0 into a ,ffa)
#
# Why each piece is what it is:
#
#   rosasm speaks ObjAsm, so s.head is the file the DDE builds, unedited.
#   It writes AOF by default, which the RISC OS linker reads and roscc does
#   not; --elf writes the same object in the container roscc reads, and
#   translates the one name that differs -- ObjAsm's |!!!Module$$Header| is
#   ELF's .module, and each toolchain insists on its own spelling.
#
#   clang needs -fropi because this module runs from ROM.  A module on the
#   ROM chain executes in place, out of read-only memory, so an image that
#   holds an absolute address cannot be fixed up: the linker's relocation
#   code would have to write into the image, and the first store is a data
#   abort (see the 17 Sep case -- HostFS 1.01 in a ROM did exactly that).
#   -fropi reaches the string literals pc-relatively instead, which is what
#   the DDE build gets from its own flags and checks with `decaof -r`.
#
#   The check below is that check, made part of the build: any absolute
#   relocation in either object and nothing is written.  A module that will
#   not run from ROM should fail here, not three minutes into a boot with a
#   black screen.
#
# Output: dde/HostFS,ffa and filer/HostFSFiler,ffa -- where make-release.sh
# and rom.py already look, the same as the DDE build's.
set -e
cd "$(dirname "$0")"
HERE=$(pwd)

CLANG="${CLANG:-clang}"
ROSASM="${ROSASM:-$HERE/../../../ROSASM/target/release/rosasm}"
ROSCC="${ROSCC:-$HERE/../../../ROSCC/target/debug/roscc}"
# llvm-readobj reads the relocations back for the check.  Homebrew puts it
# at /opt/homebrew on Apple silicon and versioned on Intel, as GVFill's
# build.sh already has to know.
READOBJ="${READOBJ:-}"
if [ -z "$READOBJ" ]; then
    for d in /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin \
             /usr/local/opt/llvm@21/bin /usr/local/opt/llvm@20/bin; do
        [ -x "$d/llvm-readobj" ] && READOBJ="$d/llvm-readobj" && break
    done
fi

for t in "$ROSASM" "$ROSCC" "$READOBJ"; do
    [ -n "$t" ] && [ -x "$t" ] || {
        echo "build-hostfs.sh: not executable: ${t:-llvm-readobj}" >&2
        echo "  rosasm: cargo build --release in the ROSASM repo" >&2
        echo "  roscc:  cargo build in the ROSCC repo" >&2
        echo "  readobj: brew install llvm, or set READOBJ=" >&2
        exit 1
    }
done

TRIPLE=armv8a-none-eabi
CPU=cortex-a72
# -fropi: see above, and it is the whole reason this can go in a ROM.
# -mno-movt: a MOVW/MOVT pair carries an absolute address in two halves,
#  which is exactly what must not appear.
# No unwind tables: .ARM.exidx is metadata for a personality routine that
#  does not exist here, and roscc drops the section anyway.
CC_FLAGS="--target=$TRIPLE -mcpu=$CPU -mfloat-abi=soft -ffreestanding \
-nostdlib -fno-builtin -fropi -frwpi -O2 -mno-movt \
-fno-unwind-tables -fno-asynchronous-unwind-tables"

# Every relocation ARM has that names an address rather than a distance.
# R_ARM_ABS32 is the one that actually turns up; the MOVW/MOVT pair and the
# byte and halfword forms are here so that a future source that produces
# one is stopped by this build rather than by a boot.
ABSOLUTE='R_ARM_ABS32|R_ARM_ABS16|R_ARM_ABS8|R_ARM_MOVW_ABS_NC|R_ARM_MOVT_ABS|R_ARM_SBREL32|R_ARM_TARGET1'

check_pc_relative_only() {
    # $1 object, $2 what it is
    bad=$("$READOBJ" -r "$1" | grep -E "$ABSOLUTE" || true)
    if [ -n "$bad" ]; then
        echo "build-hostfs.sh: $2 holds an absolute relocation:" >&2
        echo "$bad" | sed 's/^/    /' >&2
        echo >&2
        echo "  A module on the ROM chain runs in place and nothing" >&2
        echo "  relocates it.  Reach the datum pc-relatively, or move it" >&2
        echo "  into s.head where an ADR can find it." >&2
        exit 1
    fi
}

build_one() {
    dir=$1 csrc=$2 module=$3
    echo "== $module"
    mkdir -p "$HERE/$dir/build"
    out="$HERE/$dir/build"

    echo "   s.head   -> rosasm --elf"
    "$ROSASM" "$HERE/$dir/s/head" -o "$out/head.o" --elf >/dev/null
    check_pc_relative_only "$out/head.o" "$dir/s.head"

    echo "   c.$csrc -> clang"
    # The sources have no extension: RISC OS keeps the type in the
    # directory, so c.hostfs is the C file.  clang decides a language by
    # the extension it cannot see, so -x c says it outright.
    # shellcheck disable=SC2086
    "$CLANG" $CC_FLAGS -x c -c "$HERE/$dir/c/$csrc" -o "$out/$csrc.o"
    check_pc_relative_only "$out/$csrc.o" "$dir/c.$csrc"

    echo "   link     -> roscc --module"
    "$ROSCC" link --module -o "$HERE/$dir/$module" \
        "$out/head.o" "$out/$csrc.o"
    ls -l "$HERE/$dir/$module"
}

build_one dde   hostfs      'HostFS,ffa'
build_one filer hostfsfiler 'HostFSFiler,ffa'
