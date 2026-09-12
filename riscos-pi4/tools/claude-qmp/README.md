# claude-qmp — the QMP tooling the HostFS session drove machines with

These lived only in one Claude session's scratchpad on the Mac. They are
here so the work can be consolidated, not because they are the house
tools: the Windows team's `riscos-pi4/tools/keys.py` and `farm.py` cover
the same ground and are probably the ones to keep. What these carry that
may be worth merging into those is listed per file.

All of them find `qmp.py` beside themselves, and all speak to one machine
at a time over QMP.

## Pointing them at a machine

    export QMP_SOCK=$(riscos-pi4/tools/instance.sh qmp <name>)

`QMP_SOCK` names a unix socket, which is what `instance.sh` gives each
machine. Without it `qmp.py` falls back to TCP `127.0.0.1:4455`, which
only one machine on a host can own.

## The files

| File | What it does | Worth keeping |
| --- | --- | --- |
| `qmp.py` | minimal QMP client; `qmp.py <command> '<json args>'` | unix-socket support via `QMP_SOCK` |
| `keys.py` | `keys.py type 'text'`, `keys.py tap <qcode> [mods]` | three fixes, below |
| `mouse.py` | `mouse.py X Y [left\|right\|middle\|move]`, guest pixels | — |
| `shot.sh` | `shot.sh name` → `name.ppm` + `name.png` via screendump | — |
| `bootwatch.py` | waits for a settled screen, saves it as `desktop.ppm` | the reference the next two need |
| `waitdesk.py` | waits until the framebuffer **equals** `desktop.ppm` | see "boot detection" |
| `boottime.py` | launches a machine and times it to that desktop | as above |
| `elf2bin.py` | `.text` of an ELF object as a flat binary | the `mboxtest`/`bench` recipe on a Mac with no `ld.lld` |

### keys.py: three things learned the hard way

1. **The guest keymap is UK.** `"` is shift-2, `@` is shift-apostrophe,
   `#` is the backslash key, `~` shift-backslash, `\` the ISO `less` key.
2. **Hold shift across a run of shifted characters.** Toggling it per key
   dropped characters in the USB HID path: `"HostFS` arrived as `"OSTFS`.
3. **Pause after every newline.** Keys arriving while the Task Window is
   still running the previous command are dropped — observed losing the
   first one or two characters of the next line (`*Delete chostfs`,
   `*opy`), which once silently rebuilt a module from stale source.

### Boot detection

"Wait until the screen stops changing" fires at 0.2 s, on the blank
framebuffer before anything is drawn. A booted desktop is pixel-identical
from run to run and between `-display none` and `-display metal`, so the
reliable test is to capture it once with `bootwatch.py` and then wait for
the framebuffer's hash to match it exactly. The reference is specific to
one ROM, CMOS and card; recapture when any of them changes.

## Typing into a Task Window, not the F12 line

Everything here was driven through a Task Window (Ctrl-F12), which avoids
two traps `ROS_PRIVATE/docs/driving-the-emulator.md` records for the F12
command line: an empty Return leaves it, and the desktop cannot redraw
while it is open.
