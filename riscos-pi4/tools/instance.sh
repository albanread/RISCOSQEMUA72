#!/bin/zsh
#
# Isolated, named machines, so several sessions can each run their own.
#
#   instance.sh create <name>     make the instance directory
#   instance.sh start  <name> [-- extra qemu args]
#   instance.sh stop   <name>     QMP quit, then the pidfile — never by name
#   instance.sh status [<name>]   which instances exist, which are running
#   instance.sh path   <name>     print the instance directory
#   instance.sh qmp    <name>     print its QMP socket, for tools
#
# Each instance owns, under $RISCOS_INSTANCES/<name>/:
#
#   images/RISCOS.IMG   a COPY — ROM experiments elsewhere cannot reach it
#   images/cmos.bin     a COPY, for the same reason
#   images/card.img     a symlink to the shared card; safe to share, since
#                       run-macos.sh passes snapshot=on and QEMU only ever
#                       opens it read-only
#   share/              its own HostFS root
#   qmp.sock            its own QMP endpoint — a unix socket, so no port can
#                       collide with another machine's
#   qemu.pid            written by QEMU; how stop finds exactly this machine
#   trace.txt           VMCH_TRACE for this machine only
#   run.log             QEMU's stdout/stderr
#   metal-debug.txt     lands here, because start runs from this directory
#
# What an instance deliberately does NOT own: the QEMU binary.  It runs
# build-macos/qemu-system-aarch64, so a rebuild by anyone is picked up at
# that instance's next start — not by a machine already running, which
# keeps the binary it was started with.  Set QEMU_BIN to pin one.
#
# The rule this exists to enforce: never `pkill -f qemu-system-aarch64`.
# That kills every session's machine on the host.
#
set -e
HERE="${0:A:h}"
ROOT="${HERE:h:h}"
INSTANCES="${RISCOS_INSTANCES:-${ROOT:h}/instances}"
SHARED_IMAGES="${RISCOS_IMAGES:-$ROOT/riscos-images}"

die() { print -u2 "instance.sh: $*"; exit 1; }

name_ok() {
    [[ "$1" =~ '^[A-Za-z0-9][A-Za-z0-9._-]*$' ]] ||
        die "bad instance name '$1' (letters, digits, . _ -)"
}

idir() { print -r -- "$INSTANCES/$1"; }

# The PID in the pidfile, but only if that process is still QEMU running
# under this instance's name.  A pidfile outlives its machine, and a PID can
# be reused by anything, so both checks are needed before it is trusted.
live_pid() {
    local d="$(idir "$1")" pid cmd
    [[ -r "$d/qemu.pid" ]] || return 1
    pid="$(<"$d/qemu.pid")"
    [[ "$pid" =~ '^[0-9]+$' ]] || return 1
    cmd="$(ps -p "$pid" -o command= 2>/dev/null)" || return 1
    [[ "$cmd" == *qemu-system-aarch64* && "$cmd" == *"-name $1"* ]] || return 1
    print -r -- "$pid"
}

qmp_quit() {
    python3 - "$1" <<'PY'
import json, socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(sys.argv[1])
f = s.makefile("rwb")
f.readline()                                   # greeting
for cmd in ("qmp_capabilities", "quit"):
    f.write((json.dumps({"execute": cmd}) + "\r\n").encode()); f.flush()
    while True:
        m = json.loads(f.readline() or b"{}")
        if "event" not in m:
            break
PY
}

cmd="${1:-}"; shift || true

case "$cmd" in
create)
    [[ -n "${1:-}" ]] || die "usage: instance.sh create <name>"
    name_ok "$1"
    d="$(idir "$1")"
    [[ -e "$d" ]] && die "$d already exists"
    for f in RISCOS.IMG cmos.bin; do
        [[ -r "$SHARED_IMAGES/$f" ]] || die "missing $SHARED_IMAGES/$f"
    done
    mkdir -p "$d/images" "$d/share"
    cp "$SHARED_IMAGES/RISCOS.IMG" "$SHARED_IMAGES/cmos.bin" "$d/images/"
    if [[ -e "$SHARED_IMAGES/card.img" ]]; then
        ln -s "${SHARED_IMAGES:A}/card.img" "$d/images/card.img"
    fi
    print "created $d"
    ;;

start)
    [[ -n "${1:-}" ]] || die "usage: instance.sh start <name> [-- qemu args]"
    name_ok "$1"; n="$1"; shift
    [[ "${1:-}" == "--" ]] && shift
    d="$(idir "$n")"
    [[ -d "$d" ]] || die "no instance '$n' (instance.sh create $n)"
    if pid="$(live_pid "$n")"; then
        die "'$n' is already running (pid $pid)"
    fi
    rm -f "$d/qmp.sock" "$d/qemu.pid"
    cd "$d"                                     # metal-debug.txt lands here
    RISCOS_IMAGES="$d/images" \
    RISCOS_HOSTFS="${RISCOS_HOSTFS:-$d/share}" \
    RISCOS_QMP="unix:$d/qmp.sock,server,nowait" \
    RISCOS_NAME="$n" \
    RISCOS_PIDFILE="$d/qemu.pid" \
    VMCH_TRACE="$d/trace.txt" \
        nohup "$HERE/run-macos.sh" "$@" >"$d/run.log" 2>&1 &!
    for i in {1..50}; do                        # up to 5 s for QMP to appear
        [[ -S "$d/qmp.sock" ]] && break
        sleep 0.1
    done
    [[ -S "$d/qmp.sock" ]] || die "'$n' did not come up — see $d/run.log"
    print "started '$n' (pid $(<"$d/qemu.pid"))  qmp: $d/qmp.sock"
    ;;

stop)
    [[ -n "${1:-}" ]] || die "usage: instance.sh stop <name>"
    name_ok "$1"; n="$1"; d="$(idir "$n")"
    pid="$(live_pid "$n")" || { print "'$n' is not running"; exit 0; }
    # Clean first: a hard kill is yanking the power mid-write.
    if [[ -S "$d/qmp.sock" ]] && qmp_quit "$d/qmp.sock" 2>/dev/null; then
        for i in {1..50}; do
            kill -0 "$pid" 2>/dev/null || { print "stopped '$n'"; exit 0; }
            sleep 0.1
        done
    fi
    # Only this PID, and only because live_pid proved it is this machine.
    print -u2 "'$n' did not quit over QMP; sending SIGTERM to pid $pid"
    kill -TERM "$pid"
    ;;

status)
    [[ -d "$INSTANCES" ]] || { print "no instances under $INSTANCES"; exit 0; }
    for d in "$INSTANCES"/*(N/); do
        n="${d:t}"
        [[ -n "${1:-}" && "$n" != "$1" ]] && continue
        if pid="$(live_pid "$n")"; then
            print "$n\trunning (pid $pid)\t$d/qmp.sock"
        else
            print "$n\tstopped"
        fi
    done
    ;;

path) name_ok "${1:?}"; print -r -- "$(idir "$1")" ;;
qmp)  name_ok "${1:?}"; print -r -- "$(idir "$1")/qmp.sock" ;;

*)
    sed -n '3,11p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
