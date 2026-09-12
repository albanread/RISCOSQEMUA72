# rom.zsh — which ROM image a launcher boots.  Sourced by run-macos.sh and
# run-app.sh, so the developer's machine and the user's start the same way.
#
#   rom_to_boot <images dir>     sets ROM to the image for -kernel
#
#   RISCOS_MODULES        space-separated module files spliced into the ROM
#                         before boot (BOOTDESIGN.md §3; order = init order).
#                         The spliced image is cached beside the stock one
#                         and rebuilt only when a module changes.
#   RISCOS_HOSTFS         a share.  A share brings the HostFS module with it,
#                         spliced ahead of RISCOS_MODULES, so no machine
#                         starts with a share and no filing system to reach
#                         it (FSDESIGN-V1.md §13, R1 and R2).
#   RISCOS_HOSTFS_MODULE  the HostFS build to splice (default: the one
#                         committed beside its source, hostfs/dde/HostFS,ffa);
#                         set it empty to splice none.  A module titled
#                         HostFS already in RISCOS_MODULES stands instead.
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
    local m key hostfs_mod

    ROM="$images/RISCOS.IMG"
    mods=(${=RISCOS_MODULES:-})
    for m in $mods; do
        [[ -e "$m" ]] || { print -u2 "RISCOS_MODULES: missing: $m"; return 1; }
    done

    if [[ -n "${RISCOS_HOSTFS:-}" ]]; then
        hostfs_mod="${RISCOS_HOSTFS_MODULE-${here:h}/hostfs/dde/HostFS,ffa}"
        for m in $mods; do
            [[ "$(_rom_module_title "$m")" == HostFS ]] && hostfs_mod=""
        done
        if [[ -n "$hostfs_mod" ]]; then
            [[ -e "$hostfs_mod" ]] || {
                print -u2 "RISCOS_HOSTFS_MODULE: missing: $hostfs_mod"
                return 1
            }
            mods=("$hostfs_mod" $mods)
        fi
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
