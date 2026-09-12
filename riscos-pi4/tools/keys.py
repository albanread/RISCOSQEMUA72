#!/usr/bin/env python3
"""Type at a farm machine over QMP, and read the screen back.

    keys.py alpha --f12 --line "Cat HostFS:" --shot
    keys.py alpha --line "Run HostFS:$.hello" --shot --wait 4

Uses `input-send-event` with explicit key down/up, never `send-key`: the
latter goes through a delayed queue and interleaves with immediate events,
which scrambles the shift state halfway through a word.

The guest keymap is UK, so the shifted punctuation is not the US
arrangement - `"` is shift-2, `@` is shift-apostrophe, `#` is the backslash
key. Only the characters a command line actually needs are mapped; anything
else raises rather than silently typing something else.
"""

import argparse
import json
import os
import socket
import sys
import time

FARM = r"F:\RISCOSDEV\qemu-farm"
PORTS = {"alpha": 4471, "bravo": 4472, "charlie": 4473, "delta": 4474}

HOLD = 0.06          # how long a key stays down
GAP = 0.06           # and the pause before the next one
MODGAP = 0.03        # settle after pressing a modifier, and before releasing

# These are slower than they look like they need to be, and they are slower
# than they used to be, because 0.03/0.03 loses keystrokes. The guest polls
# the emulated USB keyboard on its own schedule; when four machines are
# running, or one is busy, transitions that fall between two polls are never
# seen. What you get is not a dropped character but a scrambled line - a
# shift release goes missing and the rest of the word arrives capitalised -
# so `$.othello,ff8` types as `$>OHELL<FF*` and you go looking for a
# filing system fault. Twice. Typing a command line is not a benchmark.
SETTLE = 1.5         # for a window to open and take the caret

# qcode, and whether shift is held. UK layout.
KEYS = {}
for ch in "abcdefghijklmnopqrstuvwxyz":
    KEYS[ch] = (ch, False)
    KEYS[ch.upper()] = (ch, True)
for ch in "0123456789":
    KEYS[ch] = (ch, False)

KEYS.update({
    " ": ("spc", False),
    "\n": ("ret", False),
    "\r": ("ret", False),
    "\t": ("tab", False),
    ".": ("dot", False),
    ",": ("comma", False),
    "/": ("slash", False),
    "-": ("minus", False),
    "=": ("equal", False),
    ";": ("semicolon", False),
    ":": ("semicolon", True),
    "'": ("apostrophe", False),
    "@": ("apostrophe", True),
    "[": ("bracket_left", False),
    "]": ("bracket_right", False),
    "#": ("backslash", False),          # UK: the key right of apostrophe
    "~": ("backslash", True),
    "\\": ("less", False),              # UK: the key left of Z
    "|": ("less", True),
    "!": ("1", True),
    '"': ("2", True),
    "$": ("4", True),
    "%": ("5", True),
    "^": ("6", True),
    "&": ("7", True),
    "*": ("8", True),
    "(": ("9", True),
    ")": ("0", True),
    "_": ("minus", True),
    "+": ("equal", True),
    "<": ("comma", True),
    ">": ("dot", True),
    "?": ("slash", True),
})


MODIFIERS = {"ctrl", "ctrl_r", "shift", "shift_r", "alt", "altgr", "meta_l"}


def split_chord(spec):
    """'ctrl+f12' -> (['ctrl'], 'f12').  A bare 'f12' -> ([], 'f12')."""
    parts = spec.split("+")
    keys = [p.strip().lower() for p in parts if p.strip()]
    if not keys:
        raise SystemExit(f"empty key spec {spec!r}")
    mods, base = keys[:-1], keys[-1]
    for mod in mods:
        if mod not in MODIFIERS:
            raise SystemExit(f"{mod!r} is not a modifier; try {sorted(MODIFIERS)}")
    return mods, base


class Qmp:
    def __init__(self, port, timeout=20.0):
        self.timeout = timeout
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.file = self.sock.makefile("rb")
        self.file.readline()
        self.execute("qmp_capabilities")

    def execute(self, command, arguments=None):
        request = {"execute": command}
        if arguments:
            request["arguments"] = arguments
        self.sock.sendall(json.dumps(request).encode() + b"\n")
        deadline = time.time() + self.timeout
        while time.time() < deadline:
            self.sock.settimeout(max(0.1, deadline - time.time()))
            line = self.file.readline()
            if not line:
                raise RuntimeError("QMP closed")
            try:
                message = json.loads(line)
            except ValueError:
                continue
            if "return" in message:
                return message["return"]
            if "error" in message:
                raise RuntimeError(message["error"]["desc"])
        raise TimeoutError(command)

    def key(self, qcode, down):
        self.execute("input-send-event", {"events": [{
            "type": "key",
            "data": {"down": down,
                     "key": {"type": "qcode", "data": qcode}}}]})

    def tap(self, qcode, shift=False, modifiers=()):
        """One keypress, optionally inside held modifiers.

        Modifiers are pressed in order and released in reverse, so
        ctrl+shift+x releases shift before ctrl, which is what a real
        keyboard does and what the guest's USB driver expects.
        """
        held = list(modifiers)
        if shift and "shift" not in held:
            held.append("shift")
        for mod in held:
            self.key(mod, True)
        if held:
            time.sleep(MODGAP)
        self.key(qcode, True)
        time.sleep(HOLD)
        self.key(qcode, False)
        if held:
            time.sleep(MODGAP)
        for mod in reversed(held):
            self.key(mod, False)
        time.sleep(GAP)

    def text(self, string):
        for ch in string:
            if ch not in KEYS:
                raise SystemExit(f"no key mapped for {ch!r}")
            qcode, shift = KEYS[ch]
            self.tap(qcode, shift)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class Ordered(argparse.Action):
    """Remember every action in the order it was written.

    argparse normally hands back one list per option, which loses the
    ordering between them: the tool then has to pick an order, and
    whatever it picks is wrong half the time.  Opening a window and then
    typing into it is not the same script as typing and then opening a
    window, and the second one silently does nothing at all.
    """

    def __call__(self, parser, namespace, values, option_string=None):
        script = getattr(namespace, "script", None)
        if script is None:
            script = []
            setattr(namespace, "script", script)
        script.append((self.dest, values))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("machine", choices=sorted(PORTS))
    ap.add_argument("--f12", action=Ordered, nargs=0,
                    help="press F12, for the single-line * prompt")
    ap.add_argument("--taskwindow", action=Ordered, nargs=0,
                    help="Ctrl-F12: a scrollable window that keeps the "
                         "desktop multitasking; better for real work")
    ap.add_argument("--line", action=Ordered,
                    help="type this and press Return (repeatable)")
    ap.add_argument("--text", action=Ordered, help="type this, no Return")
    ap.add_argument("--key", action=Ordered,
                    help="tap a qcode, with modifiers: f12, ctrl+f12")
    ap.add_argument("--pause", action=Ordered, type=float,
                    help="wait this many seconds here, e.g. for a window "
                         "to open and take the caret")
    ap.add_argument("--wait", type=float, default=1.5,
                    help="seconds to wait after the last action")
    ap.add_argument("--shot", action="store_true",
                    help="screendump afterwards")
    args = ap.parse_args()

    q = Qmp(PORTS[args.machine])
    try:
        for what, value in getattr(args, "script", []):
            if what == "f12":
                q.tap("f12")
                time.sleep(0.6)
            elif what == "taskwindow":
                q.tap("f12", modifiers=["ctrl"])
                time.sleep(SETTLE)
            elif what == "line":
                q.text(value)
                q.tap("ret")
                time.sleep(0.4)
            elif what == "text":
                q.text(value)
            elif what == "key":
                mods, base = split_chord(value)
                q.tap(base, modifiers=mods)
            elif what == "pause":
                time.sleep(value)

        time.sleep(args.wait)

        if args.shot:
            out = os.path.join(FARM, args.machine, "screen",
                               time.strftime("%Y%m%d-%H%M%S") + ".ppm")
            q.execute("screendump", {"filename": out})
            print(out)
    finally:
        q.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
