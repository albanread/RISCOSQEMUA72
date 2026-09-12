# BOOTDESIGN — booting from HostFS, and what may be shipped

`FSDESIGN-V1.md` makes HostFS a real filing system.  This document is
about what that unlocks: **booting RISC OS from a host directory**, with
no card image in the path, and shipping the result to someone else.

Three things have to be true at once, and they are independent problems:

1. The filing system must exist **before** the boot sequence looks for
   `$.!Boot` — and we are not building a ROM from source (§3).
2. Booting must not need anything we cannot redistribute (§2).
3. It has to be worth doing, which it is, for a reason worth measuring
   rather than asserting (§4).

Sprint order is §3, §5, §4, §2: make it boot, make it persist, measure
it, then work out what may be shipped.  §2 is first in the document
because it constrains what §3 is allowed to produce.

---

## 1. The shape

    host directory                      guest
    --------------                      -----
    riscos-boot/!Boot          <---->   HostFS::HostFS.$.!Boot
    riscos-boot/Apps                    the boot filing system
    riscos-boot/Library
                                        SD image, if attached, stays
                                        an ordinary drive on SDFS

The card stops being the root of the world and becomes optional baggage.
Everything a developer edits — `!Boot`, `Choices`, source trees — is a
plain file on the Mac, editable with Mac tools while the machine runs.

---

## 2. Licensing: what may be bundled

*A summary of public licence terms, not legal advice.  §2.5 is the part
that matters.*

### 2.1 The ROM — redistributable

RISC OS 5 was relicensed to **Apache 2.0** in October 2018, when RISC OS
Developments Ltd acquired Castle Technology and the RISC OS IPR.  Apache
2.0 permits binary redistribution, modified, commercially.  The
obligations are to ship the licence text, preserve NOTICE content, and
state where files were modified.

The core outline fonts — Trinity, Homerton, Corpus — are now copyright
RISC OS Developments and released under Apache 2.0, which removes what
was historically the sharpest problem here.

One caveat, in ROOL's own wording: *"Most of RISC OS itself is released
under the Apache 2.0 License."*  Their licences page deliberately
declines to claim uniformity and points at individual package sources.
"Most", not "all" — so a bundle needs a file-by-file inventory, not a
blanket assumption.

### 2.2 The disc image — depends which

**ROOL's plain Pi SD image**: OS, `!Boot` and applications largely built
from the same Apache sources.  Redistributable in substance, subject to
that inventory.

**RISC OS Direct, and Pi 4 images derived from it**: bundles Impression,
Fireworkz, PipeDream, Zap, StrongEd, Iris, DVD playback and games.  These
are commercial third-party products included by arrangement with RISC OS
Developments for *their* distribution.  **That permission is not
transitive.  Do not redistribute.**

This is the one that would actually catch someone out, because the Direct
image is the pleasant one to develop against.

### 2.3 Broadcom firmware — the clause that bites an emulator

`bootcode.bin`, `start*.elf` and `fixup*.dat` are Broadcom binary-only.
Redistribution is permitted; *use* is restricted:

> "...only be used for the purposes of developing for, running or using a
> Raspberry Pi device, or authorised derivative device manufactured via
> the element14 Raspberry Pi Customization Service."

A QEMU virtual machine is not a Raspberry Pi device.

The mitigation is free, because it is already true: **QEMU's raspi models
do not execute the VideoCore firmware.**  The kernel image is loaded
directly, which is why `run-macos.sh` passes `-kernel RISCOS.IMG` and no
firmware at all.  Those files are dead weight in an emulator image.
Strip them and the clause stops applying to us.

### 2.4 Trademark

Apache 2.0 §6 grants no trademark rights, and "RISC OS" is part of the
IPR RISC OS Developments acquired.  Saying an application runs RISC OS is
ordinary descriptive use; naming a product "RISC OS *something*", or
using the logo, is a separate permission to ask for.

### 2.5 What to actually do

1. **Build the bundled boot tree from the Apache sources** rather than
   redistributing ROOL's image — own ROM, own minimal `!Boot`, own
   modules.  Then the provenance of every file is known and the Broadcom
   question never arises.  This is also exactly what §6 makes easy: a
   boot tree is a host directory, so "build it" means "assemble a
   directory", not "master an image".
2. Fallback: ship the ROM and a minimal boot, and offer Direct as a
   first-run download the user fetches themselves.
3. Ship `LICENSE`, `NOTICE` and `THIRD-PARTY-NOTICES`, and state
   modifications.
4. **Ask ROOL and RISC OS Developments directly.**  Both are small and
   approachable, and a written yes is worth more than any reading of a
   licence page — including this one.

---

## 3. Getting the module in before the filing system is needed

This is the hard part, and the constraint is explicit: **we are not
building the ROM from source.**

### 3.1 Why the obvious answers do not work

The boot filing system has to be initialised during the kernel's module
scan, before anything looks for `$.!Boot`.  That rules out:

- **Soft-loading from `!Boot.Choices.Boot.PreDesk`**, which is how HostFS
  is installed today.  It requires a working filing system to reach the
  module that is supposed to *be* the filing system.  Any scheme that
  loads the module from a filesystem in order to be that filesystem is a
  trap, and it fails late and confusingly rather than cleanly.
- **`*RMLoad` from anywhere**, for the same reason.
- **Building a ROM from the RISC OS sources**, which works and is what
  RPCEmu-style setups do, but is the thing we have ruled out.

### 3.2 The answer: QEMU splices the module into the ROM as it loads it

The stock ROOL `RISCOS.IMG` goes in; a ROM with extra modules on its
chain comes out; nothing on disc, in the build, or in the guest changes.
The kernel then initialises HostFS exactly as it initialises SDFS,
because as far as it can tell that is what it is.

This is not speculative plumbing — **half of it already exists in this
repository.**  `tools/mkcmos.py` walks the ROM module chain today, to
resolve `--unplug EtherGENET` to a chunk number, and its docstring
records the format precisely:

> start at `SysModules_Info+4`, read the size word at module-4, add it to
> get the next module, stop at a zero size word; chunk numbers count from
> 0 at the first module. The start offset is not written anywhere in the
> image, so it is found the same way a wrong-length word would be: try
> every offset, keep the walk that runs cleanly to the terminator with
> sane module titles throughout.

So the chain is a flat run of `[size][module]`, the size word sitting
four bytes *before* each module and covering that module plus the next
size word, terminated by a zero.  Appending is:

```
    find the terminator T (the word at T-4 reads 0)
    write  (body_len + 4)  at T-4          -- the old terminator
    write  body            at T
    write  0               at T + body_len -- the new terminator
```

and the image grows by `body_len + 4`.

**Append, do not insert.**  Initialisation order is chain order, so a
module at the end is initialised after every ROM module — FileSwitch
included — which is exactly what HostFS needs.  Inserting earlier buys
nothing and risks initialising before a dependency.

### 3.3 The size coupling, which is the part that will bite

The OS image header lives at file offset `0x10000`:

```
    +0  'OSIm'   magic (0x6D49534F)
    +4  flags
    +8  image_size
```

and `mkcmos.py` already derives one thing from it:

```python
def load_address(rom_path):
    """0x10000 + OS image size + 0x10000, read from the image's own header."""
```

That is where the CMOS blob goes — `run-macos.sh` passes
`addr=0x510000`, which for the 5.30 ROM is exactly
`0x10000 + 0x4F0000 + 0x10000`.  **The HAL computes the CMOS address from
`image_size`.**  Two consequences:

- There is roughly **64 KiB of headroom** between the end of the image
  and the CMOS blob.  HostFS is about 12 KiB, so several modules fit
  without disturbing anything.
- Growing past that moves the CMOS blob — and because the HAL derives
  the address, **updating `image_size` in the header makes the HAL follow
  automatically**.  The injector must then place the blob at the new
  address.  Since QEMU is doing both, it can.

That is the whole coupling, and it is the thing to get right first: a
spliced ROM whose `image_size` and CMOS address disagree will boot to a
black screen with no useful diagnostic, because a CMOS blob with a bad
checksum is silently blanked (`FSDESIGN.md`'s CMOS notes explain why).

### 3.4 Where it hooks in

`hw/arm/raspi.c` reaches `arm_load_kernel()`, which loads `-kernel` at the
machine's load address.  The injector wants to run between reading the
file and handing it over: read `RISCOS.IMG`, splice, update the header,
and place the result with `rom_add_blob_fixed()` rather than letting
`load_image_targphys()` read the file.

The property should follow `vmchannel-root`'s pattern:

```
    -global bcm2838-peripherals.inject-modules=HostFS,ffa[,Other,ffa...]
```

with `run-macos.sh` growing a `RISCOS_MODULES` variable beside
`RISCOS_HOSTFS`.  And it should be loud: print each spliced module's
title and the new image size at startup, because a silent injector that
did nothing looks identical to a module that failed to initialise.

### 3.5 What the module owes in return

A module on the ROM chain runs **from ROM**.  It may not write to its own
image.  The DDE's `-zM` module build is designed for exactly this — the C
statics live in a workspace claimed from the RMA at initialisation, not
in the image — which is why `cc -zM -zps1` is in `dde/Build,feb`.  Our
module's `reqstore[]` and the `catents[]` array must land in that
workspace and not in the image.

**Verify this before trusting it.**  The test is cheap and unambiguous:
splice the module, boot, and see whether it registers.  A module writing
to ROM aborts immediately and visibly; this is not a failure mode that
hides.

Two further obligations:

- **Position independence.**  Modules are built for it, and ours already
  is, but it is now being run at an address it has never been run at.
- **No dependency on being soft-loaded.**  `hostfs_init` currently maps
  the doorbell lazily (`ensure_vmch`) precisely because OS_Memory 13
  fails during module init.  That workaround becomes load-bearing rather
  than convenient, and should be commented as such.

### 3.6 The escape hatch

If splicing inside QEMU proves awkward, the same code run as a host-side
tool — `tools/mkrom.py stock.img HostFS,ffa -o spliced.img` — produces
the same artefact with a different trigger, and `run-macos.sh` boots the
output.  It is a worse developer experience and an identical result.
Write the splicer as a library either way, so the choice stays cheap.

---

## 4. What the boot sequence will demand

HostFS sits directly on FileSwitch, not FileCore, so none of FileCore's
work is done for us.  Booting exercises far more of the FS interface than
copying files ever did.

### 4.1 The list

- The FSEntry set — Open, GetBytes, PutBytes, Args, File, Func.  Sprints
  1–3 cover the first five for the paths exercised so far.
- **Directory enumeration** via `OS_GBPB` with full catalogue
  information.  `FSDESIGN-V1.md` §8 designs it; it is sprint 5 and it is
  now on the critical path, because the boot sequence and the Filer
  hammer it.
- **Wildcard matching** — `FSEntry_Func 24` returning `r4 = -1` hands it
  back to FileSwitch, which is the cheap and correct answer.
- **A plausible `*Free`** — `FSEntry_Func 30`.  Something in `!Boot` will
  ask.
- **Writability from early on.**  `!Boot` is not a read-only consumer: it
  needs `Wimp$ScrapDir`, writes into `!Boot.Choices`, and several PreDesk
  stages fail noisily on a read-only mount.  Any `readonly=` mode belongs
  after boot, not during it.

### 4.2 Case, which sprint 3 already fixed

RISC OS is case-insensitive and case-preserving, and different parts of
`!Boot` refer to the same objects with different casing.  On a
case-sensitive host that would bite immediately.

Sprint 3 resolves names case-insensitively **against a directory scan of
our own**, not against host rules — exact match first, then a
forward-mapped scan.  That is the right behaviour and it is already in.
The remaining work is that the scan is currently O(n) per miss, which
§4.4 cares about.

### 4.3 Index coherence — the genuinely new problem

An SD image is isolated, and isolation is what made cache invalidation a
non-issue.  Booting from a host directory ends that: files can be edited
on the Mac while RISC OS is running, and something has to give.

Three policies, in increasing order of cost and correctness:

- **Re-stat on open.**  Simple, correct for file contents, and a
  per-operation tax on the hot path.
- **A directory watch** — FSEvents, inotify, `ReadDirectoryChangesW` —
  pushing invalidation instead of polling for it.  Keeps the fast path
  clean; three implementations and their quirks.
- **An explicit `*HostFSRefresh`.**  Trivial, and wrong often enough to
  be a support burden.

Note that this is the same watcher question `FSDESIGN-V1.md` §6.3 settled
*against* for metadata — and the answer there does not carry over.  There,
the host filesystem was the authority and nothing needed to know a file
existed before being asked.  Here we would be *caching* a directory
listing for speed, and a cache does need invalidating.  The distinction
is worth keeping straight: metadata needs no watcher; a listing cache
might.

Recommended: start with no cache at all and measure (§5).  A stale
listing after a host-side edit is the failure mode that costs an
afternoon before it is recognised, and it is better not to introduce it
until the numbers say a cache is needed.

### 4.4 What is lost compared with an image

Disc-level semantics, nested image filing systems, and anything writing
the medium directly.  Nothing in a normal boot does any of these, which
is why this is viable at all.

### 4.5 First test, before writing anything bespoke

Unpack the tree from the working SD image into plain host files through
the mapping that already exists, point `RISCOS_HOSTFS` at it, and boot
that.  Reaching the desktop proves the filing system.  Everything after
that is curation, and curation is a much nicer problem than debugging a
boot.

---

## 5. Performance: why this should win, and how to keep the win

HostFS should beat the emulated card substantially, and the reason is
simply how many times each path crosses the expensive boundary.

### 5.1 The SD path

- Every MMIO register access traps into the device model.  A 512-byte
  sector moved through a FIFO a word at a time is ~128 traps.
- Command and response protocol per block, on top.
- Polling loops on status bits — pure emulator overhead, burning traps to
  observe a bit that could have been set instantly.
- FileCore above all of it: disc record, allocation map, on-disc
  directory objects, sector-granular reads.  A 40-byte read of a boot
  option becomes map lookups and a full sector transfer.

### 5.2 The HostFS path

- One crossing per filing-system operation.
- A `pread` served from the host page cache.
- One `memcpy` into guest memory.
- No FileCore; the host filesystem does the block work, with better
  readahead than anything we would write.
- No sector amplification: 40 bytes read is 40 bytes read.

### 5.3 Expectations

One to two orders of magnitude on metadata-heavy work, which is exactly
what booting is.  The gap narrows on large sequential reads, where an SD
model with a DMA fast path into guest memory approaches memcpy speed.
The small-operation storm is where the emulated protocol is hopeless, and
booting is nothing but a small-operation storm.

### 5.4 Four ways to throw the advantage away

All implementation, none architecture — and the first is already a live
risk in our code:

- **A host `open`/`stat`/`close` per RISC OS operation.**  `OS_File 5`
  existence checks and wildcard scans during boot run into the thousands.
  Hold host handles for the life of the RISC OS handle.
  *Live risk*: sprint 3's `resolve_leaf()` falls back to a full directory
  scan whenever the fast-path stat misses — and a `,xxx`-suffixed name
  misses **every time**.  A boot tree full of typed files would scan the
  directory once per lookup.  This wants an index before §3 lands, not
  after.
- **`fsync` per write.**  The emulated card writes lazily into a host
  file and will beat us comfortably if we make every `PutBytes` durable.
  Let the host cache work; flush on `FSEntry_Args 6`, which is what it is
  for.
- **Stat-on-every-open for coherence** (§4.3).  A per-operation tax to
  solve a problem that may not need solving.
- **A doorbell per catalogue entry.**  Sprint 3 avoided this by putting
  load/exec into the CAT entry; enumeration must stay one round trip per
  *listing*, not per file.

### 5.5 Measurement

**Measured 13 Sep; the numbers are in `FSDESIGN-V1.md` §13 B2.** In short:
HostFS boots in 14.4 s against the card's 15.7 s, with 2,657 doorbells
against 2.4 million SDHCI register accesses — and 0.1 s of host time. §5.3's
"one to two orders of magnitude" holds for crossings, not for wall-clock,
because under TCG the emulated CPU, not the crossings, is what a boot
waits on. The rest of this section is the plan as written.

Instrument both: count MMIO accesses for an SD boot against
filing-system calls for a HostFS boot.  That ratio carries most of the
argument, and it stays honest later when someone optimises the SD model
and wants to know whether HostFS is still worth it.

Both instruments already exist in outline — `VMCH_TRACE` counts one side,
and QEMU's tracing counts the other.  The boot-time instrument from
`MACOS.md` §8 (sampling `screendump` until the image settles) gives the
wall-clock number to put beside the ratio.

---

## 6. Mounting SD images alongside

Booting from HostFS does not retire the card images, and should not: they
are the reference for what a real system looks like, and
`quemu_pi4_dde-2026-09-11.img` is where the DDE lives.

Nothing needs building for the common case.  `-drive if=sd` is
independent of the boot filing system, so a machine booted from HostFS
can still have a card attached and reachable as `SDFS::0`.  Copying
between `HostFS:` and `SDFS:` already works in both directions — that is
how every module built this week reached the host.

What is worth designing:

- **Boot source as a launch option**, not a rebuild.  `RISCOS_BOOT=hostfs`
  or `=sd` in `run-macos.sh`, driving the injected module set and the
  CMOS `*Configure FileSystem` seed together, since those two must agree.
- **Attaching a card without booting it** — already the case; document
  it, because the useful workflow is "boot fast from HostFS, mount the
  DDE card, build".
- **Reading an image from the host side** is the one genuinely new
  capability, and it is a FileCore reader in C on the host.  That is
  real work for a narrow benefit: unpacking an image can be done once,
  with the guest, into a host directory that then boots.  §4.5's first
  test is exactly that operation.  **Not recommended** until something
  needs it.

---

## 7. CMOS has to persist

**Built 13 Sep, as neither option below:** HostFS keeps CMOS in the share
the way SDCMOS keeps it on a card, and the launcher boots from that file
(`FSDESIGN-V1.md` §13 B2). No device and no HAL change was needed.

`*Configure FileSystem HostFS` and `*Configure Boot` are how the boot
source is chosen, and they live in CMOS.  Today the CMOS blob is static:
`-device loader` places `cmos.bin` and nothing writes it back, so every
launch starts from the same settings and any `*Configure` is forgotten at
power-off.

For an SD boot that was merely untidy.  For a HostFS boot it is
load-bearing, because the boot filing system is itself a CMOS setting.

Two options:

- **Pre-seed it.**  `mkcmos.py` already synthesises a CMOS blob from the
  ROM's own tables, including `--language`; teaching it
  `--filesystem HostFS --boot` is a small change, and the result is a
  machine that always boots the same way by construction.  Good for a
  shipped bundle, useless for a user who wants to change anything.
- **Back it with a host file.**  A writable NVRAM region whose contents
  are flushed to a file on change.  This is the honest answer and it is
  more work: RISC OS reaches CMOS through the HAL, so it needs a device
  or a HAL-visible region rather than a `-device loader` blob.

Recommended: **both, in that order.**  Pre-seeding unblocks §3 and is
enough to prove the boot; persistence is what makes it usable, and can
follow once there is something worth persisting.

---

## 8. Sprints

The sprints for booting off HostFS now live in one plan with the filing
system work, **`FSDESIGN-V1.md` §13**, because booting depends on HostFS
first running from ROM (R1), being present at startup (R2), and looking
up typed names fast (B1). This document keeps the design.

**The acceptance test this section used to give for splicing was too
weak.** It was "`*Modules` lists HostFS with a ROM address" — which proves
the module loaded onto the chain, not that it runs, and a module that
splices and then aborts passes it. It was cited as passing for work that
did not run. The test is now a working result from a spliced ROM with
nothing soft-loaded (`FSDESIGN-V1.md` §13).

§3.3's analysis of `image_size` and the CMOS address, and its 64 KiB
headroom, were inference from `mkcmos.py` and were never tested. The
loader that exists (`tools/mkrom.py`) does not change the header, and
`docs/rom-modules.md` in the private repository gives that as verified
against the 5.31 sources; treat §3.3 as superseded, pending a boot that
exercises it.

---

## 9. Open questions

- **Is there a ROM checksum?**  If the kernel or HAL validates the image
  beyond the `OSIm` magic, the splicer must fix it.  Nothing in
  `mkcmos.py` suggests one, but nothing in `mkcmos.py` grows the image
  either.  Check `Kernel/s/` in the sources before writing the splicer.
- **Do the module's statics really land in RMA?** (§3.5.)  Cheap to test,
  fatal if assumed wrongly.
- **How much headroom is there really?**  64 KiB is inferred from the
  5.30 image's `image_size` against the CMOS address, not measured across
  ROM versions.
- **Does `*Configure FileSystem` accept a soft FS number?**  HostFS uses
  220, which `Hdr/Global/FSNumbers` does not allocate (§`FSDESIGN-V1.md`
  15).  A boot filing system is a more public thing than a share, and
  this is the point at which asking ROOL for a number stops being
  optional.
