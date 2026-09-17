# Reliability review: host↔guest memory — 2026-09-17

Scope: every path by which the host reaches guest memory, or the guest
hands memory to the host — the MMU walk in `hw/misc/vmchannel.c` and its
consumers (HostFS transfers, HostNet iovecs and sockaddrs, the blitter's
sprite reads), the framebuffer config hand-off to the UI threads, the
vchiq pointer/audio services, and the hrtimer dispatch that touches
device state.  Static review of the tree at `74ec52d`; nothing here was
executed.  Two passes: the walk core and its consumers read directly,
the fb/vchiq/timer slice by a second reviewer whose quotes were checked
against the source.

---

## Findings

### R1 — HIGH (architectural, dormant today): the walk's safety is an assumption about the guest, not a property of the emulator

`hw/arm/raspi.c:384`:

```c
    mc->default_cpus = mc->min_cpus = mc->max_cpus = cores_count(board_rev);
```

Every launch creates four vCPUs whether asked or not, and no launcher
passes `-accel tcg,thread=single`.  On an arm64 or x86-64 host, QEMU's
default for an aarch64 guest is MTTCG.  The doorbell handler runs under
the BQL, but under MTTCG the other three vCPU threads execute guest code
concurrently with it — and `vmch_guest_rw()`
(`hw/misc/vmchannel.c:248`) and `blit_src_xlate()`
(`hw/misc/riscos_blitter.c`) read the guest's page tables with no
snapshot.  A descriptor rewritten by another core mid-walk can tear, and
the bad case is not a fault: a half-updated descriptor can still
translate, to the **wrong physical page**, and the transfer silently
corrupts whatever lives there.

Dormant because RISC OS 5.30 schedules only core 0 and the secondaries
sit quiescent — an invariant of the guest OS version, unenforced and
undocumented by the emulator.  The cheap belt is
`-accel tcg,thread=single` in the launchers: for a single-scheduled
guest it costs nothing (the parked cores block anyway) and turns a
formal race into an impossible one.  The TCG mode also belongs in the
start-up log, so a future `-accel` experiment cannot reintroduce it
silently.

### R2 — MEDIUM: the framebuffer seqlock's "odd = retry" is broken

`hw/display/bcm2835_fb.c:300-306`:

```c
    do {
        gen = s->generation;
        if (gen & 1) {
            continue;                    /* write in flight */
        }
        *out = s->config;
    } while (gen != s->generation);
```

`continue` in a `do { } while` jumps to the controlling expression, not
the top of the body.  A reader that samples an odd generation — a
`bcm2835_fb_reconfigure` between its two increments — can leave the
loop having **never written `*out`**, with the generation stable odd,
and no caller checks.  The UI then builds a frame from an uninitialized
stack `BCM2835FBConfig` (readers at `ui/metal.c:83`, `ui/dx11.c:61`,
`ui/metal.c:636`, `hw/misc/bcm2835_vchiq.c:722`).  One garbage frame,
self-healing — but this is exactly the interleaving the lock exists
for.  The loop must retry, not fall through.

### R3 — MEDIUM: the config seqlock has no barriers or atomics, on either side

Writer at `bcm2835_fb.c:272-274` — plain `s->generation++` around a
plain struct assignment; reader — plain loads, no `smp_rmb`.  Formally
a C11 data race; practically, on the weakly-ordered arm64 host the
Metal front end targets, the struct copy's stores can become visible
after the closing counter store, so a reader accepts a stable even
generation over a half-old half-new config: a torn mode (old `bpp`,
new `xres`, wrong `pitch`).  The correct idiom already exists in this
tree — `bcm2835_vchiq_get_cursor` (`bcm2835_vchiq.c:976-991`) uses
`qatomic_read` + `smp_rmb` — and never migrated.

### R4 — MEDIUM: reset and vmstate load write the config outside the protocol

`bcm2835_fb.c:528-529` assigns the whole config and only then bumps the
generation by two; `vmstate_bcm2835_fb` (`:454-504`) loads the config
field-by-field with the generation untouched until `post_load`.  A UI
thread rendering during a machine reset or a snapshot restore
(`metal_glue_load_snapshot`) can read a stable even generation over a
half-loaded config.  Snapshot-restore-under-render is a supported
operation; the protocol must cover its writers.

### R5 — MEDIUM (functional, not corruption): HostNet and GVFill have no touch loop

The lazy-page protocol — touch before handing a buffer to the host,
short-count with R3 on a partial walk, touch the tail, retry once — is
implemented end-to-end in the HostFS GBPB path and nowhere else:

- `riscos-pi4/hostnet/hostnet.c`: iov arrays, sockaddrs and message
  headers reach the host untouched; an in-slot-but-untouched buffer
  fails the socket call (`HN_RC_BADADDR`) that real hardware would
  have served.  Clean failure, thanks to the counted walk — but a
  reliability gap against real hardware.
- `riscos-pi4/blitter/blitmod.s`: sprite paths reach
  `blit_src_xlate` untouched; the plot faults and GVFill falls back to
  SpriteExtend — correct pixels, silently un-accelerated.

(Both files' only "touch" mentions are comments.)  This is the
remaining Phase 1 work in PORTAL.md.

### R6 — LOW: `SET_PALETTE` checks offset and length independently

`hw/misc/bcm2835_property.c:513-531`: `offset > 255 || length > 256`
lets `offset = 255, length = 256` write palette entries 255..510 — up
to 1020 bytes past the 1024-byte palette into vcram.  No host OOB (the
writes stay inside the vcram RAM region), but it is guest-controlled
divergence from the hardware's array bounds.  The check needs
`offset + length ≤ 256`.

### R7 — LOW: cursor pixels are copied without a post-copy re-check

`bcm2835_vchiq.c:735-783` snapshot-checks the cursor *metadata*
correctly, but hands out `s->ptr_image` as a pointer and the UI copies
the pixels afterwards (`ui/metal.m:1573-1577`, `ui/dx11.cpp:2304-2321`)
with no post-copy generation re-check — a `disp_commit` landing mid-
`memcpy` tears the sprite for one frame.  The writer's `s->ptr_gen++`
is also a plain increment (same defect class as R3).  Visual only;
all sizes are fixed 64×64 and bounded.

### R8 — LOW: `script_screendump` ignores read failure and encodes uninitialized heap

`ui/metal.c:650-662`: `address_space_read`'s return is not checked; a
failed row read decodes uninitialized `g_malloc` heap into the PNG
written under Application Support — a same-process heap disclosure into
a file the scripting agent then reads.  Second nit: this path and the
shaders treat `xoffset` as pixels while the device console path
(`bcm2835_fb.c:199`) adds it as raw bytes — a panned 16/32 bpp mode
renders shifted on VNC versus the native UIs.

### R9 — LOW (trap for future work): Func 9/10/11 must validate for themselves

The HostFS module's own comment at `dde/c/hostfs:663-672` records the
PRM's warning that FSEntry_Func 9/10/11 buffers arrive *unvalidated* —
`touch_pages` is safe only on the GBPB path, where FileSwitch knows the
count.  Those entries are unwritten today; whoever writes them inherits
the obligation.  Recorded here so it is not rediscovered as a bug.

### R10 — LOW: the walk's CPU choice is de-facto, not documented

`vmch_guest_rw` uses `current_cpu ?: first_cpu`: correct in doorbell
context (the ringing CPU's tables) and de-facto correct from BQL-thread
callers because RISC OS 5.30 keeps one address space and core 0's
tables are canonical.  Worth a comment saying so; if the OS ever ran a
task with its own L1 table on a secondary core, silent wrong-page
transfers would return (see R1).

---

## Verified correct (the counter-evidence)

1. **The walk core is disciplined end to end** (`vmchannel.c:248-293`):
   `address_space_map`/`unmap` strictly paired including the failure
   path, `plen` shortening handled before the copy, unmap write-counts
   only what was written.
2. **The lazy-page protocol exists and is measured** — HostFS GBPB:
   `touch_pages` (`dde/c/hostfs:674`, one read byte per page, final
   partial page included), the host's `guest_rw_counted` short count
   with R3 progress (`vmchannel.c:2250`, `:2283`), and the
   touch-and-retry-once loop (`dde/c/hostfs:730-738`), with the
   252-of-256KiB measurement in the comment.
3. **The blitter's mapping window is correct** (`riscos_blitter.c:478-522`):
   runs extend only while pages stay physically flat inside the
   mapping; boundaries drop and remap; DMA faults are faults, not
   pixels.
4. **HostFS read paths zero short-read tails**, so a guest buffer never
   keeps stale data past what was delivered.
5. **The fb view cannot overflow on a torn config**: `address_space_map`
   clamps the length and rows are recomputed from the clamped mapping
   (`ui/metal.c:107-134`, `ui/dx11.c:84-107`) — the property that
   downgrades R2-R4 from corruption to transient artifact.  It deserves
   a comment saying it is load-bearing.
6. **The threading claims hold**: hrtimer dispatch takes the BQL before
   callbacks (`system/hrtimer.c:113-129`); the audio callback reaches
   the doorbell only via the main-loop audio timer (coreaudio's render
   thread touches no guest state); vchiq bounds every guest-supplied
   index before use (slot zero, queue entries, message sizes,
   RESOURCE_CREATE dimensions, the bulk pagelist —
   `bcm2835_vchiq.c:1030-1144`, `:856`, `:876`, `:218-272`).

---

## Priority order

1. R2, R3, R4, R6 — real bugs, small fixes, all compile-gated by the
   macOS leg (R6 is `hw/`, all legs).
2. R1 — one flag per launcher plus a start-up log line; kills the only
   silent-wrong-page class in the tree.
3. R5 — the touch loops in HostNet and GVFill close Phase 1 of
   PORTAL.md entirely (HostFS already has them).
4. R7, R8 — one-frame cosmetics and a disclosure worth closing.
5. R9, R10 — comments, so the obligations are inherited rather than
   rediscovered.

Also owed: PORTAL.md's Phase 1 section describes the touch/retry work
as unbuilt across HostFS, HostNet and GVFill; HostFS GBPB already has
it end-to-end.  The design document should say so — the remaining work
is the other two modules.
