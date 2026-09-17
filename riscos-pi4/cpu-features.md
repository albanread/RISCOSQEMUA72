# CPU features — what the guest has, what moves, and the plan for changing any of it

This is the umbrella over two narrower documents:
[`neon-acceleration.md`](neon-acceleration.md) is the NEON question and
its measurements; [`tcg-profiling.md`](tcg-profiling.md) is the
profiling tooling.  What belongs here is the inventory of optional CPU
features on the A72 guest — every property probed live over QMP, every
refusal quoted from a run — and the evaluation plan any feature change
must pass before it goes near a user.  The general rule both halves
pointed at, written once: every feature is *realise-time state the ROM
reads through ID registers at boot*, so nothing about the feature set
can change on a running machine, and any change at all is a different
machine to the one the HAL has been proven on.

---

## 1. What the guest has

`aarch64_a72_initfn` (`target/arm/tcg/cpu64.c:276`): an ARMv8-A core
with NEON, VFP, EL2, EL3, the PMU and the generic timer, advertising
MVFR0 `0x10110222`, MVFR1 `0x12111111`, MVFR2 `0x00000043` (full
VFPv4 + NEON) and `ID_AA64ISAR0 0x00011120` (AES, SHA1, SHA2, CRC32).
Plus the one flag of ours: `aarch64=off`, which fixes the reset state,
not the feature set.

## 2. What moves, and what does not — probed live

    qom-get /machine/unattached/device[0]
        neon      = true     (exists; off-state inexpressible, below)
        pmu       = true     (exists)
        aarch64   = false    (exists; ours, the load-bearing one)
        dsp       = Property 'cortex-a72-arm-cpu.dsp' not found
        vfp-d32   = Property 'cortex-a72-arm-cpu.vfp-d32' not found

- **`neon`**: the property is real, but `-cpu ...,neon=off` is refused
  ("ARM CPUs must have both VFP-D32 and Neon or neither") and the
  companion `vfp-d32` does not exist on a v8 model, so the one legal
  off-state cannot be written.  On this model NEON is immutable; the
  details and the ID-register bundle that an off-state would drag with
  it are in [`neon-acceleration.md`](neon-acceleration.md) §3.
- **`pmu`**: present and readable.  Nothing in 5.30 that we know of
  reads it; the one look on the farm (§4, gate 5) is still owed before
  any change rides on it.
- **`dsp`, `vfp-d32`**: not properties here at all.  Pre-v8 knobs.
- **crypto and CRC32**: not separate properties; they ride the model
  (and the NEON bundle where that applies).
- **EL2/EL3**: features of the model, not properties; RISC OS never
  enters hyp or monitor mode, so they are inert.
- **`aarch64`**: the exception — a real property that moves, and the
  whole 32-bit guest hangs off it.  Any other model-level property
  change is on the same footing as this one and owes the same boot
  proof.

And the rule for all of them, including `pmu`:

    qom-set /machine/unattached/device[0] neon false
        ->  Attempt to set property 'neon' ... after it was realized

Realise-time only.  Launch-time arguments or nothing.

## 3. What each would do to RISC OS

One line each; the reasoning lives in
[`neon-acceleration.md`](neon-acceleration.md) §4.  **NEON off** (were
it expressible): a Pi-1-shaped machine — MVFR1 SIMD fields clear,
VFPSupport adapts, NEON-executing applications take clean
undefined-instruction traps, boot unaffected.  **PMU off**: nothing we
know of notices.  **Crypto/CRC**: unused by 5.30 either way.
**EL2/EL3**: inert.  **A different CPU model** (the only route to a
non-NEON compat mode): different cache geometry — CLIDR, CCSIDR — that
the Pi HAL's cache maintenance keys on, which is why a model change is
a boot-risk question and not a flag.

## 4. The evaluation plan — how a feature change earns its way in

Any proposed change to the CPU line — a feature flipped, a property
added, a model swapped — climbs the same five gates, in order, and the
reports named are the ones [`tcg-profiling.md`](tcg-profiling.md)
verified:

1. **Compile gate.**  The CI builds it on all three legs.  Free, and
   catches nothing about the guest — it exists so the later gates can
   run.
2. **Boot gate.**  A farm instance with the changed `-cpu` reaches the
   desktop (`run.py`'s wait-for-desktop, `farm.py up`).  The machine is
   proven on exactly one CPU model; this gate re-proves it.  For a
   model swap this is the hard one, for the §3 cache-geometry reason.
3. **Instruction-usage gate.**  The same boot under
   `RISCOS_PLUGINS=hotblocks,ips`: total instructions to desktop and
   the top blocks, attributed to modules against `*Modules`, compared
   against the same run on the unchanged line.  A change that claims
   an improvement must show it here — fewer instructions, or the same
   instructions concentrated differently — or the claim is withdrawn.
4. **Workload gate.**  A `redrawbench` session under the plugins, the
   heaviest repeatable desktop workload the tools drive.  NEON-related
   changes additionally owe the NEON-share measurement (the A32
   classifier, `tcg-profiling.md` §4) before and after.
5. **Compatibility gate.**  `*Modules` shows VFPSupport; applications
   built against the affected features run; the refusal paths behave
   as §3 says they must.

A change that cannot express its claimed benefit in these reports does
not ship.  A change with no claimed benefit — a debug knob, a compat
mode — ships only if it passes gates 2 and 5, and is documented here
with the gates' outputs attached.

## 5. Worked example: the non-NEON compat mode

[`neon-acceleration.md`](neon-acceleration.md) §5 raised and rejected
a settings-menu NEON switch; here is the same idea as a plan, so the
shape of a real evaluation is on paper.  The off-state is inexpressible
on cortex-a72 (§2), so the vehicle would be a different CPU model with
the same ARMv8-A shape minus SIMD.  Gate 1: passes, a model is data.
Gate 2: the risk concentrates here — the model's cache geometry and
peripherals must satisfy the Pi HAL, and nothing shorter than a farm
boot answers it.  Gates 3-4: baseline runs on both lines, expect the
desktop's instruction count barely to move (the base system does not
execute NEON).  Gate 5: VFPSupport present, a NEON-built application
traps cleanly on the compat line.  If all five held, the mode would
land as a launcher flag — never a menu item, for the reasons the NEON
document gives.
