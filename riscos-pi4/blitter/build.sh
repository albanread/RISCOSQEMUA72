#!/bin/bash
# Build GVFill and refuse to produce a module that will not load.
#
# A RISC OS module is position independent: it is loaded wherever the RMA
# has room, and nothing relocates it.  Any relocation left in the object
# is therefore a branch or a word pointing at nowhere -- and the failure
# is not a load error, it is the desktop going black some time later.
# Twice now that has cost a debugging round, so the check is the build.
set -e
cd "$(dirname "$0")"

CLANG=${CLANG:-clang}
# llvm-objcopy and llvm-readobj come from Homebrew's LLVM, which lives at
# /opt/homebrew on Apple silicon and versioned (/usr/local/opt/llvm@N) on
# Intel; OBJCOPY/READOBJ in the environment beat any of it.
llvmbin=""
for d in /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin \
         /usr/local/opt/llvm@21/bin /usr/local/opt/llvm@20/bin; do
    [[ -x "$d/llvm-objcopy" && -x "$d/llvm-readobj" ]] && llvmbin="$d" && break
done
OBJCOPY=${OBJCOPY:-${llvmbin:+$llvmbin/llvm-objcopy}}
READOBJ=${READOBJ:-${llvmbin:+$llvmbin/llvm-readobj}}
if [ -z "$OBJCOPY" ] || [ -z "$READOBJ" ]; then
    echo "need llvm-objcopy and llvm-readobj (brew install llvm), or set OBJCOPY/READOBJ" >&2
    exit 1
fi

rm -f blitmod.o 'GVFill,ffa'
"$CLANG" --target=arm-none-eabi -mcpu=cortex-a72 -mfloat-abi=soft \
         -c blitmod.s -o blitmod.o

n=$("$READOBJ" -r blitmod.o | grep -c R_ARM || true)
if [ "$n" -ne 0 ]; then
    echo "refusing to build: $n relocation(s) left" >&2
    "$READOBJ" -r blitmod.o | grep R_ARM >&2
    echo >&2
    echo "A BL to a named symbol leaves R_ARM_CALL even within the file;" >&2
    echo "inline it, or reach data with ADR (about a kilobyte of range)." >&2
    exit 1
fi

"$OBJCOPY" -O binary --only-section=.text blitmod.o 'GVFill,ffa'
ls -l 'GVFill,ffa'
