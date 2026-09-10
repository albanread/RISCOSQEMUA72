#!/bin/bash
# Build the HostFS module: header + FS veneers + C body -> HostFS,ffa
#
#   hostfs/build-hostfs.sh        (from anywhere)
#
# Chain, per riscos-pi4/FSDESIGN.md:
#   gen_module.py -> module header and init/final/command veneers
#   hostfs_fsinfo.s -> the FSInfo block, concatenated into the generated
#     head so it lands in the one .module section roscc keeps
#   clang -fropi -frwpi -O0 -> code PC-relative, statics r9-relative
#   hostfs_entries.s -> the eight FSEntry veneers (assembly, same flags)
#   roscc link --module -> base-0 relocatable module, position checked
#
# -O0 and -mno-movt matter: they force literal-pool addressing of
# statics (SBREL32), the one static-base relocation the module linker
# applies.  MOVW/MOVT pairs (BREL) come out at -O1, or on any CPU with
# Thumb-2 extensions unless movt is disabled — build-module.sh never saw
# this because its default CPU is strongarm110, which has no movt.

set -e
cd "$(dirname "$0")"

LLVM="/c/Program Files/LLVM/bin"
ROSCC="/f/RISCOSDEV/compiler/target/debug/roscc.exe"
GEN="/f/RISCOSDEV/compiler/tools/gen_module.py"
RTRT="/f/RISCOSDEV/compiler/rostrt"
TRIPLE=armv8a-none-eabi
CPU=cortex-a72
OUT=build

MOD_ARENAS="-DHEAP_BYTES=32768 -DARENA_BYTES=8192"
CC_FLAGS="--target=$TRIPLE -mcpu=$CPU -mfloat-abi=soft -ffreestanding -nostdlib -fno-builtin -fropi -frwpi -O0 -mno-movt $MOD_ARENAS"

mkdir -p "$OUT"

echo "== header + module veneers + FSInfo block"
python "$GEN" \
    --title HostFS \
    --help-text 'HostFS\t1.00 (09 Sep 2026)' \
    --rwpi \
    --init hostfs_init --final hostfs_final \
    --command 'HostFSPing:hostfs_command_ping:0:0:*HostFSPing rings the doorbell\rSyntax:\t*HostFSPing' \
    --arch armv8a \
    -o "$OUT/module_head.s"
cat "$OUT/module_head.s" hostfs_fsinfo.s > "$OUT/module_head_full.s"
"$LLVM/clang.exe" $CC_FLAGS -c "$OUT/module_head_full.s" -o "$OUT/module_head.o"

echo "== FS veneers"
"$LLVM/clang.exe" $CC_FLAGS -c hostfs_entries.s -o "$OUT/entries.o"

echo "== module body"
"$LLVM/clang.exe" $CC_FLAGS -c hostfs.c -o "$OUT/hostfs.o"

echo "== runtime"
for src in rostrt.c swis_os.c modrt.c; do
    "$LLVM/clang.exe" $CC_FLAGS -c "$RTRT/$src" -o "$OUT/${src%.c}.o"
done
"$LLVM/clang.exe" --target=$TRIPLE -mcpu=$CPU -mfloat-abi=soft \
    -c "$RTRT/aeabi.s" -o "$OUT/aeabi.o"
"$LLVM/clang.exe" --target=$TRIPLE -mcpu=$CPU -mfloat-abi=soft \
    -c "$RTRT/atomics_swp.s" -o "$OUT/atomics.o"

echo "== link"
"$ROSCC" link --module -o "$OUT/HostFS,ffa" \
    "$OUT/module_head.o" "$OUT/entries.o" "$OUT/hostfs.o" \
    "$OUT/rostrt.o" "$OUT/swis_os.o" "$OUT/modrt.o" \
    "$OUT/aeabi.o" "$OUT/atomics.o"

ls -l "$OUT/HostFS,ffa"
