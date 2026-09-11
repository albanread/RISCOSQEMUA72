#!/bin/zsh
#
# Assemble the .app the Apple Events surface needs (SCRIPTING.md section 3):
# a bundle identity to target, an Info.plist that turns scripting on, and
# the sdef osscript compiles against.  The executable is a copy, so the
# bundle survives rebuilds of the bare binary; re-run this after a build.
#
#   make-bundle.sh [binary]        default: build-macos/qemu-system-aarch64
#
# The bundle is ad-hoc signed, which is enough for local TCC testing; the
# Developer ID and notarization step is Sprint E5.
#
set -e
HERE="${0:A:h}"
ROOT="${HERE:h:h}"
BIN="${1:-$ROOT/build-macos/qemu-system-aarch64}"
APP="$ROOT/build-macos/RISCOSQEMU.app"

[[ -x "$BIN" ]] || { print -u2 "no such binary: $BIN"; exit 1 }
# The sdef and the schema regenerate from the command table first, so a
# stale dictionary cannot ship (mksdef.py is the mkcmos.py precedent of
# parsing the source that the dispatch actually uses).
python3 "$ROOT/riscos-pi4/tools/mksdef.py" || exit 1
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/"
cp "$ROOT/riscos-pi4/app/Info.plist" "$APP/Contents/"
cp "$ROOT/riscos-pi4/app/RISCOSQEMU.sdef" "$APP/Contents/Resources/"
# Force a fresh ad-hoc signature every time: the executable is a copy of
# an already-signed binary and the bundle contents change per build, so
# a stale seal gets the app SIGKILLed at launch for an invalid signature
# (observed: E0's Q3, "Taskgated Invalid Signature").  A failed sign is
# an error, not a warning -- an unsigned or stale-sealed bundle does not
# start.  Developer ID and notarization are Sprint E5.
rm -rf "$APP/Contents/_CodeSignature"
xattr -cr "$APP"     # resource forks and Finder info break codesign
codesign -f -s - "$APP"
codesign --verify "$APP" || { print -u2 "signature does not verify"; exit 1; }
print "$APP"
