# FSDESIGN-V1 — HostFS as a real filing system

`FSDESIGN.md` designed the doorbell and a v0 protocol, and that protocol
was built: the device implements every command in it, including OPEN,
READ, WRITE and SEEK.  What was never finished is the *guest* half.  The
module turns FileSwitch's world into POSIX calls by hand, and the hand
translation is where it fails.  This document is the v1 design: move
that translation across the doorbell into the host, and leave the module
with nothing to get wrong.

The governing principle, and the reason for every choice below:

> **Do as much work in the host as possible, and as little in the guest
> as possible.**

The host is C on a fast machine, with a debugger, a test harness and an
edit-compile cycle measured in seconds.  The guest is Norcroft C cross-
built through a DDE round trip, running under TCG, debugged by taking
screenshots of a task window.  Every line of logic moved from the second
to the first is bought at a large discount.

**Sources.**  Everything asserted here about RISC OS is checked against
primary material, all of it in the private repository:

- **`devdocs/PRM2.PDF`**, *Writing a filing system*, pages 2-534 onwards.
  This is the specification; where it and anything else disagree, it
  wins.  Page references below are the printed ones.
- **`src/src-bcm2835-dev.5.31.tar.bz2`** — ROOL's RISC OS 5.31 sources.
  `BCM2835/RiscOS/Sources/FileSys/FileSwitch` is the implementation, and
  citations of the form `s/OSGBPB` or `hdr/LowFSI` are paths within it.
  `Doc/SimpleFS` there is Acorn's own recipe for the kind of filing
  system HostFS is.
- **`Hdr/Global/{FileTypes,FSNumbers,NewErrors}`** from the DDE, for
  filetype, filing system and error number allocations.
- **`db/riscos_prm.sqlite`** indexes the PRM's SWIs; the `FSEntry_*`
  points are prose in PRM2, not SWIs, so they are not in it.

---

## 1. Where it stands, and exactly why

Measured 12 Sep 2026 against `quemu_pi4_dde-2026-09-11.img`, the module
as shipped, tracing every doorbell request through `VMCH_TRACE`.

| Operation | Result |
| --- | --- |
| `*Cat HostFS:` | works |
| `*Ex HostFS:` | runs, but **every file reports load/exec `FFFFFFFF FFFFFFFF`** |
| `*Copy HostFS:c.hello c.hello` (whole file in) | works, byte-exact |
| `*Copy hello HostFS:hellobin` (whole file out) | works, byte-exact |
| `OPENIN` + `EXT#` | works — correct extent (29) |
| `*Type HostFS:TESTTXT` | **prints nothing** |
| `BGET#` | **returns 254 for a file starting `l` (108), then "handle is either illegal or has been closed"** |

### The stream bug, found in the source

The module's open handler returns a file information word with bit 28
set:

```c
r->r[0] = (int)((1u << 31) | (1u << 30) | (1u << 28));
r->r[2] = 0;      /* unbuffered: every get/put carries the address */
```

and its filing system information block ends:

```c
InfoBlk[10] = 0;                     /* gbpb: FileSwitch synthesises */
```

Those two contradict each other, and FileSwitch believes the first.
`hdr/LowFSI` defines bit 28 as `fsopen_UnbufferedGBPB`; `s/OSFind` turns
it into `scb_unbuffgbpb`, which `s/StreamBits` documents as "1 if stream
directly supports gbpb, 0 if not"; and `s/OSGBPB` acts on it:

```
UnbufferedMultiple Entry "r5-r7"
        TST     status, #scb_unbuffgbpb ; Can FS do this quickly ?
        BEQ     %FT01                   ; Nope !
        BL      CallFSMultiple          ; Gets/updates block itself
```

So every multi-byte transfer is dispatched to the FS's **`FS_gbpb`
entry — info block word 10 — which the module sets to zero.**  The
comment beside that zero says what the author meant ("FileSwitch
synthesises"), and setting bit 28 asked for the opposite.  Garbage data
and a dead handle are the exact expected result.

*An earlier draft of this document guessed the cause was an inverted
"bytes not transferred" convention on FSEntry_GetBytes.  That was wrong,
and the source settles it: the fault is the bit-28/word-10 contradiction.*

There is a second inconsistency in the same handler, and the PRM makes
it plain.  The two GetBytes entry points are *different calls*:

| | buffered (PRM 2-544) | unbuffered (PRM 2-545) |
| --- | --- | --- |
| entry | R1 handle, R2 buffer, R3 count, R4 file offset | R1 handle |
| exit | — (nothing) | R0 = **the one byte**, C set at EOF |

The unbuffered entry transfers **a single byte**.  `fsentry_get_handler`
reads R2/R3/R4 and moves a block — the *buffered* signature — while the
open word declares the stream unbuffered.  So the module declares one
interface, implements a second, and leaves the entry for the third
(`FS_gbpb`) null.  Configured as a buffered filing system, the handler
it already has is the right shape and needs no return value at all.

The remaining defect is independent.  `xfer()` translates
addresses itself, via `mem_phys()` (OS_Memory 0) per chunk, splitting
every transfer at 4 KiB boundaries and issuing a SEEK before every
READ — and **on a failed translation it falls back to the identity
mapping**, DMAing to whatever physical address the logical one happens
to resemble.  Silent corruption is the designed behaviour of that
fallback.

Two smaller gaps, both squarely in the ask:

- **No type mapping.**  `riscos_type_for()` reads a `,xxx` suffix and
  otherwise returns `&FFF`.  Nothing maps `.txt`, `.png`, `.zip`.  And
  the suffix is not consumed: `suffixed,fff` is *displayed* as
  `suffixed,fff` rather than presented as `suffixed` of type Text.
- **Host names with dots are visible but unreachable.**  `readme.txt`
  catalogues fine; every attempt to open it fails, because RISC OS
  reads the dot as a path separator.  A file you can see and cannot
  open is worse than one that is hidden.

---

## 2. Four precedents, three in QEMU and one in RISC OS

**`Doc/SimpleFS` — Acorn's own recipe, and the most important document
here.**  It is a note on how to write "a network flavour filing system"
— remote, no local disc structure, which is precisely HostFS's shape.
It gives the filing system information word bit by bit, the open
information word, and the exact list of `FSEntry_File`, `_Args` and
`_Func` reason codes such an FS must implement.  §4 and §5 below are
that recipe applied.  Its closing line is the argument for this whole
document: *"I believe writing such a filing system is less work than the
RISC OS 2.00 way."*

**`semihosting/` in QEMU — the para-virtual idiom, and our licence.**
ARM semihosting's `SYS_READ` and `SYS_WRITE` do exactly our job: host
file I/O on behalf of guest code, with the guest passing *its own
virtual pointers*.  `semihosting/uaccess.c` is the whole mechanism:

```c
void *uaccess_lock_user(CPUArchState *env, vaddr addr, size_t len, bool copy)
{
    void *p = malloc(len);
    if (p && copy) {
        if (cpu_memory_rw_debug(env_cpu(env), addr, p, len, 0)) { ... }
    }
    return p;
}
```

`cpu_memory_rw_debug()` takes a guest *virtual* address, walks the MMU
with the CPU's current settings, and performs the access — crossing
pages internally.  Not a debug-only hack borrowed out of scope: it is
the path a shipped QEMU feature uses for precisely this purpose.

**`hw/9pfs/` — the passthrough-filesystem idiom.**  VirtFS has already
solved "expose a host directory to a guest with a foreign metadata
model".  Its `mapped-file` model keeps a sidecar directory,
`.virtfs_metadata/`, holding one `key=value` file per real file, and
filters it out of enumeration so the guest never sees it.  §6.3 adopts
that.

**`hw/misc/bcm2835_property.c` — the hardware idiom.**  A per-device
`AddressSpace` (`s->dma_as`) and `dma_memory_read/write` on *bus*
addresses, because real silicon cannot walk guest page tables.
`vmchannel` currently uses the global `address_space_memory`, skipping
the SoC's bus translation; if any physical-addressed path survives v1,
it should take a `dma_as` link like its neighbours.

---

## 3. The central move: the host walks the map

**All guest addresses on the v1 wire are logical (virtual), exactly as
FileSwitch passed them to the module.  The host translates.**

The module stops calling OS_Memory, stops splitting at page boundaries,
stops looping, and stops having an identity-mapping fallback that can
corrupt memory.  It passes R2 through untouched.  The host does:

```c
/* GETBYTES: read len bytes at file offset off into guest virtual addr */
g_autofree uint8_t *bounce = g_malloc(len);
ssize_t got = pread(fd, bounce, len, off);
if (got < 0) { rc = errno_to_riscos(errno); break; }
if (cpu_memory_rw_debug(current_cpu, addr, bounce, got, true)) {
    rc = VMCH_RC_BADADDR;               /* not mapped: say so, never guess */
    break;
}
```

Three consequences, all of them the point:

- **One doorbell per FSEntry call**, whatever the size or alignment.
  The 4 KiB chunking and the SEEK-before-every-READ both disappear —
  the offset rides in the request, `pread`/`pwrite` style.
- **The guest cannot mistranslate an address**, because it no longer
  translates one.
- **A failed translation is an error, not a guess.**

Honest limits, recorded so nobody rediscovers them:

- `cpu_memory_rw_debug` **will not fault a page in**, and measurement
  says that matters.  Sprint 1 built this and tried it:

  | Buffer | Result |
  | --- | --- |
  | `0x493aa000`, 12320 bytes (RMA / dynamic area) — module copied out | **whole transfer, one doorbell** |
  | FileSwitch's own buffer, `*Type` | works |
  | `0x00008f04`, a BASIC `DIM` in application space | **252 bytes translate, then the page at `0x9000` fails** |

  252 bytes is exactly the distance to the next page boundary, so the
  first page of that buffer translates and the second does not.  The
  earlier text here assumed "application space or the RMA, so this is
  fine in practice".  The RMA half is right; **application space is
  not**.

  What the evidence does and does not support, because the difference
  matters to whoever picks this up:

  - **Page crossing is not the problem.**  The 12320-byte transfer at
    `0x493aa000` spans four pages and moves whole.  `cpu_memory_rw_debug`
    crosses pages correctly.
  - **It is not lazy mapping.**  Touching every page of the buffer from
    BASIC first (`FOR I%=0 TO 262143 STEP 4096:B%?I%=0:NEXT`) changes
    nothing: the next attempt fails at the same address, after the same
    252 bytes.  The guest can read and write `0x9000` perfectly well at
    the moment QEMU says it cannot translate it.
  - **Translation is context-dependent, and the monitor is not a witness
    to it.**  `gva2gpa` over QMP reports *Unmapped* for every one of
    these addresses — including `0x493aa000`, which the same run had just
    transferred successfully.  The monitor samples whatever task happens
    to be current, so it cannot be used to check an address that was live
    during somebody else's doorbell.  Any future diagnosis has to be made
    from inside the handler, not from the monitor.

  So the open question is narrow: why, at the instant the module rings
  the doorbell on BASIC's behalf, does the ARM debug walk resolve
  `0x8f04` and refuse `0x9000`.  The candidates are the mmu_idx the debug
  path picks for the current mode, a permission check applied to the
  walk, and RISC OS mapping application space in a form the walk handles
  differently from a dynamic area.  Instrumenting `guest_rw_counted` to
  log the failing address and the CPU's mode and TTBR at the point of
  failure would settle it in one run.

  **A fix that does not need that answer**: stage through memory that is
  known to translate.  The module owns RMA, and RMA translates — so a
  transfer whose guest buffer comes up short can be retried into an RMA
  staging block and `memcpy`d to the caller in guest code, where the
  guest's own MMU does the work and the question does not arise.  That
  costs one copy and keeps the doorbell count proportional to the staging
  size rather than to the page count: a 64 KiB stage makes a 256 KiB read
  four doorbells instead of 130.  Worth doing on the failure path only,
  so the fast path stays at one.

  Two consequences, neither optional:

  1. The host transfers **page at a time and reports how far it got**
     (`guest_rw_counted`), so a partly-mapped buffer produces a short
     count rather than plausible-looking rubbish past the boundary.
  2. The **module must return an error** when the count is short.
     Buffered GetBytes has no exit registers, so the only way to say
     "this failed" is a `_kernel_oserror` — and the first cut of this
     sprint dropped the old `r->r[3] = moved` without putting that in
     its place, so a failed transfer was reported to FileSwitch as
     success. `OS_GBPB` then cheerfully answered "0 bytes not
     transferred" over a buffer holding 252 good bytes and 261892 stale
     ones. Silence is the worst possible failure here.
- `current_cpu` is the right CPU: the handler runs synchronously inside
  the vCPU's MMIO write, under the BQL, as `FSDESIGN.md` requires.
  Assert it is non-NULL rather than assuming.
- Under TCG there is no cache model, so writing guest RAM behind the
  CPU's back is coherent.  This board is TCG-only (`aarch64=off`).  If
  an HVF or KVM path ever appears, this needs revisiting.
- The bounce buffer costs one copy.  `probe_access_flags()` can hand
  back a host pointer for RAM-backed pages, allowing `pread` straight
  into guest memory; an optimisation to measure, not a starting point.

---

## 4. The wire speaks FileSwitch, not POSIX

The deeper reason the module has bugs is that the v0 protocol is
POSIX-shaped.  Something has to convert between FileSwitch's model —
reason codes, register frames, the C flag, load/exec words — and
open/seek/read/close.  In v0 that something is the module.  In v1 it is
the host.

**v1 commands carry a RISC OS register frame verbatim.**  The module
copies R0–R7 into the block, rings the doorbell, copies R0–R7 back, and
returns.  It does not interpret them.

Registers stay as `FSDESIGN.md` defined them; `VERSION` becomes 1 and
`FEATURES` gains:

    0x08  FSENTRY   v1 register-frame commands
    0x10  VIRTADDR  guest addresses on the wire are logical
    0x20  HOSTNAME  host-side name and type mapping (§6)
    0x40  META      sidecar metadata store present (§6.3)

The v1 request block, 64-byte header as before:

    +0   u32  cmd
    +4   u32  seq
    +8   u32  rc          transport result (VMCH_RC_*)
    +12  u32  err_num     RISC OS error number, 0 = none
    +16  u32  arglen      inline bytes after the header
                          (on return: length of the error text, if any)
    +20  u32  r[8]        R0..R7 in, R0..R7 out          (+20 .. +51)
    +52  u32  psr_out     bit 29 = C flag to return
    +56  u32  reserved[2]
    +64  ..   arglen bytes

Commands:

    0x100  FSENTRY_OPEN        0x104  FSENTRY_CLOSE
    0x101  FSENTRY_GETBYTES    0x105  FSENTRY_FILE
    0x102  FSENTRY_PUTBYTES    0x106  FSENTRY_FUNC
    0x103  FSENTRY_ARGS        0x120  STARCMD

v0's commands 0–17 stay implemented, unchanged, forever (§11).

**Pathnames are not copied by the guest either.**  FSEntry calls pass a
pointer in R1 to a string terminated by any control character; the host
reads it through the MMU (`uaccess_strlen_user()` is the model, adjusted
for the control-character terminator).  And `Doc/SimpleFS` makes that
cheap: with information word bit 22 clear, **FileSwitch fully
canonicalises every path before the FS sees it** — special field, disc
name, and the path from `$.` onwards — so the host receives clean
absolute names and never has to parse `^`, `@`, `%` or `&`.

### The filing system information word

Straight from `Doc/SimpleFS`, adjusted where HostFS differs.  Bits are
defined in `hdr/LowFSI`.

| Bit | Set | Why |
| --- | --- | --- |
| 31 `special` | 0 at first | Special fields (`HostFS::share.$`) — §15 |
| 29 `nullnameok` | 0 | |
| 28 `alwaysopen` | 0 | |
| 27 `flushnotify` | 1 | We want to know when to `fsync` |
| 26 `fsfilereadinfonolen` | 0 | |
| 23 `multifsextensions` | 1 | Canonicalise/ResolveWildcard |
| 22 `handlesurdetc` | 0 | **FileSwitch canonicalises paths for us** |
| 21 `nodirectories` | 0 | FileSwitch stores `*Dir`/`*Lib` for us |
| 20 `dontuseload` | **1** | `*Load` becomes Open/GBPB/Close |
| 19 `dontusesave` | **1** | `*Save` becomes Open/GBPB/Close |
| 18 `giveaccessstring` | 0 | FileSwitch parses `*Access` strings |
| 16 `readonly` | from `readonly=` | **FileSwitch enforces it** (§10) |
| 15–8 `nfiles` | 16 | Open-file limit |
| 7–0 `number` | see §15 | The module currently uses 220 |

Bits 20 and 19 are the interesting ones: they retire `fsfile_Load` and
`fsfile_Save` entirely, so whole-file and streaming stop being two code
paths and become one.  Today whole-file works and streaming does not;
after v1 there is only streaming, and the `*Copy` that works now keeps
working because FileSwitch builds it out of Open/GetBytes/Close.

---

## 5. Streams, buffering, and the end of an open question

The PRM and `Doc/SimpleFS` agree, and settle what the previous draft
left open.  Acorn's recommendation for a filing system of this shape is
**buffered**: bit 28 clear, and a real buffer size in R2.

The constraint is exact (PRM 2-541, FSEntry_Open on exit):

> R2 = buffer size for FileSwitch to use (0 if file unbuffered, else
> **must be a power of 2 between 64 and 1024**)

So the ceiling is **1024 bytes, not a page** — `s/OSFind` enforces it,
rejecting the open and deallocating the stream if the value is not a
power of two in range.  R4 (allocated space) must be a multiple of it.

That cap sounds worse for a doorbell-based filing system than it is,
because FileSwitch does not always chop transfers into buffers
(PRM 2-544):

> A client has called OS_GBPB to read a whole number of the buffer size
> at a file offset that is a multiple of the buffer size.  **FileSwitch
> requests that the filing system transfer this data directly to the
> client's memory.**

So the cost depends on how the guest reads, and both cases are
acceptable:

| Guest does | Doorbells for 256 KiB |
| --- | --- |
| `OS_GBPB` of the whole file, buffer-aligned | **1** |
| `BGET` in a loop (FileSwitch refills a buffer) | 256, one per 1024 bytes |
| today, unbuffered and broken | 3 per byte, and wrong answers |

Take 1024 as the buffer size: it is the maximum, and the doorbell — not
the copy — is what costs.

Three consequences worth writing down before they are rediscovered:

- **Buffered GetBytes returns nothing.**  The PRM's "On exit" is a dash.
  There is no count to get backwards; either the bytes are there or you
  return an error.  The whole question the previous draft agonised over
  does not exist in the buffered interface.
- **You will be asked to read past the end of the file.**  The offset is
  guaranteed within the extent and the count is a multiple of the buffer
  size, so the final block routinely runs past EOF; `Doc/SimpleFS` is
  explicit — *"simply read to the file's end and don't return an error"*.
  Today's `xfer()` treats a short read as a reason to stop and the caller
  sees failure.
- **A buffered FS is asked to do less.**  `hdr/LowFSI` marks
  `fsargs_ReadPTR`, `SetPTR`, `ReadEXT`, `ReadSize`, `EOFCheck` and
  `Flush` "Only unbuffered fs": FileSwitch tracks the sequential pointer
  and the extent itself.  The per-handle `hptr`/`hext` arrays in the
  module exist to answer calls a buffered FS never receives.

One more detail from PRM 2-542, for open reason code 1: a newly created
file should be given filetype **&FFD (Data)**, a datestamp, and access
`WR/`.  The host does that, not the guest.

With §3 and §4 in place the stream operations are thin host functions:

| FSEntry | Host action |
| --- | --- |
| Open | `open()`, allocate a handle, `fstat`, return info word, handle, buffer size, extent, allocated |
| GetBytes | `pread(fd, bounce, R3, R4)`, then write to guest virtual R2; short at EOF is fine |
| PutBytes | read guest virtual R2, `pwrite(fd, bounce, R3, R4)` |
| Args 3 / 7 | `ftruncate` — write extent, ensure size |
| Args 8 | write zeroes |
| Args 9 | read load/exec (§7) |
| Args 6 / 10 | flush (`fsync`); ImageStampIs returns no error |
| Close | write back load/exec if given, `close()` |

The offset arrives in R4, so the handle carries no host file position
and SEEK leaves the hot path entirely.

---

## 6. Names and types

The explicit ask: `.xxx` should become `&xxx`.  Three mechanisms, all
host-side, in priority order.

### 6.1 The dot is a slash

RISC OS already has a convention for foreign names containing dots, and
has had since DOSFS: **the dot becomes a `/`**.  `readme.txt` on the
host is `readme/txt` in the guest, and back again on the way out.

This is the whole fix for "visible but unreachable", it is lossless and
bidirectional, and it is the spelling RISC OS users already expect from
DOS discs and CD-ROMs.  No new convention is invented.

### 6.2 Type inference, on the way in

Host filename to (guest leafname, filetype), first match wins:

1. Trailing `,xxx` with three hex digits — the RISC OS typed-file
   convention, and the one ROOL's own DDE archive uses throughout
   (`ReadMe,fff`, `Hello,ffb`).  Type is `xxx`; **the suffix is removed
   from the presented name**.  `notes,ffb` is `notes`, type &FFB.
   (Today it is shown verbatim, which is the bug.)
2. A sidecar metadata entry (§6.3) — authoritative if present.
3. A known extension: type from the table, dot to `/`.
   `readme.txt` is `readme/txt`, type &FFF.
4. An unknown extension: dot to `/`, type = the configured default.
5. No extension: name unchanged, type &FFF.
6. Directories: object type 2, dots to slashes as above.

The table is generated from **`Hdr/Global/FileTypes`**, which is ROOL's
autogenerated allocation list and carries the name of every type
(`FileType_Text EQU &00000FFF`, `FileType_Text_Name SETS "Text"`).
Generate it, do not transcribe it.  Values spot-checked against that
header:

    Text FFF   Command FFE   Data FFD   Utility FFC   BASIC FFB
    Module FFA   Sprite FF9   Font FF6   Obey FEB   Squash FCA
    AIFF FC2   WaveForm FB1   HTML FAF   XML F80   JPEG C85

Ship the generated table compiled in, overridable by a plain text file
named by a device property (`typemap=`), so extensions are data.

**Inference must never produce an executable type.**  Several RISC OS
types mean "run me" when double-clicked — &FFA Module, &FF8 Absolute,
&FFC Utility, &FEB Obey, &FFB BASIC, &FFE Command.  A guess at an
extension is not grounds for telling the desktop that an arbitrary host
file is code.  Only rule 1 (an explicit `,xxx` suffix) or rule 2 (a
sidecar entry) may yield one of those types: both are deliberate acts by
something that already knew.  A table entry that maps an extension to an
executable type is a bug, and the generator should refuse to emit one.

`.mod` is the case that makes the point.  To RISC OS a "module" is a
relocatable module, &FFA.  To everyone else `.mod` is an Amiga
ProTracker song — and also a Fortran module, a Linux kernel object, a
3D mesh.  Map it to &FFA and double-clicking a tracker tune hands
arbitrary bytes to `RMLoad`, which is a crash at best.  Nor is there a
safe alternative to map it *to*: ROOL's list has no "Tracker" type at
all, and `Amiga` (&FE2), `Music` (&AF1), `MIDI` (&FD4) and `GenSound`
(&F96) are all something else.

So `.mod` is simply not in the table; it falls to rule 4 and arrives as
Data.  Nothing is lost, because a real RISC OS module never needed the
rule: modules are `!RunImage`, or `Modules.Foo`, or carry `,ffa` — and
rules 1 and 5 already handle all three.  The same reasoning keeps `.bin`,
`.abs`, `.com` and `.exe` out of the table.

### 6.3 Type persistence: a table, not a database

Inference is a guess, and a file *given* a type by RISC OS must keep it.
The question is where that fact lives.  Three answers were considered.

**A sidecar per file** (9pfs's `mapped-file` model, `.riscos-meta/name`
holding `type=fff`).  Portable and self-describing, and it travels when
a subtree is copied.  But it clutters every directory, it costs a second
filesystem operation per file — which lands squarely on directory
enumeration, the hot path of §8 — and it orphans the moment anyone
renames a file in the Finder.

**A host-side database keyed by filename** — the natural next thought,
and a small SQLite would do it.  One indexed query per directory instead
of N opens, transactional, inspectable with tooling the team already
uses.  Two costs, though, and the second is the one that decides it:

- Linking libsqlite3 into `qemu-system-aarch64` is a real dependency on
  three platforms, and this tree has already fought Homebrew over
  pcre2/glib/libslirp on Intel macOS.  A hash table over a plain text
  index does this job — tens to hundreds of rows, looked up by name —
  without any of that.
- **It would need a filesystem watcher.**  If the database is the truth
  about a file's type, then a file dropped into the share has no truth
  until something notices it, and "something notices it" is FSEvents on
  macOS, inotify on Linux and `ReadDirectoryChangesW` on Windows —
  three implementations, each with its own coalescing and limits, inside
  a QEMU device.

**So: the lookup table, and only the lookup table.**  The watcher is not
needed because the premise that requires it is wrong.  The host
filesystem is the truth; the extension table turns a name into a type;
and nothing needs to know a file exists before it is asked about it:

- A file dropped in has no entry anywhere.  Its type is derived from its
  extension when the guest first looks, which is the right answer with
  no work and no event.
- A file whose type RISC OS *sets* is the only case a table cannot
  derive — and `naming=` already handles it by encoding the type in the
  name (`,xxx`, or the canonical extension).  The name **is** the store,
  it travels with the file, it survives Finder moves, archives and
  other filesystems, and no index can go stale against it.

That leaves exactly one gap: a type with no canonical extension, set on
a file whose name must not change.  That is rare enough to take the
honest answer — refuse it and say so — rather than build an index, a
watcher and an invalidation story to cover it.  If it turns out to bite
in practice, a per-share override file (plain `name<TAB>type` lines,
loaded at start and rewritten on change, no watcher because we are the
only writer) is a day's work and slots in behind the same lookup.

### 6.3.1 The table is data, and deserves a UI

`typemap=` names a plain text file: extension, type code, description.

    txt   FFF   Text
    c     FFF   C source
    png   B60   PNG image
    zip   A91   Archive

Generate the default from **`Hdr/Global/FileTypes`**, which already
carries both halves — `FileType_PNG EQU &00000B60` and
`FileType_PNG_Name SETS "PNG"` — so the table is derived from ROOL's
allocation list rather than typed out, and regenerating it picks up new
allocations.

A host-side editor for that file is worth having and costs nothing in
the device: the table is read at startup and on demand, so anything that
can write the file can change the mapping.  It is also the natural first
customer for the Apple Events surface in `SCRIPTING.md` — a typed
command to read and set rows, with the UI on top.

One thing the UI should *not* do is carry descriptions into the guest.
RISC OS already knows that &FFF is "Text": `*Ex` and the Filer get type
names from the OS's own table via `OS_FSControl 18`/`19`, and HostFS
returns a type number, never a name.  The descriptions in this file are
for the person editing it on the Mac.

### 6.4 Case

RISC OS is case-insensitive; APFS and NTFS usually are, so today's exact
match mostly works and hides the problem.  On a case-sensitive volume it
breaks.  v1 does the RISC OS thing host-side: exact match first, then a
unique case-insensitive scan.  Ambiguity is an error, not a coin toss.

---

## 7. Metadata: load, exec, attributes

Today every file reports `FFFFFFFF FFFFFFFF` — nominally a typed file of
type &FFF bearing an all-ones date, which is to say undated and useless
to the Filer.  `stamp_load_exec()` is guest-side, and wrong.

**Delete it.  The host returns the words ready-made**, from the `stat`
and the type it already has:

    load = 0xFFF00000 | (type << 8) | ((cs >> 32) & 0xFF)
    exec = cs & 0xFFFFFFFF

where `cs` is centiseconds since 1900-01-01, which the device already
computes in `date_cs_for()`.  Directories get object type 2 and
load/exec zero.

Attributes deserve better than today's two bits:

    bit 0  owner read     <- S_IRUSR
    bit 1  owner write    <- S_IWUSR
    bit 3  locked         <- !S_IWUSR, or the sidecar
    bit 4  public read    <- S_IROTH
    bit 5  public write   <- S_IWOTH

and the reverse on `fsfile_WriteAttr`, with the sidecar carrying
anything the host mode cannot express.

---

## 8. Directories — much less work than it looks

The previous draft proposed implementing `*Cat` and wildcard matching
host-side.  `Doc/SimpleFS` says not to bother.  FileSwitch already
handles, for a filing system configured as in §4:

> Directory setting — `*Dir`, `*Lib` etc; parsing paths relative to set
> directories; **performing catalogue listings of any sort (`*Cat`,
> `*Ex` etc)**; parsing `*Access` strings; `^`s in paths.

So the FS supplies only the raw enumeration:

- **`fsfunc_ReadDirEntries` (14)** and **`ReadDirEntriesInfo` (15)** —
  "give me N entries from offset C into this buffer".  The host writes
  the format FileSwitch expects straight into the caller's buffer
  through the MMU, and returns the next offset and count.
- **`fsfunc_ResolveWildcard` (24)** — *return `r4 = -1` and FileSwitch
  does the matching itself.*  No host-side glob, no RISC OS wildcard
  rules to reimplement.
- Reason 19 may be errored; 23 `CanonicaliseSpecialAndDisc`, 27
  `ReadBootOption` (return 0, do not error), 30 `ReadFreeSpace` (worth
  supporting — it is `*Free`), 32 `StampImage`, and 16 `Shutdown`.

The host keeps a short-lived enumeration snapshot keyed by (directory,
offset) so a paged listing is stable while it runs, sorted
case-insensitively, with `.riscos-meta` filtered out.  Invalidate on any
write through the channel and on a short TTL — which is also what keeps
`FSDESIGN.md`'s acceptance that a file appearing on the host shows up in
the guest within a second.

`fsextra_FSDoesCat` and `fsextra_FSDoesEx` exist (`hdr/LowFSI`) to let a
filing system format `*Cat` and `*Ex` itself.  We do **not** want them:
FileSwitch's formatting is free and correct.

---

## 9. Errors

The guest's job is to return a pointer to a `_kernel_oserror`.  It
should not decide what is in it.  The host maps `errno` to a RISC OS
error number and message and returns both — number in `err_num`, text
inline — and the module copies the text into one static buffer and
returns its address.

The numbers the module uses today are raw FileCore values, which is
wrong for a registered filing system.  `Hdr/Global/NewErrors` gives the
convention:

```
ErrorBase_NetFS * &00010000 + ( fsnumber_net :SHL: 8 )
```

**A filing system's error base is `&00010000 + (fsnumber << 8)`**, to
which the standard offsets are added: `FileNotFound` &D6,
`AccessViolation` &BD, `IsADirectory` &A8, `BadRename` &B0,
`DirectoryNotEmpty` &B4, `BadFileName` &CC.  HostFS should use its own
base so its errors are attributable to it, and the strings should match
the standard ones (`ErrorString_FileNotFound` and friends) so they
localise.

Map at least: ENOENT, EACCES, EPERM, ENOSPC, EEXIST, EISDIR, ENOTDIR,
ENOTEMPTY, EMFILE, ENAMETOOLONG, ELOOP, EROFS, and `VMCH_RC_BADADDR`
from §3.  The v0 table in `dde/c/hostfs` is deleted, not ported.

---

## 10. Security

Keep what `host_path()` does — canonicalise, re-check the root prefix,
refuse `..` and absolute paths, refuse Windows device names — and add:

- **Resolve symlinks before the containment check**, not after.  A
  symlink inside the share pointing out of it is the obvious escape.
  `realpath()`, then compare.
- **`readonly=on`** as a device property — and it sets **information
  word bit 16** (`fsinfo_readonly`), so FileSwitch refuses writes before
  they reach the wire, rather than the host refusing them after.
- Enumeration never reveals `.riscos-meta`, and the guest cannot open,
  create or delete inside it by name.
- The doorbell remains reachable by any guest code, as `FSDESIGN.md`
  intended.  The share root is the only perimeter — say so in the user
  documentation, since `RISCOS_HOSTFS=$HOME` would be a poor choice.

---

## 11. Compatibility

Card images in circulation carry the v0 module, and they must keep
working: **the device implements v0 and v1 side by side.**  v0 commands
are untouched, physical-addressed, and stay that way.

The v1 module probes `VERSION` and `FEATURES` at init.  If the FSENTRY
bit is missing it refuses to register with a clear message rather than
falling back — a silent fallback to the path that corrupts memory is
worse than not loading.  The module is soft-loaded from
`!Boot.Choices.Boot.PreDesk`, so updating it is dropping in a file.

---

## 12. What is left in the guest

The point of the whole design, stated as a target:

| Today | v1 |
| --- | --- |
| `dde/c/hostfs`, 1046 lines | ~250–300 lines |
| `xfer()`, `mem_phys()`, page splitting | gone |
| `stamp_load_exec()`, `objtype_of()` | gone |
| `hptr/hext/hload/hexec` arrays | gone — a buffered FS is never asked |
| `put_path()`, path assembly | gone — FileSwitch canonicalises |
| error table | gone |

What remains is irreducible: module header, init and finalisation, the
doorbell mapping (one OS_Memory 13 at init), FS registration, and seven
veneers that all look like this:

```c
_kernel_oserror *fsentry_get_handler(_kernel_swi_regs *r, void *pw)
{
    _kernel_oserror *e = ensure_vmch();
    if (e) return e;
    return vmch_call(VMCH_FSENTRY_GETBYTES, r);   /* copy in, ring, copy out */
}
```

`vmch_call()` is written once.  There is no RISC OS *semantics* left in
the module — which is the property that makes every remaining bug a
host-side bug, fixable in seconds.

---

## 13. Sprints

1. **Transport and streams.**  `cpu_memory_rw_debug` helpers in the
   device, `VMCH_RC_BADADDR`, VERSION 1, the v1 block, and
   `FSENTRY_OPEN/GETBYTES/PUTBYTES/ARGS/CLOSE` host-side.  Module:
   `vmch_call()` and five veneers.  **Clear bit 28 and return a valid
   power-of-two buffer size** (§5) — that alone is the stream fix.
   *Acceptance:* `*Type` prints; `BGET#` returns `l`; the DDE compiles a
   source read straight off HostFS.
2. **The information word.**  Set bits 19/20 so `*Load`/`*Save` become
   Open/GBPB/Close, retire `fsfile_Load`/`Save`, and confirm `*Copy`
   still works through the stream path alone.
3. **Names and types.**  Dot/slash, `,xxx` consumed, the table generated
   from `Hdr/Global/FileTypes`, `typemap=`, `naming=`, case fallback.
   *Acceptance:* `*Ex` shows real types and dates; `readme/txt` opens.
4. **Metadata.**  load/exec/attrs host-side, the `.riscos-meta` sidecar,
   `WriteInfo`/`WriteAttr`.
5. **Directories.**  Func 14/15, the enumeration snapshot, Func 24
   returning −1, 27, 30 (`*Free`), 16.
   *Acceptance:* the Filer opens a HostFS window with correct icons and
   a double-click loads a text file into StrongED.
6. **Edges and measurement.**  Errors on the §9 base, `readonly=`,
   multiple shares, and the doorbell budget of §5 on paper.

### The measurement that goes with sprint 1

`VMCH_TRACE` a 256 KiB read from BASIC — `OPENIN`, then a `BGET` loop,
then again with `OS_GBPB 4` — and count `cmd=` lines.  The design
predicts 1 doorbell for the `OS_GBPB` case and 256 for the `BGET` case;
if either is wildly out, something about the buffered path is not what
§5 thinks it is, and that is worth knowing on day one rather than in
sprint 6.  Nothing about the *design* now hangs on the result: the PRM
and `Doc/SimpleFS` settle what to build.  The number is there to catch a
mistake in the building.

---

## 14. Acceptance

v1 is done when, on a stock card image with the share pointed at a
working directory:

- `*Type HostFS:readme/txt` prints the file, and `BGET#` returns its
  first byte.
- `cc HostFS:c.hello -o HostFS:hello` compiles and links **entirely on
  HostFS**, and the binary runs.
- `*Ex HostFS:` shows honest types and honest dates for every entry.
- The Filer opens HostFS windows with the right icons, and a
  double-click opens the file in the right application.
- A 256 KiB `OS_GBPB` read costs one doorbell round trip, and the
  same read driven by `BGET` costs one per 1024 bytes.
- A file created on the host is visible in the guest within a second,
  with no reboot; a file created in the guest appears on the host with a
  sane name and mtime.
- Nothing in the module knows what a load address is.

---

## 15. Open questions

- **The filing system number.**  The module uses 220.
  `Hdr/Global/FSNumbers` — "the definitive list" — allocates up to 195
  (`Source`, Rob Sprowson), so 220 is unclaimed but also unregistered.
  ROOL's allocations manager exists; ask for one before anything ships
  outside the team.
- **Special fields.**  `HostFS::share.$.foo` is the RISC OS spelling for
  multiple shares, and information word bit 31 plus
  `fsfunc_CanonicaliseSpecialAndDisc` is how it is done.  Worth
  designing in now even if only one root is implemented.
- **`.riscos-meta` versus xattrs.**  The sidecar is portable and
  visible; macOS xattrs are invisible and native but do not survive most
  archives or a copy to another filesystem.  The sidecar is the default
  for that reason; `meta=xattr` is cheap to add if the directory annoys.
- **Zero-copy** via `probe_access_flags()` (§3) — worth it only if the
  doorbell budget shows the copy mattering.
- **`dma_as`.**  If any physical-addressed path survives v1, the device
  should take an `AddressSpace` link rather than using
  `address_space_memory`.
