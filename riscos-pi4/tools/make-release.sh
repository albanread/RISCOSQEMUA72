#!/bin/zsh
#
# make-release.sh -- the end-user Mac release: one self-contained app.
#
#   <NAME>v<N>.app   the emulator, every library it links, the stock ROM
#                    with HostFS and its filer spliced in, the end-user
#                    disc as a zip the app unpacks on first run, and the
#                    launcher: a compiled stub (app/launcher.c) that runs
#                    app/launcher.zsh, which ties them together
#   <NAME>v<N>.dmg   that app, a Read Me and an Applications link
#   <NAME>v<N>-disc.zip  the cut-down disc on its own, as the app carries it
#
# all under build-macos/release/.  make-bundle.sh is the developer's
# bundle -- the bare emulator, no ROM, QMP on; this is the user's: no
# QMP, everything inside, nothing to configure.
#
#   make-release.sh [N]      N is the release number (default 1)
#
#   RELEASE_NAME  base name (default RISCOSQEA72; the app is <base>v<N>)
#   ROS_PRIVATE   the private repo (default ../ROS_PRIVATE, beside this one)
#   ROM           the stock ROM (default $ROS_PRIVATE/roms/pi/v530/RISCOS.IMG)
#   FS_ZIP        the end-user disc (default $ROS_PRIVATE/dist/end_user_fs.zip)
#   QEMU_BIN      the built emulator (default build-macos/qemu-system-aarch64)
#   OUT           where the release lands (default build-macos/release)
#   STRIP         what to leave off the disc: space-separated paths under
#                 its root, globs allowed.  The default, STRIP_DEFAULT
#                 below, cuts the end-user zip down to the desktop and
#                 its applications: no DDE (licensed to us, not ours to
#                 hand out), no Store or PackMan, no Pi boot-partition
#                 image, no Ghostscript, no unused themes, no manuals, no
#                 games.  Whatever is left off is also unpinned from the
#                 Pinboard.  STRIP="" ships the zip as it is.
#   NO_BUILD=1    do not run ninja first
#   SIGN_ID       the codesign identity.  Default "-": ad-hoc, enough for
#                 local use and for testers who use "Open Anyway".  A
#                 "Developer ID Application: ..." identity signs every
#                 Mach-O with the hardened runtime and a timestamp, the
#                 emulator with app/entitlements.plist (allow-jit: TCG
#                 writes the code it runs), as notarization requires.
#   BACKDROP      the layer the host draws beneath the desktop (MACOS.md,
#                 "The backdrop layer"): acorn (default), acorn-live, or
#                 off.  Anything but off tiles the tagged sprite from
#                 mkbacktile.py across the disc's pinboard, turns on the
#                 Wimp's -NoIconBoxesInTransWindows, and makes the value
#                 the app's default (RISCOSBackdrop in Info.plist; a user
#                 overrides it with `defaults write <id> backdrop ...`).
#                 off gates the feature out of the app: the disc keeps its
#                 watermark, and the emulator shows no Backdrop menu.
#   NOTARY_PROFILE a notarytool keychain profile (xcrun notarytool
#                 store-credentials <name>, done once by the account
#                 holder).  With it, and a Developer ID, the app and then
#                 the disk image are submitted to Apple, waited for, and
#                 stapled; the build fails if Apple declines.
#
# The libraries: Homebrew's dylibs are copied into Contents/Frameworks and
# every load command that named them -- in the emulator and in each other
# -- is rewritten to @executable_path/../Frameworks, so the app runs on a
# Mac with no Homebrew.  Their minimum macOS becomes the app's.
#
set -e
HERE="${0:A:h}"
ROOT="${HERE:h:h}"
N="${1:-1}"
[[ "$N" =~ '^[0-9]+$' ]] || { print -u2 "make-release.sh: the release number must be an integer, not '$N'"; exit 1 }
BASENAME="${RELEASE_NAME:-RISCOSQEA72}"
NAME="${BASENAME}v${N}"
ID="com.github.albanread.$BASENAME"
PRIV="${ROS_PRIVATE:-${ROOT:h}/ROS_PRIVATE}"
ROM="${ROM:-$PRIV/roms/pi/v530/RISCOS.IMG}"
FS_ZIP="${FS_ZIP:-$PRIV/dist/end_user_fs.zip}"
BIN="${QEMU_BIN:-$ROOT/build-macos/qemu-system-aarch64}"
OUT="${OUT:-$ROOT/build-macos/release}"
SIGN_ID="${SIGN_ID:--}"
BACKDROP="${BACKDROP:-acorn}"
[[ "$BACKDROP" == (acorn|acorn-live|off) ]] || die "BACKDROP must be acorn, acorn-live or off, not '$BACKDROP'"
NOTARY_PROFILE="${NOTARY_PROFILE:-}"
ENTS="$ROOT/riscos-pi4/app/entitlements.plist"
STRIP_DEFAULT=(
    Apps/DDE Documents/DDE                  # the DDE: licensed to us, not ours to hand out
    Apps/!Store !Boot/Choices/PlingStore    # the Store is not part of this product,
    Apps/!PackMan                           #   and PackMan is its other half
    !Boot/Loader,fc8                        # the Pi's boot partition image: QEMU loads the ROM
    !Boot/Resources/!Ghostscr               # Ghostscript: only !PrintPDF used it
    !Boot/Resources/!ThemeDefs/Themes/{Iyonix,Sovereign,Raspberry,Ursula,Morris4}   # only Acorn is used
    Documents/{Books,Manuals,Images,OvationPro,UserGuide,PipeDream,Other,Music}     # Welcome stays: it is pinned
    Diversions                              # the games
    Utilities/!DPlngScan                    # drives a scanner
)
STRIP="${STRIP-${(j: :)STRIP_DEFAULT}}"
APP="$OUT/$NAME.app"
DMG="$OUT/$NAME.dmg"
STAGE="$OUT/stage"
PB=/usr/libexec/PlistBuddy
HOSTFS_MODS=( "$ROOT/riscos-pi4/hostfs/dde/HostFS,ffa" "$ROOT/riscos-pi4/hostfs/filer/HostFSFiler,ffa" )

die()  { print -u2 "make-release.sh: $*"; exit 1 }
step() { print -- "\n== $*" }
sha()  { shasum -a 256 "$1" | cut -c1-64 }
# a module's version string, from its help string: "2.04 (13 Sep 2026)"
modver() { strings -n 6 "$1" | grep -m1 -oE '[0-9]+\.[0-9]{2} \([0-9]{2} [A-Za-z]{3} [0-9]{4}\)' || print '?' }
# Set a plist key, adding it if the template lacks it
pbset() { "$PB" -c "Set :$1 $3" "$PLIST" 2>/dev/null || "$PB" -c "Add :$1 $2 $3" "$PLIST" }

for t in python3 ninja codesign hdiutil install_name_tool otool unzip zip xattr ditto strings; do
    command -v "$t" >/dev/null || die "need $t on the PATH"
done
[[ -x "$PB" ]] || die "need $PB"
[[ -r "$ROM" ]]    || die "no stock ROM at $ROM (ROM=...)"
[[ -r "$FS_ZIP" ]] || die "no end-user disc zip at $FS_ZIP (FS_ZIP=...)"
for m in $HOSTFS_MODS; do [[ -r "$m" ]] || die "missing module $m"; done
for f in launcher.zsh launcher.c entitlements.plist Info.plist AppIcon.icns; do
    [[ -r "$ROOT/riscos-pi4/app/$f" ]] || die "missing riscos-pi4/app/$f"
done
[[ -n "$NOTARY_PROFILE" && "$SIGN_ID" == "-" ]] && die "NOTARY_PROFILE needs a Developer ID in SIGN_ID"

# 1. the emulator, and its scripting dictionary from the command table
if [[ -z "${NO_BUILD:-}" ]]; then
    step "building the emulator"
    ninja -C "$ROOT/build-macos" | tail -2
fi
[[ -x "$BIN" ]] || die "no emulator at $BIN"
# which Macs this copy is for: the emulator's own architecture
if lipo -archs "$BIN" 2>/dev/null | grep -q x86_64; then
    ARCH_DESC="Intel"
else
    ARCH_DESC="Apple silicon"
fi
python3 "$HERE/mksdef.py" >/dev/null

# 2. a clean stage
step "staging $NAME under $OUT"
# Rename before removing: Finder drops .DS_Store files into folders it is
# showing, and an rm -rf racing that fails with "directory not empty".
for d in "$STAGE" "$APP"; do
    [[ -e "$d" ]] && mv "$d" "$d.old.$$"
done
rm -rf "$STAGE.old.$$" "$APP.old.$$" "$DMG" 2>/dev/null || true
mkdir -p "$STAGE/disc" "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Frameworks"
RES="$APP/Contents/Resources"
PLIST="$APP/Contents/Info.plist"

# 3. the ROM: the stock image with HostFS and its filer appended to the
#    module chain -- rom.zsh's splice, done once here instead of at launch
#    -- and the boot screen: BootFX's Raspberry Pi splash, logo and bar
#    replaced in place by ours (app/bootfx, built by tools/mkbootfx.py)
BOOTFX="$ROOT/riscos-pi4/app/bootfx"
resargs=()
for f in 1920x1080,c85 Logo,c85 Bar24,fca; do
    [[ -r "$BOOTFX/$f" ]] && resargs+=( -r "Resources.BootFX.${f%%,*}=$BOOTFX/$f" )
done
step "ROM: ${ROM:t} + ${HOSTFS_MODS[1]:t} + ${HOSTFS_MODS[2]:t}${resargs:+ + the Acorn boot screen}"
python3 "$HERE/mkrom.py" "$ROM" -m "${HOSTFS_MODS[1]}" -m "${HOSTFS_MODS[2]}" "${resargs[@]}" -o "$RES/RISCOS.IMG"

# 4. the disc
step "disc: ${FS_ZIP:t}"
unzip -q "$FS_ZIP" -d "$STAGE/disc"
[[ -d "$STAGE/disc/!Boot" ]] || die "$FS_ZIP has no !Boot at its top level (the zip must hold the disc's contents, no wrapper directory)"
find "$STAGE/disc" \( -name .DS_Store -o -name __MACOSX -o -name '._*' \) -prune -exec rm -rf {} +
stripped=""
for d in ${=STRIP}; do
    hits=( "$STAGE/disc"/${~d}(N) )
    if (( ${#hits} )); then
        print "leaving off $d"
        rm -rf "${hits[@]}"
        stripped="$stripped $d"
    else
        print "STRIP: no $d on this disc"
    fi
done
# A pin for something left off would greet the user with an error box
# at every boot, so those lines leave the Pinboard task with it.
pins="$STAGE/disc/!Boot/Choices/Boot/Tasks/Pinboard,feb"
if [[ -n "$stripped" && -f "$pins" ]]; then
    for d in ${=stripped}; do
        ref="Boot:^.${d//\//.}"
        if grep -q -F "$ref" "$pins"; then
            print "unpinning $ref"
            grep -v -F "$ref" "$pins" > "$pins.new"
            mv -f "$pins.new" "$pins"
        fi
    done
fi
# The backdrop: the pinboard tiles a sprite whose pixels carry the "below"
# layer tag in their transfer byte, so the host's layer shows through the
# background, and the Wimp stops filling boxes behind pinboard icon names
# (they would cover the layer).  Without the layer the tile is the sage
# ground the watermark sat on.
backdrop_note="none"
if [[ "$BACKDROP" != off ]]; then
    theme="$STAGE/disc/!Boot/Resources/!ThemeDefs/Themes/Acorn"
    [[ -d "$theme" ]] || die "no Acorn theme on the disc for the backdrop tile: $theme"
    python3 "$HERE/mkbacktile.py" --out "$theme/BackTile,ff9" >/dev/null
    python3 - "$STAGE/disc" <<'PY' || die "the disc's PinSetup or ThemeSetup is not the shape the backdrop patch expects"
import sys
disc = sys.argv[1]
def patch(rel, old, new, done):
    path = f"{disc}/{rel}"
    s = open(path, "rb").read().decode("latin-1")
    if done in s:
        return
    if s.count(old) != 1:
        sys.exit(f"{rel}: expected one '{old}'")
    open(path, "wb").write(s.replace(old, new).encode("latin-1"))
patch("!Boot/Choices/Boot/Tasks/PinSetup,feb",
      "Backdrop -Centre Boot:Resources.!ThemeDefs.Themes.Acorn.Backdrop",
      "Backdrop -Tile Boot:Resources.!ThemeDefs.Themes.Acorn.BackTile",
      "Themes.Acorn.BackTile")
patch("!Boot/Choices/Boot/PreDesk/ThemeSetup,feb",
      "WimpVisualFlags -RemoveIconBoxes",
      "WimpVisualFlags -RemoveIconBoxes -NoIconBoxesInTransWindows",
      "-NoIconBoxesInTransWindows")
PY
    backdrop_note="$BACKDROP (the pinboard tiles BackTile, the tagged sprite)"
    print "backdrop: $BACKDROP; the disc tiles the tagged sprite"
fi

# The CMOS: the disc's own, with FileSystem HostFS forced so the app
# boots from the disc whatever the snapshot last said.  The same blob
# ships as cmos.bin, to seed a disc that has lost its CMOS,ff2.
if [[ -e "$STAGE/disc/CMOS,ff2" ]]; then
    cmos_base="$STAGE/disc/CMOS,ff2"
elif [[ -e "$ROOT/riscos-images/cmos.bin" ]]; then
    cmos_base="$ROOT/riscos-images/cmos.bin"
else
    die "the disc has no CMOS,ff2 and there is no riscos-images/cmos.bin to start from"
fi
python3 "$HERE/mkcmos.py" --symbols "$HERE/cmos-symbols-530.json" \
    --base "$cmos_base" --filesystem 220 -o "$RES/cmos.bin" >/dev/null
cp "$RES/cmos.bin" "$STAGE/disc/CMOS,ff2"
( cd "$STAGE/disc" && zip -q -r -X "$RES/Disc.zip" . )
cp "$RES/Disc.zip" "$OUT/$NAME-disc.zip"
disc_files=$(zipinfo -1 "$RES/Disc.zip" | grep -vc '/$')   # files, not directories
print "$disc_files files, $(du -sh "$STAGE/disc" | cut -f1) unpacked, $(du -h "$RES/Disc.zip" | cut -f1) zipped"

# 5. the bundle
step "bundle"
cp "$BIN" "$APP/Contents/MacOS/qemu-system-aarch64"
cp "$ROOT/riscos-pi4/app/launcher.zsh" "$RES/launcher.zsh"
# The launcher stub is the app's main executable, so it must be the same
# architecture as the emulator it runs -- take that from the emulator
# rather than hard-coding it, or an Intel (x86_64) build gets an arm64 main
# executable the Mac cannot launch (and the reverse on Apple silicon).
LARCH=$(lipo -archs "$BIN" 2>/dev/null | awk '{print $1}')
[[ -n "$LARCH" ]] || LARCH=$(uname -m)
clang -arch "$LARCH" -mmacosx-version-min=11.0 -O2 -Wall -framework CoreGraphics \
    -o "$APP/Contents/MacOS/$NAME" "$ROOT/riscos-pi4/app/launcher.c" || die "the launcher stub did not compile"
print "launcher and emulator: $LARCH"
chmod 755 "$APP/Contents/MacOS/$NAME" "$APP/Contents/MacOS/qemu-system-aarch64" "$RES/launcher.zsh"
cp "$ROOT/riscos-pi4/app/AppIcon.icns" "$ROOT/riscos-pi4/app/RISCOSQEMU.sdef" "$RES/"
cp "$ROOT/riscos-pi4/app/Info.plist" "$PLIST"
pbset CFBundleExecutable string "$NAME"
pbset CFBundleName string "$NAME"
pbset CFBundleDisplayName string "$NAME"
pbset CFBundleIdentifier string "$ID"
pbset CFBundleShortVersionString string "$N"
pbset CFBundleVersion string "$N"
pbset NSHighResolutionCapable bool true
pbset NSHumanReadableCopyright string "Built on QEMU (GPL v2). RISC OS is copyright RISC OS Open Ltd."
pbset RISCOSBackdrop string "$BACKDROP"
for k in NSDocumentsFolderUsageDescription NSDesktopFolderUsageDescription NSDownloadsFolderUsageDescription NSRemovableVolumesUsageDescription NSNetworkVolumesUsageDescription; do
    pbset $k string "RISC OS keeps its disc in the folder you chose for it."
done

# 6. the libraries
step "libraries"
python3 - "$APP" <<'PY'
import os, shutil, subprocess, sys
app = sys.argv[1]
fw = os.path.join(app, "Contents", "Frameworks")
exe = os.path.join(app, "Contents", "MacOS", "qemu-system-aarch64")
NEW = "@executable_path/../Frameworks/"
HOMEBREW = ("/opt/homebrew/", "/usr/local/")

def run(*a):
    return subprocess.run(a, capture_output=True, text=True, check=True).stdout

def loads(path):          # every dylib a file names; a dylib's own id comes first
    lines = run("otool", "-L", path).splitlines()[1:]
    return [l.split()[0] for l in lines if l.strip()]

def rpaths(path):
    paths, grab = [], False
    for l in run("otool", "-l", path).splitlines():
        if "LC_RPATH" in l:
            grab = True
        elif grab and l.strip().startswith("path "):
            paths.append(l.split()[1]); grab = False
    return paths

def is_system(ref):
    return ref.startswith("/usr/lib/") or ref.startswith("/System/")

def resolve(ref, referrer):
    if ref.startswith(("@loader_path/", "@rpath/", "@executable_path/")):
        cand = os.path.join(os.path.dirname(referrer), ref.split("/", 1)[1])
    else:
        cand = ref
    return os.path.realpath(cand)

copied = {}          # real source path -> leaf name in Frameworks
edits = {exe: []}    # bundled file -> [(old ref, new ref)]
todo = [(exe, exe)]  # (file in the bundle to edit, original to read from)
while todo:
    target, source = todo.pop()
    for ref in loads(source):
        if is_system(ref):
            continue
        real = resolve(ref, source)
        if real == os.path.realpath(source):
            continue                   # the dylib's own id
        if not os.path.exists(real):
            sys.exit(f"cannot resolve {ref} (from {source})")
        if real not in copied:
            ident = run("otool", "-D", real).splitlines()[1].strip()
            leaf = os.path.basename(ident or real)
            copied[real] = leaf
            dst = os.path.join(fw, leaf)
            shutil.copy2(real, dst)
            os.chmod(dst, 0o755)
            edits[dst] = []
            todo.append((dst, real))
        edits[target].append((ref, NEW + copied[real]))

# rewrite: each library's id, every reference, and drop Homebrew rpaths
for target, pairs in edits.items():
    args = ["install_name_tool"]
    if target != exe:
        args += ["-id", NEW + os.path.basename(target)]
    for old, new in pairs:
        args += ["-change", old, new]
    for rp in rpaths(target):
        if rp.startswith(HOMEBREW):
            args += ["-delete_rpath", rp]
    subprocess.run(args + [target], check=True, capture_output=True)

# nothing may still point outside the app or the system, ids included
bad = []
for f in edits:
    for ref in loads(f):
        if not (is_system(ref) or ref.startswith(NEW)):
            bad.append((os.path.basename(f), ref))
    for rp in rpaths(f):
        if rp.startswith(HOMEBREW):
            bad.append((os.path.basename(f), "rpath " + rp))
if bad:
    sys.exit("still pointing outside the app: " + ", ".join(f"{f} -> {r}" for f, r in bad))
for real, leaf in sorted(copied.items(), key=lambda kv: kv[1]):
    print(f"  {leaf:32} <- {real}")
print(f"{len(copied)} libraries")
PY

# the app needs the newest macOS any of its binaries was built for
minos=$(
    for f in "$APP/Contents/MacOS/qemu-system-aarch64" "$APP"/Contents/Frameworks/*.dylib; do
        otool -l "$f" | awk '$1=="minos"{print $2} /LC_VERSION_MIN_MACOSX/{f=1} f&&$1=="version"{print $2; f=0}'
    done | sort -t. -k1,1n -k2,2n | tail -1
)
[[ -n "$minos" ]] || die "could not read a minimum macOS version from the binaries"
pbset LSMinimumSystemVersion string "$minos"
print "minimum macOS: $minos"

# 7. the record of what went in
step "provenance"
commit=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || print unknown)
branch=$(git -C "$ROOT" branch --show-current 2>/dev/null || print unknown)
dirty=$(git -C "$ROOT" status --porcelain 2>/dev/null | wc -l | tr -d ' ')
{
    print "$NAME"
    print "built $(date -u '+%Y-%m-%d %H:%M UTC') on macOS $(sw_vers -productVersion) $(uname -m)"
    print ""
    print "emulator   RISCOSQEMUA72 $commit ($branch${dirty:+, $dirty uncommitted change(s)})"
    print "           $("$BIN" --version | head -1)"
    print "ROM        ${ROM:t} sha256 $(sha "$ROM")"
    print "           + HostFS $(modver "${HOSTFS_MODS[1]}"), HostFSFiler $(modver "${HOSTFS_MODS[2]}")"
    print "           ${resargs:+Acorn boot screen (app/bootfx) spliced in; }spliced sha256 $(sha "$RES/RISCOS.IMG")"
    print "disc       ${FS_ZIP:t} sha256 $(sha "$FS_ZIP")"
    print "           $disc_files files${stripped:+; left off:$stripped}; CMOS forced to FileSystem HostFS"
    print "minimum    macOS $minos, $ARCH_DESC"
    print "backdrop   $backdrop_note"
    print "signing    $SIGN_ID${NOTARY_PROFILE:+; notarized and stapled}"
    print "libraries  $(ls "$APP/Contents/Frameworks" | tr '\n' ' ')"
} > "$RES/RELEASE.txt"
sed 's/^/  /' "$RES/RELEASE.txt"

# 8. the Read Me, into the app and beside it on the disk image
cat > "$RES/ReadMe.txt" <<EOF
$NAME -- RISC OS 5.30 on your Mac

WHAT IT IS
  RISC OS 5.30, RISC OS Open's Raspberry Pi build, running in its own
  window on an emulated Raspberry Pi 4.  Everything it needs is inside
  the app.

YOU NEED
  A Mac running macOS $minos or later.  This copy is for $ARCH_DESC Macs;
  the release page has the other.

INSTALLING
  Drag $NAME to Applications, or run it from wherever it is.
$(if [[ -n "$NOTARY_PROFILE" ]]; then cat <<EON
  It is signed and notarised, so it opens like any other app; the first
  time, macOS just confirms that you downloaded it.
EON
else cat <<EON
  This release is not notarised, so the first time macOS will decline
  to open it.  Open System Settings > Privacy & Security, scroll down,
  click "Open Anyway", and open it again.  Or, in Terminal:
      xattr -dr com.apple.quarantine /Applications/$NAME.app
EON
fi)

FIRST RUN
  The app asks where to keep the RISC OS disc: a folder called RISCOS
  in your home folder, or one you choose.  It puts the disc there and
  boots from it -- about twenty seconds to the desktop.  That folder IS
  the RISC OS disc: put a file there on the Mac and it is inside RISC
  OS, as HostFS; save a file in RISC OS and it appears there.  A later
  version never replaces a disc that is already there.
  To keep the disc somewhere else, hold down the Option key as the app
  starts and it asks again; move or copy the folder first if you want
  to keep what is in it.

USER GUIDE
  https://github.com/albanread/Aldershot/blob/main/mac/user-guide.md

USING IT
  Select is a click, Menu is Control-click, Adjust is Command-click.
  Close the window to switch RISC OS off, as you would a real machine.
  The desktop starts at 800x600; pick another size in RISC OS's own
  Display Manager and the window follows.  Resize the window freely --
  the picture scales.

SETTINGS (optional, in Terminal)
  defaults write $ID mode 1920x1200    start the desktop at that size
  defaults write $ID backdrop acorn-live   a gently moving backdrop; or acorn,
                                       none, tile:<picture> or picture:<picture>
  defaults write $ID backdrop off      no backdrop layer and no Backdrop menu
  defaults write $ID disc ~/Elsewhere  the disc folder, as the dialog sets it
  defaults delete $ID                  forget both; the app asks again

IF SOMETHING GOES WRONG
  ~/Library/Logs/$BASENAME/run.log and metal-debug.txt say what happened.
  Delete CMOS,ff2 in the disc folder to reset RISC OS's configuration;
  delete the folder to start again from a fresh disc.

LICENCES
  Built on QEMU, GNU GPL v2 (https://www.qemu.org).  RISC OS is
  copyright RISC OS Open Ltd, Apache 2.0 (https://www.riscosopen.org).
  The applications on the disc belong to their authors.  Source for
  the emulator: https://github.com/albanread/RISCOSQEMUA72
EOF

# 9. signing: the libraries, then the emulator with the entitlements it
#    was built with, then the bundle as a whole (its main executable is
#    the launcher script, sealed with the resources)
step "signing (${SIGN_ID})"
sign() {   # codesign, quiet unless it fails: "replacing existing signature" is not news
    if ! codesign "$@" 2>"$STAGE/codesign.err"; then
        cat "$STAGE/codesign.err" >&2
        die "codesign failed on ${@[-1]}"
    fi
}
signopts=()
[[ "$SIGN_ID" != "-" ]] && signopts=(--options runtime --timestamp)
xattr -cr "$APP"
for f in "$APP"/Contents/Frameworks/*.dylib; do
    sign -f -s "$SIGN_ID" "${signopts[@]}" "$f"
done
sign -f -s "$SIGN_ID" "${signopts[@]}" --entitlements "$ENTS" "$APP/Contents/MacOS/qemu-system-aarch64"
sign -f -s "$SIGN_ID" "${signopts[@]}" "$APP/Contents/MacOS/$NAME"
sign -f -s "$SIGN_ID" "${signopts[@]}" "$APP"
codesign --verify --deep --strict "$APP" || die "the signature does not verify"
print "signature verifies (${SIGN_ID}${signopts:+; hardened runtime, timestamped})"

# 9b. notarization: the app first, stapled; the disk image after it is made
notarize() {   # notarize <file>: submit, wait, insist on Accepted
    local out="$STAGE/notary-${1:t}.json" id verdict   # not "status": read-only in zsh
    xcrun notarytool submit "$1" --keychain-profile "$NOTARY_PROFILE" --wait \
        --output-format json >"$out" 2>"$STAGE/notary.err" || {
        cat "$STAGE/notary.err" >&2; die "notarytool submit failed for ${1:t}"
    }
    id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["id"])' "$out")
    verdict=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["status"])' "$out")
    print "notarization of ${1:t}: $verdict (submission $id)"
    if [[ "$verdict" != "Accepted" ]]; then
        xcrun notarytool log "$id" --keychain-profile "$NOTARY_PROFILE" >&2 || true
        die "Apple did not accept ${1:t}"
    fi
}
if [[ -n "$NOTARY_PROFILE" ]]; then
    step "notarizing the app (keychain profile $NOTARY_PROFILE)"
    ditto -c -k --keepParent "$APP" "$STAGE/$NAME.zip"
    notarize "$STAGE/$NAME.zip"
    xcrun stapler staple "$APP" >/dev/null || die "stapling the app failed"
fi

# 10. the disk image
step "disk image"
mkdir -p "$STAGE/dmg"
ditto "$APP" "$STAGE/dmg/$NAME.app"
ln -s /Applications "$STAGE/dmg/Applications"
cp "$RES/ReadMe.txt" "$STAGE/dmg/Read Me.txt"
hdiutil create -volname "$NAME" -srcfolder "$STAGE/dmg" -ov -format UDZO -quiet "$DMG"
[[ "$SIGN_ID" != "-" ]] && sign -f -s "$SIGN_ID" --timestamp "$DMG"
if [[ -n "$NOTARY_PROFILE" ]]; then
    notarize "$DMG"
    xcrun stapler staple "$DMG" >/dev/null || die "stapling the disk image failed"
    xcrun stapler validate "$APP" >/dev/null && xcrun stapler validate "$DMG" >/dev/null \
        && print "staples validate" || die "a staple does not validate"
fi
mv "$STAGE" "$STAGE.old.$$" && rm -rf "$STAGE.old.$$" 2>/dev/null || true

step "Gatekeeper's verdict"
spctl --assess --type execute -vv "$APP" 2>&1 | sed 's/^/  /' || true
[[ -n "$NOTARY_PROFILE" ]] && { spctl --assess --type open --context context:primary-signature -vv "$DMG" 2>&1 | sed 's/^/  /' || true; }

step "done"
print "app   $APP  ($(du -sh "$APP" | cut -f1))"
print "dmg   $DMG  ($(du -h "$DMG" | cut -f1), sha256 $(sha "$DMG"))"
print "disc  $OUT/$NAME-disc.zip  ($(du -h "$OUT/$NAME-disc.zip" | cut -f1))"
print "try   open -n \"$APP\""
