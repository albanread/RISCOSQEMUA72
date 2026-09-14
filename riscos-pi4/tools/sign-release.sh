#!/bin/zsh
#
# sign-release.sh -- sign, notarize and package an already-built release app
# into a DMG.  The companion to make-release.sh for a two-machine flow: one
# machine assembles the app (make-release.sh with SIGN_ID="-", ad-hoc), and
# the machine that holds the Developer ID and the notarytool profile runs
# this on the handed-over bundle.  It does exactly make-release.sh's steps
# 9-10 (sign the frameworks, then the emulator with its entitlements, then
# the launcher, then the app; notarize the app; make, sign and notarize the
# DMG) and nothing else -- it never rebuilds or re-stages, so it works on a
# bundle of any architecture (codesign and notarytool are arch-agnostic).
#
#   sign-release.sh <app> [N]
#
#     <app>   the .app to sign (built elsewhere; its Mach-Os may be x86_64)
#     N       release number, for the DMG name (default: from the app name)
#
#   SIGN_ID         Developer ID Application identity (required; not "-")
#   NOTARY_PROFILE  a notarytool keychain profile; if set, the app and the
#                   DMG are submitted, waited for and stapled
#   ENTS            entitlements for the emulator
#                   (default riscos-pi4/app/entitlements.plist)
#   OUT             where the DMG lands (default: the app's directory)
#   DMG_NAME        the .dmg basename (default <app-stem>-<arch>)
#
# The app must already carry its RELEASE.txt (make-release.sh writes it
# before sealing): do not add or change a file in the bundle after signing,
# or the seal breaks ("a sealed resource is missing or invalid").
#
set -e
HERE="${0:A:h}"
ROOT="${HERE:h:h}"

APP="${1:?usage: sign-release.sh <app> [N]}"
APP="${APP:A}"
[[ -d "$APP" ]] || { print -u2 "sign-release.sh: no such app: $APP"; exit 1 }
STEM="${${APP:t}:r}"                          # RISCOSQEA72vN.app -> RISCOSQEA72vN
N="${2:-${STEM##*[!0-9]}}"                    # trailing digits of the name
NAME="$STEM"

SIGN_ID="${SIGN_ID:-}"
[[ -n "$SIGN_ID" && "$SIGN_ID" != "-" ]] || {
    print -u2 "sign-release.sh: SIGN_ID must be a Developer ID identity, not '$SIGN_ID'"; exit 1 }
NOTARY_PROFILE="${NOTARY_PROFILE:-}"
ENTS="${ENTS:-$ROOT/riscos-pi4/app/entitlements.plist}"
[[ -f "$ENTS" ]] || { print -u2 "sign-release.sh: no entitlements at $ENTS"; exit 1 }
OUT="${OUT:-${APP:h}}"
EMU="$APP/Contents/MacOS/qemu-system-aarch64"
[[ -x "$EMU" ]] || { print -u2 "sign-release.sh: emulator not found in bundle: $EMU"; exit 1 }
ARCH=$(lipo -archs "$EMU" 2>/dev/null | awk '{print $1}')
DMG_NAME="${DMG_NAME:-$NAME-$ARCH}"
DMG="$OUT/$DMG_NAME.dmg"
STAGE="$OUT/sign-stage"
rm -rf "$STAGE"; mkdir -p "$STAGE" "$OUT"

for t in codesign hdiutil xattr ditto lipo; do
    command -v "$t" >/dev/null || { print -u2 "sign-release.sh: missing tool: $t"; exit 1 }
done
[[ -n "$NOTARY_PROFILE" ]] && { command -v xcrun >/dev/null || { print -u2 "no xcrun"; exit 1 } }

print "signing $NAME ($ARCH) with: $SIGN_ID"

sign() {   # codesign, quiet unless it fails
    if ! codesign "$@" 2>"$STAGE/codesign.err"; then
        cat "$STAGE/codesign.err" >&2
        print -u2 "sign-release.sh: codesign failed on ${@[-1]}"; exit 1
    fi
}

# --- sign, inside-out: frameworks, emulator (entitlements), launcher, app
signopts=(--options runtime --timestamp)
xattr -cr "$APP"
for f in "$APP"/Contents/Frameworks/*.dylib(N); do
    sign -f -s "$SIGN_ID" "${signopts[@]}" "$f"
done
sign -f -s "$SIGN_ID" "${signopts[@]}" --entitlements "$ENTS" "$EMU"
sign -f -s "$SIGN_ID" "${signopts[@]}" "$APP/Contents/MacOS/$NAME"
sign -f -s "$SIGN_ID" "${signopts[@]}" "$APP"
codesign --verify --deep --strict "$APP" || { print -u2 "the signature does not verify"; exit 1 }
print "signature verifies (hardened runtime, timestamped)"

notarize() {   # notarize <file>: submit, wait, insist on Accepted
    local out="$STAGE/notary-${1:t}.json" id verdict
    xcrun notarytool submit "$1" --keychain-profile "$NOTARY_PROFILE" --wait \
        --output-format json >"$out" 2>"$STAGE/notary.err" || {
        cat "$STAGE/notary.err" >&2; print -u2 "notarytool submit failed for ${1:t}"; exit 1
    }
    id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["id"])' "$out")
    verdict=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["status"])' "$out")
    print "notarization of ${1:t}: $verdict (submission $id)"
    if [[ "$verdict" != "Accepted" ]]; then
        xcrun notarytool log "$id" --keychain-profile "$NOTARY_PROFILE" >&2 || true
        print -u2 "Apple did not accept ${1:t}"; exit 1
    fi
}

if [[ -n "$NOTARY_PROFILE" ]]; then
    print "notarizing the app (keychain profile $NOTARY_PROFILE)"
    ditto -c -k --keepParent "$APP" "$STAGE/$NAME.zip"
    notarize "$STAGE/$NAME.zip"
    xcrun stapler staple "$APP" >/dev/null || { print -u2 "stapling the app failed"; exit 1 }
fi

# --- the disk image
mkdir -p "$STAGE/dmg"
ditto "$APP" "$STAGE/dmg/$NAME.app"
ln -s /Applications "$STAGE/dmg/Applications"
[[ -f "$APP/Contents/Resources/ReadMe.txt" ]] && cp "$APP/Contents/Resources/ReadMe.txt" "$STAGE/dmg/Read Me.txt"
hdiutil create -volname "$NAME" -srcfolder "$STAGE/dmg" -ov -format UDZO -quiet "$DMG"
sign -f -s "$SIGN_ID" --timestamp "$DMG"
if [[ -n "$NOTARY_PROFILE" ]]; then
    notarize "$DMG"
    xcrun stapler staple "$DMG" >/dev/null || { print -u2 "stapling the disk image failed"; exit 1 }
    xcrun stapler validate "$APP" >/dev/null && xcrun stapler validate "$DMG" >/dev/null \
        && print "staples validate" || { print -u2 "a staple does not validate"; exit 1 }
fi

spctl -a -t open --context context:primary-signature -v "$DMG" 2>&1 | sed 's/^/  /' || true
rm -rf "$STAGE"
print "done: $DMG"
