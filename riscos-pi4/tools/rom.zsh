# rom.zsh — which ROM image and CMOS a launcher boots.  Sourced by
# run-macos.sh and run-app.sh, so the developer's machine and the user's
# start the same way.
#
#   rom_to_boot <images dir>     sets ROM to the image for -kernel
#   cmos_to_boot <images dir>    sets CMOS to the blob for the loader
#
#   RISCOS_BOOT           "hostfs" boots from the share (FSDESIGN-V1.md §13
#                         B1): the CMOS gets FileSystem HostFS, written by
#                         mkcmos.py to cmos-hostfs.bin beside cmos.bin and
#                         remade when cmos.bin changes.  Unset boots as
#                         cmos.bin says.  A card stays attached either way,
#                         and reachable as SDFS::0.
#
#   RISCOS_MODULES        space-separated module files spliced into the ROM
#                         before boot (BOOTDESIGN.md §3; order = init order).
#                         The spliced image is cached beside the stock one
#                         and rebuilt only when a module changes.
#   RISCOS_HOSTFS         a share.  A share brings HostFS and its icon-bar
#                         filer with it, spliced ahead of RISCOS_MODULES, so
#                         no machine starts with a share and no filing
#                         system to reach it (FSDESIGN-V1.md §13, R1, R2, D1).
#   RISCOS_HOSTFS_MODULES the builds to splice for a share (default: the ones
#                         committed beside their sources,
#                         hostfs/dde/HostFS,ffa hostfs/filer/HostFSFiler,ffa);
#                         set it empty to splice none.  A module whose title
#                         is already in RISCOS_MODULES stands instead.
#
# With no modules to splice, the stock RISCOS.IMG is booted untouched.

_ROM_ZSH_DIR="${${(%):-%x}:A:h}"        # tools/, while this is being sourced

# The module's title, from the offset at +16 of its header.
_rom_module_title() {
    python3 -c 'import struct, sys
b = open(sys.argv[1], "rb").read()
t = struct.unpack_from("<I", b, 16)[0]
print(b[t:b.index(b"\0", t)].decode("latin-1"))' "$1"
}

rom_to_boot() {
    local images="$1" here="$_ROM_ZSH_DIR"
    local -a mods
    local m key hm title
    local -a hostfs_mods titles

    ROM="$images/RISCOS.IMG"
    mods=(${=RISCOS_MODULES:-})
    for m in $mods; do
        [[ -e "$m" ]] || { print -u2 "RISCOS_MODULES: missing: $m"; return 1; }
    done

    if [[ -n "${RISCOS_HOSTFS:-}" ]]; then
        local list="${here:h}/hostfs/dde/HostFS,ffa ${here:h}/hostfs/filer/HostFSFiler,ffa"
        list="${RISCOS_HOSTFS_MODULES-$list}"
        hostfs_mods=(${=list})
        for m in $mods; do
            titles+=("$(_rom_module_title "$m")")
        done
        local -a add
        for hm in $hostfs_mods; do
            [[ -e "$hm" ]] || {
                print -u2 "RISCOS_HOSTFS_MODULES: missing: $hm"
                return 1
            }
            title="$(_rom_module_title "$hm")"
            (( ${titles[(Ie)$title]} )) || add+=("$hm")
        done
        mods=($add $mods)
    fi
    (( ${#mods} )) || return 0

    # Cached under a hash of the stock ROM and every module's contents, so
    # a module edit rebuilds it and a relaunch does not.
    key=$( (shasum -a 256 "$ROM" $mods; shasum -a 256 $mods) \
           | shasum -a 256 | cut -c1-16 )
    ROM="$images/RISCOS-$key.IMG"
    if [[ ! -e "$ROM" ]]; then
        print "rom: splicing ${#mods} module(s) -> ${ROM:t}"
        local -a modargs=( -o "$ROM" )
        for m in $mods; do modargs+=( -m "$m" ); done
        "$here/mkrom.py" "$images/RISCOS.IMG" $modargs || return 1
    else
        print "rom: cached ${ROM:t} (${#mods} module(s))"
    fi
}

cmos_to_boot() {
    local images="$1" here="$_ROM_ZSH_DIR" out

    CMOS="$images/cmos.bin"
    case "${RISCOS_BOOT:-}" in
    "")
        return 0
        ;;
    hostfs)
        [[ -n "${RISCOS_HOSTFS:-}" ]] || {
            print -u2 "RISCOS_BOOT=hostfs: set RISCOS_HOSTFS to the share to boot"
            return 1
        }
        out="$images/cmos-hostfs.bin"
        if [[ ! -e "$out" || "$CMOS" -nt "$out" ]]; then
            # 220 is HostFS's filing system number (hostfs/dde/s.head)
            python3 "$here/mkcmos.py" --symbols "$here/cmos-symbols-530.json" \
                --base "$CMOS" --filesystem 220 -o "$out" 2>/dev/null || {
                print -u2 "cmos: mkcmos.py failed"
                return 1
            }
            print "cmos: FileSystem HostFS -> ${out:t}"
        fi
        CMOS="$out"
        ;;
    *)
        print -u2 "RISCOS_BOOT: '$RISCOS_BOOT' is not a boot source (hostfs, or unset)"
        return 1
        ;;
    esac
}
