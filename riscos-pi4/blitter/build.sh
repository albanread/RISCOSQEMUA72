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
OBJCOPY=${OBJCOPY:-/opt/homebrew/opt/llvm/bin/llvm-objcopy}
READOBJ=${READOBJ:-/opt/homebrew/opt/llvm/bin/llvm-readobj}

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
