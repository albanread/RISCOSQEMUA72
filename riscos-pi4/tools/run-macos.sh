#!/bin/zsh
#
# The canonical macOS launch: riscos-pi4/README.md's line with
# -display metal in place of -display dx11.
#
#   RISCOS_IMAGES  where RISCOS.IMG, cmos.bin and card.img live
#                  (default: riscos-images/ beside the repo root)
#   QEMU_BIN       the built emulator (default: build-macos/)
#   DISPLAY_OPT    the -display argument; "cocoa" for QEMU's own display,
#                  which converts the framebuffer on the CPU
#
# Anything after the options is passed on to QEMU.
#
set -e
HERE="${0:A:h}"
ROOT="${HERE:h:h}"
IMAGES="${RISCOS_IMAGES:-$ROOT/riscos-images}"
Q="${QEMU_BIN:-$ROOT/build-macos/qemu-system-aarch64}"
DISPLAY_OPT="${DISPLAY_OPT:-metal,vsync=30}"

for f in "$Q" "$IMAGES/RISCOS.IMG" "$IMAGES/cmos.bin"; do
    [[ -e "$f" ]] || { print -u2 "missing: $f"; exit 1; }
done

args=(
    -M raspi4b -cpu cortex-a72,aarch64=off
    -kernel "$IMAGES/RISCOS.IMG"
    -device loader,file="$IMAGES/cmos.bin",addr=0x510000,force-raw=on
    -netdev user,id=n0
    -device usb-hub,bus=usb-bus.0,port=1
    -device usb-kbd,bus=usb-bus.0,port=1.1
    -device usb-tablet,bus=usb-bus.0,port=1.2
    -device usb-net,netdev=n0,rndis=off,bus=usb-bus.0,port=1.3
    -display "$DISPLAY_OPT"
    -qmp tcp:127.0.0.1:4455,server,nowait
)
# Without a card it boots to the desktop from ROM alone, with nothing
# behind the SD icon.  snapshot=on keeps the image pristine across runs.
[[ -e "$IMAGES/card.img" ]] && \
    args+=(-drive file="$IMAGES/card.img",if=sd,format=raw,snapshot=on)

exec "$Q" "${args[@]}" "$@"
