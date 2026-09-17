#!/bin/zsh
#
# The user launch.  This is Contents/Resources/launcher.zsh in the release
# bundle tools/make-release.sh builds; Contents/MacOS/<app name> is the
# compiled stub (app/launcher.c) LaunchServices starts on a double click,
# and it execs /bin/zsh on this file.  This in turn execs the emulator
# with the machine's arguments, so the running process is the app itself
# -- same PID, an executable inside the bundle -- and stays scriptable
# through the Apple Events surface (SCRIPTING.md).  It opens no QMP
# socket: this is the user persona of run-app.sh, with the paths settled
# for an installed app.
#
# What it settles, in order:
#
#   the disc    a folder of the user's choosing, asked for the first time
#               the app runs (the offer is ~/RISCOS), unpacked from the
#               app's Disc.zip and never touched by a later version.  That
#               folder is the RISC OS disc: HostFS serves it, both ways.
#               The choice is kept in `defaults` under `disc`; holding
#               Option as the app starts asks again (the stub sets
#               RISCOS_CHOOSE_DISC), as does a folder that has gone missing.
#   the CMOS    the disc's own CMOS,ff2 -- HostFS rewrites it after every
#               *Configure -- copied under a comma-free name, because the
#               loader's option syntax splits on commas.  The app's
#               cmos.bin seeds a disc that has none.
#   the logs    ~/Library/Logs/<base name>/run.log (QEMU's stdout and
#               stderr) and metal-debug.txt, which lands in the cwd.
#   the backdrop  `defaults write <bundle id> backdrop acorn-live` picks
#               the layer drawn beneath the desktop: acorn (the release's
#               default, from RISCOSBackdrop in Info.plist), acorn-live,
#               none, tile:<image> or picture:<image> -- or off, which
#               gates the feature out of the emulator, Backdrop menu and
#               all.  It shows through where the disc tiles its tagged
#               backdrop sprite (MACOS.md, "The backdrop layer"); on a
#               disc that does not, it never shows.
#   the mode    `defaults write <bundle id> mode 1920x1200` opens the
#               desktop at that size (README.md: the EDID timing).
#   the network HostNet or the ROM's own stack, one or the other.  HostNet
#               is a module on the disc: in Modules/ the boot loads it and,
#               titled Internet, it replaces the ROM's stack; in
#               Modules/Disabled/ the ROM's stack boots, with DHCP.
#               Machine > HostNet switches in place -- moves the module,
#               sets the doorbell, reboots RISC OS -- and keeps the choice
#               as `hostnet` (on or off) in `defaults`; until the first
#               switch, the build's RISCOSHostNet in Info.plist stands.
#               Here the module is put where the choice says, the doorbell
#               lit only when the module is in Modules/ (dark, the module
#               would replace the ROM's Internet and decline, leaving none),
#               and the emulated card attached either way, for the ROM's
#               stack.
#
# Anything LaunchServices or `open --args` passes is handed on to QEMU,
# which is how a developer adds a QMP socket to an installed app.
#
set -u
SELF="${0:A}"
CONTENTS="${SELF:h:h}"           # Contents/Resources/launcher.zsh -> Contents
RES="$CONTENTS/Resources"
BIN="$CONTENTS/MacOS/qemu-system-aarch64"
ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$CONTENTS/Info.plist")"
APPNAME="${RISCOS_APP_NAME:-$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$CONTENTS/Info.plist")}"
BASE="${ID##*.}"                 # com.github.albanread.RISCOSQEA72 -> RISCOSQEA72

pref() { defaults read "$ID" "$1" 2>/dev/null; return 0 }
q()    { print -r -- "${1//,/,,}" }   # a comma in a QEMU option value is written ,,

LOGS="$HOME/Library/Logs/$BASE"
STATE="$HOME/Library/Application Support/$BASE"
mkdir -p "$LOGS" "$STATE" 2>/dev/null
[[ -e "$LOGS/run.log" ]] && mv -f "$LOGS/run.log" "$LOGS/run-previous.log"
exec >>"$LOGS/run.log" 2>&1
cd "$LOGS" 2>/dev/null || cd /   # metal-debug.txt lands in the cwd
print -r -- "$APPNAME: $(date '+%Y-%m-%d %H:%M:%S') starting"

fail() {
    print -r -- "$APPNAME: $*"
    osascript -e "display alert \"$APPNAME\" message \"$*\" as critical" >/dev/null 2>&1
    exit 1
}

# Ask where the disc lives: a dialog with the offer of ~/RISCOS, or a
# folder chooser.  Cancelling either quits, since there is no disc to boot.
choose_disc() {
    local offer="$HOME/RISCOS" why="$1" reply
    reply=$(osascript - "$APPNAME" "$offer" "$why" <<'EOS' 2>/dev/null
on run argv
    set appName to item 1 of argv
    set offer to item 2 of argv
    set why to item 3 of argv
    activate    -- in front, not behind whatever window was frontmost at launch
    set msg to why & "RISC OS keeps its disc in a folder on this Mac. What you see inside RISC OS is that folder, and anything you put in the folder appears inside RISC OS." & return & return & "Use the folder " & offer & ", or choose one of your own? A new or empty folder is best: it becomes the disc."
    set answer to display dialog msg buttons {"Choose a Folder…", "Use " & offer} default button 2 with title appName with icon note
    if button returned of answer is "Choose a Folder…" then
        return POSIX path of (choose folder with prompt "Choose the folder that will be the RISC OS disc" default location (path to home folder))
    else
        return offer
    end if
end run
EOS
)
    if [[ -z "$reply" ]]; then
        print -r -- "$APPNAME: no disc folder chosen; not starting"
        exit 0
    fi
    DISC="${reply%/}"
    defaults write "$ID" disc "$DISC"
    print -r -- "$APPNAME: disc folder chosen: $DISC"
}

DISC="$(pref disc)"
MODE="$(pref mode)"
BACKDROP="$(pref backdrop)"
[[ -n "$BACKDROP" ]] || BACKDROP="$(/usr/libexec/PlistBuddy -c 'Print :RISCOSBackdrop' "$CONTENTS/Info.plist" 2>/dev/null)"
BACKDROP="${BACKDROP:-off}"
HOSTNET="$(pref hostnet)"
[[ -n "$HOSTNET" ]] || HOSTNET="$(/usr/libexec/PlistBuddy -c 'Print :RISCOSHostNet' "$CONTENTS/Info.plist" 2>/dev/null)"
HOSTNET="${HOSTNET:-off}"
if [[ -n "${RISCOS_CHOOSE_DISC:-}" ]]; then
    print -r -- "$APPNAME: Option held at launch: asking for the disc folder"
    choose_disc ""
elif [[ -z "$DISC" ]]; then
    if [[ -d "$HOME/RISCOS/!Boot" ]]; then
        DISC="$HOME/RISCOS"                    # a disc from before the choice existed
    else
        print -r -- "$APPNAME: first run: asking for the disc folder"
        choose_disc ""
    fi
elif [[ ! -d "$DISC" ]]; then
    print -r -- "$APPNAME: the disc folder $DISC is missing: asking again"
    choose_disc "The RISC OS disc folder $DISC is not there any more. "
fi

[[ -x "$BIN" ]] || fail "The emulator is missing from the app: $BIN"
[[ -r "$RES/RISCOS.IMG" ]] || fail "The ROM is missing from the app: $RES/RISCOS.IMG"

if [[ ! -d "$DISC/!Boot" ]]; then
    [[ -r "$RES/Disc.zip" ]] || fail "There is no RISC OS disc at $DISC, and this app has none to install."
    mkdir -p "$DISC" || fail "Cannot create the disc folder $DISC"
    print -r -- "$APPNAME: installing the disc into $DISC"
    # -n: never overwrite.  The folder is the user's choice and may already
    # hold files of their own; a name the disc shares with one of them
    # keeps the user's file.
    unzip -q -n "$RES/Disc.zip" -d "$DISC" || fail "Unpacking the disc into $DISC failed"
fi
[[ -e "$DISC/CMOS,ff2" ]] || cp "$RES/cmos.bin" "$DISC/CMOS,ff2"
CMOS="$STATE/cmos.bin"
cp -f "$DISC/CMOS,ff2" "$CMOS" || fail "Cannot copy the CMOS settings to $CMOS"

# The network: HostNet's module where the choice puts it -- Machine > HostNet
# moves it too, so this only matters when the two have drifted -- and the
# doorbell lit only where the module is.  A disc from before HostNet has
# none, and boots the ROM's stack whatever the choice.
MODS="$DISC/Modules"
if [[ "$HOSTNET" == on ]]; then
    [[ -f "$MODS/Disabled/HostNet,ffa" && ! -e "$MODS/HostNet,ffa" ]] &&
        mv "$MODS/Disabled/HostNet,ffa" "$MODS/HostNet,ffa"
elif [[ -f "$MODS/HostNet,ffa" ]]; then
    mkdir -p "$MODS/Disabled" && mv -f "$MODS/HostNet,ffa" "$MODS/Disabled/HostNet,ffa"
fi
hostnet=()
NETWORK="the RISC OS stack"
if [[ -f "$MODS/HostNet,ffa" ]]; then
    hostnet=( -global hostnet.sockets=on )
    NETWORK="HostNet"
fi

args=(
    -M raspi4b -cpu cortex-a72,aarch64=off
    -kernel "$RES/RISCOS.IMG"
    -device "loader,file=$(q "$CMOS"),addr=0x510000,force-raw=on"
    -netdev user,id=n0,domainname=lan
    -device usb-hub,bus=usb-bus.0,port=1
    -device usb-kbd,bus=usb-bus.0,port=1.1
    -device usb-tablet,bus=usb-bus.0,port=1.2
    -device usb-net,netdev=n0,rndis=off,bus=usb-bus.0,port=1.3
    "${hostnet[@]}"
    -audiodev coreaudio,id=snd0
    -global bcm2835-vchiq.audiodev=snd0
    -display "metal,vsync=30,backdrop=$(q "$BACKDROP")"
    -name "$APPNAME"
    -global "bcm2838-peripherals.vmchannel-root=$(q "$DISC")"
)
if [[ -n "$MODE" ]]; then
    args+=( -global "bcm2835-property.mode=$MODE" )
fi
extra=()
for a in "$@"; do
    [[ "$a" == -psn_* ]] || extra+=("$a")   # LaunchServices' process serial number, if it sends one
done
print -r -- "$APPNAME: disc $DISC${MODE:+, mode $MODE}, backdrop $BACKDROP, network $NETWORK"
exec "$BIN" "${args[@]}" "${extra[@]}"
