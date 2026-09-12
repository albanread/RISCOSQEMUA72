#!/bin/zsh
#
# Launch the app the way an app is meant to be launched: through
# LaunchServices, with `open --args` carrying the machine's arguments.
# Launching the bundle executable directly leaves the app in a
# half-registered state in which terminology, TCC and event routing
# behave inconsistently (SCRIPTING.md section 14); this is the app
# persona's canonical start.
#
# This is the USER persona: no QMP socket.  The Apple Events surface is
# the only always-on control channel, which is the whole point of the
# consent model -- the developer persona keeps tools/run-macos.sh and
# its socket.
#
#   RISCOS_IMAGES  where RISCOS.IMG, cmos.bin and card.img live
#                  (default: riscos-images/ beside the repo root)
#   APP            the bundle (default: build-macos/RISCOSQEMU.app)
#   WITH_QMP=1     add the developer's QMP socket anyway
#   RISCOS_HOSTFS  a host directory to serve as HostFS: inside the
#                  guest (FSDESIGN.md); unset leaves the doorbell
#                  device present but file commands off.  A share also
#                  puts the HostFS module in the ROM (rom.zsh)
#   RISCOS_MODULES further modules for the ROM (rom.zsh)
#   RISCOS_BOOT    "hostfs" boots from the share instead of the card
#
# Anything after the options is passed on as further QEMU arguments.
#
set -e
HERE="${0:A:h}"
ROOT="${HERE:h:h}"
IMAGES="${RISCOS_IMAGES:-$ROOT/riscos-images}"
APP="${APP:-$ROOT/build-macos/RISCOSQEMU.app}"

for f in "$APP" "$IMAGES/RISCOS.IMG" "$IMAGES/cmos.bin"; do
    [[ -e "$f" ]] || { print -u2 "missing: $f"; exit 1; }
done

# The same ROM the developer's launch boots: HostFS in it with a share
source "$HERE/rom.zsh"
rom_to_boot "$IMAGES"
cmos_to_boot "$IMAGES"

args=(
    -M raspi4b -cpu cortex-a72,aarch64=off
    -kernel "$ROM"
    -device loader,file="$CMOS",addr=0x510000,force-raw=on
    -netdev user,id=n0
    -device usb-hub,bus=usb-bus.0,port=1
    -device usb-kbd,bus=usb-bus.0,port=1.1
    -device usb-tablet,bus=usb-bus.0,port=1.2
    -device usb-net,netdev=n0,rndis=off,bus=usb-bus.0,port=1.3
    -audiodev coreaudio,id=snd0
    -global bcm2835-vchiq.audiodev=snd0
    -display metal,vsync=30
)
# Without a card it boots to the desktop from ROM alone.  snapshot=on
# keeps the image pristine across runs.
[[ -e "$IMAGES/card.img" ]] && \
    args+=(-drive file="$IMAGES/card.img",if=sd,format=raw,snapshot=on)
[[ -n "${WITH_QMP:-}" ]] && \
    args+=(-qmp tcp:127.0.0.1:4455,server,nowait)
[[ -n "${RISCOS_HOSTFS:-}" ]] && \
    args+=(-global "bcm2838-peripherals.vmchannel-root=$RISCOS_HOSTFS")

exec open -n "$APP" --args "${args[@]}" "$@"
