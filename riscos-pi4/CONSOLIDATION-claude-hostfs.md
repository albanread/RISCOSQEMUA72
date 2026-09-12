# Consolidation note — the Claude HostFS session, 12 Sep 2026

For whoever consolidates the diverged work. This says what the session
produced, where it is, what is measured and what is not — and, plainly,
what it got wrong, so none of that is merged as fact.

## Where the work is

| Where | What |
| --- | --- |
| `riscos-pi4`, up to `cbee066699` | HostFS v1: sprints 1–5 (below). **Already merged**, by fast-forward. |
| `consolidation/claude-hostfs` (this branch) | the same, plus `tools/claude-qmp/` and this note |
| `ROS_PRIVATE` `main` | `docs/hostfs.md`; corrections to `README.md` and `docs/driving-the-emulator.md` |
| `ROS_PRIVATE` `consolidation/claude-hostfs-docs` | the same, under a name that says whose |
| not in git | test instance `claude-hostfs` under `/Volumes/xb/RISCOS/instances/` on the Mac: its share, built modules `HostFSv*`, trace |

## Measured — each with the evidence it rests on

- `*Type`, `BGET#` and `OS_GBPB` on a HostFS handle return correct bytes
  (first, deep and last bytes checked against the host file).
- A 256 KiB read is 1 doorbell round trip; under v0 it was 130 (counted in
  `VMCH_TRACE`).
- 256 KiB copied in and back out is byte-identical (`cmp`).
- `*Ex` shows real filetypes and host-local dates; dotted and `,xxx` names
  open; lookups are case-insensitive (screenshots).
- `*SetType` renames the host file; a module copied out as `name,ffa` copies
  back in as a Module and `RMLoad`s without `SetType`.
- `*Rename` works and keeps the type; `OS_GBPB` 9/10/11 return correct
  records; `OS_FSControl 49` returns saturated free space; wildcards
  resolve.

All on the Mac, on the Pi 4 machine, TCG. **None of it was tested on
Windows**; the Windows team has since made v1 build there (`b71faa65d6`).

## Not measured, or wrong — correct these during consolidation

- **`cbee066699`'s commit message is false.** It says "the static-data half
  of being rommable already holds under the existing DDE build". That was
  inferred from `*HostFSStatus` showing the statics outside the image when
  soft-loaded; a module run from ROM executes in place, and a real splice
  aborted. The message is in shared history and cannot be rewritten.
- **HostFS is not ROM-safe.** Nothing here has run from ROM.
- **`BOOTDESIGN.md` §8 sprint 1's acceptance test is too weak.** "`*Modules`
  lists HostFS with a ROM address" proves the module *loaded* onto the
  chain, not that it runs. A splice that fails passes it. The test should
  require a working result from a spliced ROM with nothing soft-loaded — for
  HostFS a `*Copy` round trip through the doorbell.
- **`BOOTDESIGN.md` §3.3** (the `image_size` / CMOS coupling and the 64 KiB
  headroom) is inference from `mkcmos.py`, never tested.
- The TCG instruction rates quoted in conversation (1639 / 925 M/s) were
  best of three consecutive runs and may carry cold-chip bias.

## Misattributed content on `riscos-pi4`

**`818cbac24b` ("hostfs: write types back") contains someone else's work.**
It was committed with `git add -A … riscos-pi4/` in the shared checkout,
before worktrees were in use, and swept up two uncommitted files belonging
to the Mac colleague's ROM loader:

- `riscos-pi4/tools/mkrom.py` — the whole splicer, 218 lines
- the `RISCOS_MODULES` splicing block in `riscos-pi4/tools/run-macos.sh`

So the loader is on `riscos-pi4`, authored as this session and described
as a HostFS filetype change. The session's other commits contain only its
own files.

## The three conflicts, which are semantic

`riscos-pi4` and `zcode/romloader` both changed these since `e885a1a429`:

- **`blitter/blitmod.s`.** The colour-depth commit `0223e6f2d8` added NColour
  as a sixth mode constant in the in-image `sv_vduvals` block, moving
  everything after it by 4. `zcode/romloader` holds that block in the
  workspace at `WK_sv_vduvals` with offsets for the old five-constant
  layout, so `WK_sv_vduvals + 20` names a different constant on each side.
  A merge can succeed as text and read the wrong constant at runtime.
- **`blitter/GVFill,ffa`** is a binary: rebuild from the merged source.
- **`hostfs/dde/c/hostfs`.** `zcode/romloader`'s RMA conversion (`f5abc37686`)
  was made against the v0 module. v1 rewrote much of that code and added
  statics the conversion does not cover. Redo it on v1 rather than merging.

## Not pushed by this session

Visible only on the Mac, and not this session's to push:
`zcode/romloader` (3 commits), and in `ROS_PRIVATE` the local commits
`6972411` (`docs/rom-modules.md`) and `0f34b3b`. `rom-modules.md` states that
GVFill booted from ROM and cites the §8 test above as passing — the same
loaded-is-not-running problem.
