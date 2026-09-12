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
#   AUDIODEV       the -audiodev driver (default coreaudio; "none" for
#                  silence that still keeps the guest's sound loop turning)
#   RISCOS_HOSTFS  a host directory to serve as HostFS: inside the
#                  guest (FSDESIGN.md); unset leaves the doorbell
#                  device present but file commands off
#   RISCOS_QMP     the -qmp chardev (default tcp:127.0.0.1:4455,server,nowait).
#                  Two machines cannot share a port; give each its own,
#                  or a unix socket: unix:/path/qmp.sock,server,nowait
#   RISCOS_NAME    -name, so a machine is identifiable in ps and its window
#   RISCOS_PIDFILE -pidfile, so a machine can be stopped without touching
#                  anyone else's.  Stop machines by pidfile or QMP quit,
#                  never by process name: pkill -f qemu-system-aarch64
#                  kills every session on the host.
#
# Everything above defaults to exactly what this script did before, so a
# plain run-macos.sh is unchanged.  tools/instance.sh builds on these to
# run several isolated machines at once.
#   RISCOS_MODULES space-separated module files spliced into the ROM
#                  before boot (BOOTDESIGN.md §3; order = init order).
#                  The spliced image is cached beside the stock one and
#                  rebuilt only when a module changes.  Without it the
#                  stock RISCOS.IMG is booted untouched.
#
# Anything after the options is passed on to QEMU.
#
set -e
HERE="${0:A:h}"
ROOT="${HERE:h:h}"
IMAGES="${RISCOS_IMAGES:-$ROOT/riscos-images}"
Q="${QEMU_BIN:-$ROOT/build-macos/qemu-system-aarch64}"
DISPLAY_OPT="${DISPLAY_OPT:-metal,vsync=30}"
AUDIODEV="${AUDIODEV:-coreaudio}"
MODE="${MODE:-}"

for f in "$Q" "$IMAGES/RISCOS.IMG" "$IMAGES/cmos.bin"; do
    [[ -e "$f" ]] || { print -u2 "missing: $f"; exit 1; }
done

# ROM module splicing (BOOTDESIGN.md §3): the spliced image is cached
# under a hash of the stock ROM plus every module's contents, so a
# module edit rebuilds it and a relaunch does not.
ROM="$IMAGES/RISCOS.IMG"
if [[ -n "${RISCOS_MODULES:-}" ]]; then
    mods=(${=RISCOS_MODULES})
    for m in $mods; do
        [[ -e "$m" ]] || { print -u2 "RISCOS_MODULES: missing: $m"; exit 1; }
    done
    key=$( (shasum -a 256 "$ROM" $mods; shasum -a 256 $mods) \
           | shasum -a 256 | cut -c1-16 )
    ROM="$IMAGES/RISCOS-$key.IMG"
    if [[ ! -e "$ROM" ]]; then
        print "run-macos: splicing ${#mods} module(s) -> ${ROM:t}"
        modargs=( -o "$ROM" )
        for m in $mods; do modargs+=( -m "$m" ); done
        "$HERE/mkrom.py" "$IMAGES/RISCOS.IMG" $modargs
    else
        print "run-macos: cached ${ROM:t}"
    fi
fi

args=(
    -M raspi4b -cpu cortex-a72,aarch64=off
    -kernel "$ROM"
    -device loader,file="$IMAGES/cmos.bin",addr=0x510000,force-raw=on
    -netdev user,id=n0
    -device usb-hub,bus=usb-bus.0,port=1
    -device usb-kbd,bus=usb-bus.0,port=1.1
    -device usb-tablet,bus=usb-bus.0,port=1.2
    -device usb-net,netdev=n0,rndis=off,bus=usb-bus.0,port=1.3
    -audiodev "$AUDIODEV",id=snd0
    -global bcm2835-vchiq.audiodev=snd0
    -display "$DISPLAY_OPT"
    -qmp "${RISCOS_QMP:-tcp:127.0.0.1:4455,server,nowait}"
)
[[ -n "${RISCOS_NAME:-}" ]] && args+=(-name "$RISCOS_NAME")
[[ -n "${RISCOS_PIDFILE:-}" ]] && args+=(-pidfile "$RISCOS_PIDFILE")
# Without a card it boots to the desktop from ROM alone, with nothing
# behind the SD icon.  snapshot=on keeps the image pristine across runs.
# MODE=1920x1200 etc: the EDID preferred timing, so RISC OS brings the
# desktop up at that size. Empty keeps the 800x600 default.
[[ -n "$MODE" ]] && args+=(-global "bcm2835-property.mode=$MODE")
[[ -n "${RISCOS_HOSTFS:-}" ]] && \
    args+=(-global "bcm2838-peripherals.vmchannel-root=$RISCOS_HOSTFS")

[[ -e "$IMAGES/card.img" ]] && \
    args+=(-drive file="$IMAGES/card.img",if=sd,format=raw,snapshot=on)

exec "$Q" "${args[@]}" "$@"
