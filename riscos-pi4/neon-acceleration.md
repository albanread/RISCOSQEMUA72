# NEON — SIMD under TCG, and the knobs around it

Written after asking three questions: does the emulator accelerate the
guest's NEON, should the settings menu have a NEON on/off switch, and
what would the optional TCG CPU features do to RISC OS if flipped.
Everything below was established against this tree's own build — the
CPU model from `target/arm`, the lowering from `target/arm/tcg`, and
every command line quoted was run, not imagined.

---

## 1. What the guest has

`-cpu cortex-a72,aarch64=off` realises `aarch64_a72_initfn`
(`target/arm/tcg/cpu64.c:276`): an ARMv8-A core with `ARM_FEATURE_NEON`
alongside V8, EL2, EL3, PMU and the generic timer, and the feature
registers to match — MVFR0 `0x10110222`, MVFR1 `0x12111111` (every SIMD
field present), MVFR2 `0x00000043`, and `ID_AA64ISAR0 0x00011120`, which
advertises AES, SHA1, SHA2 and CRC32 as well.  So the guest sees a full
VFPv4 + NEON + crypto core, which is what a real Pi 4's A72 is.

## 2. How TCG runs it

NEON is not interpreted opcode by opcode.  `translate-neon.c` lowers
roughly ninety-nine of its sites to the gvec infrastructure — QEMU's
generic vector IR — against twenty-nine that fall back to helpers; the
split is by operation: arithmetic, load/store and the regular permutes
go gvec, the odd corners (VTBL above the fast cases, some lane moves)
go to C.  On this Mac's arm64 host the AArch64 TCG backend emits host
NEON for the gvec ops it implements; on the Windows box, SSE/AVX.  A
guest NEON loop therefore executes substantially as host vector code,
with a helper tax on the corners.

The crypto extension is different: AES and SHA lower to
`crypto_helper.c` helpers, and CRC32 to a table helper — correct and
slow-ish, no host acceleration, and nothing in RISC OS 5.30 calls them.

The practical caveat is that it barely matters for the desktop as
shipped.  The DDE's C compiles floating point to FPA-and-softfloat by
default; the base system's float goes through FPEmulator or the C
library, not NEON registers.  TCG's NEON speed is a question for the
things that actually use it — NEON-enabled builds from ROOL's newer
components, GCCSDK applications built for VFP, media codecs — not for
the feel of the desktop.

## 3. The knobs, tried for real

`neon` is a real property on this CPU — `DEFINE_PROP_BOOL("neon", ...)`
(`target/arm/cpu.c:1332`), installed only under TCG and only because
the model has the feature (`cpu.c:1644`) — and it reads back live:

    qom-get /machine/unattached/device[0] neon  ->  {"return": true}

Turning it off is another matter.  On the command line:

    -cpu cortex-a72,aarch64=off,neon=off
        ->  ARM CPUs must have both VFP-D32 and Neon or neither

    -cpu cortex-a72,aarch64=off,neon=off,vfp-d32=off
        ->  Property 'cortex-a72-arm-cpu.vfp-d32' not found

The consistency rule demands NEON and VFP-D32 move together; on an
ARMv8 model VFP-D32 is architectural, its property is not even
installed, and so the one legal "off" state cannot be expressed.
**NEON is immutable on this CPU model.**  And even where a feature
could move, it is a realise-time decision — over QMP, after boot:

    qom-set /machine/unattached/device[0] neon false
        ->  Attempt to set property 'neon' ... after it was realized

For the record, on models where `neon=off` is expressible it is a
bundle, not a scalpel: `cpu.c:2036-2080` unsets the feature and zeroes
AES, SHA1, SHA2, SHA3, SM3, SM4, DP, FCMA, BF16, I8MM, FHM, RDM and
VCMA in every ID view, and clears the SIMD fields of MVFR1 and MVFR2.
There is no state in which a guest keeps the crypto extension but
loses SIMD.

## 4. Consequences for RISC OS execution, feature by feature

- **NEON, on (the status quo).**  VFPSupport in the 5.30 ROM finds the
  SIMD fields in MVFR1 and manages NEON context; applications built for
  it work.  Nothing in the base desktop executes NEON, so its speed
  under TCG surfaces only in the applications above.
- **NEON, off — unreachable here, but the analysis stands.**  MVFR1
  with the SIMD fields clear is what a Pi 1 looks like, and RISC OS
  boots on those by design; VFPSupport would report no NEON and
  NEON-executing applications would take undefined-instruction traps,
  which the kernel fields.  So it would be a compatibility mode, not a
  crash — if the CPU model allowed it, which it does not.
- **vfp-d32.**  Not a property on this model (mandatory in v8); on
  models that have the knob it removes D16-D31, and VFPSupport copes —
  the Pi 1 precedent again.
- **Crypto and CRC32.**  Advertised and helper-emulated; unused by
  5.30.  No consequence either way, and no way to switch them
  independently of NEON regardless (§3).
- **EL2 and EL3.**  Present in the model, absent from RISC OS's life —
  the OS never enters hyp or monitor mode, and the HAL's boot state is
  what the machine already provides.  Not togglable on this model; no
  consequence observed.
- **PMU.**  Advertised (`ID_DFR0 0x03010066`).  Nothing we know of in
  5.30 reads it; worth one look on the farm (`*Modules`, a
  PMU-touching HAL read) before anyone ships a cpu-model change, but no
  consequence today.
- **`aarch64=off`.**  The load-bearing flag, orthogonal to all of this:
  it fixes the reset state, not the feature set.

The general rule all of this points at: every optional feature is
realise-time state that the ROM reads through ID registers at boot, so
no feature change can be applied to a running machine, and any change
at all is a different machine to the one the HAL has been proven on.

## 5. The settings menu

It cannot honestly have a NEON switch, for the two reasons §3
established: the property refuses to move after realise, so no
runtime toggle exists; and on this CPU model the off state is not
expressible even at launch, so not even a "restarts the machine to
apply" toggle could deliver it.  A menu item that changed the launch
configuration would have to change the CPU model to do it — and a
different model means different cache geometry (CLIDR, CCSIDR) that the
Pi HAL's cache maintenance is keyed to, a boot risk traded for a debug
knob nobody needs.

The recommendation is: no switch, and this document is the record of
why.  If a non-NEON compatibility mode is ever genuinely wanted, the
vehicle is a launcher flag with a different `-cpu` model, gated by the
farm's boot check before it is allowed near a user — never a menu.

## 6. What to measure, on the farm

1. That the status quo is what this document says: `*Modules` shows
   VFPSupport; an application built NEON-on by the DDE runs.
2. A NEON-using workload's wall time against its soft-float build, on
   one machine, both through `run.py` — the only measurement of
   "acceleration" that answers anything, since on/off is not
   switchable.
3. The PMU curiosity from §4, once, for the record.

Not on the list, because unreachable: anything comparing NEON on
versus off on this CPU model.
