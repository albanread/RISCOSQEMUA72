# PORTAL — the guest finishes its page tables for the host

`vmch_guest_rw()` walks the guest's ARMv7 short descriptors to reach the
buffers FileSwitch, the sockets and the sprite paths hand over.  The walk
is right; the tables it walks are incomplete.  RISC OS maps lazily — a
growable area's pages exist as OS bookkeeping before they exist as page
table entries, and first *touch* is what fills them in.  The host's walk
is not a touch: `cpu_translate_for_debug()` cannot fault a page in, it
can only report that the mapping is absent.  So the host sees a hole
where the OS sees a perfectly valid lazy allocation, and we have spent a
weekend proving that was what it was (the lazy-mapping investigation:
the pages were lazy, and the walk was right).

This document designs the fix in two phases, because there is a deadlock
in the middle of the obvious one:

1. **Phase 1 — touch, ring, retry.**  The guest touches a buffer's pages
   before handing it to the host, and the host has a return code for
   "not mapped" instead of collapsing the failure into a generic fault.
   Small, no new module, fixes every failure the doorbell path has today.
2. **Phase 2 — the portal module.**  A host-initiated request queue the
   guest services from a callback: map-ensure, translate-to-physical,
   copy.  For the consumers where the *host* initiates the access — the
   blitter reading a sprite — and for retiring the debug-contract
   translation where the guest can consent instead.

Sprint order is Phase 1 entire, then Phase 2.  Phase 1 is the whole fix
for the failures we are actually bumping into; Phase 2 is the general
service, and §"The deadlock" is why it cannot precede it.

Two properties of the failure shape both phases.  The information that
fills the hole is **guest-private** — for a PMP-backed area the physical
page may sit in a pool with the area association not yet performed;
there is nothing host-side to read, and writing page table entries from
outside the OS would fight the OS's own lazy bookkeeping and lose.  And
the only entity that can complete the mapping is the guest, in SVC or
callback context, through the OS's own fault path.

---

## The deadlock

The natural design — the host's walk fails mid-doorbell, the host asks
the guest to map the page, retries — deadlocks.  The doorbell op runs
synchronously inside the guest's MMIO write, under the BQL, on the vCPU
that rang.  The code that would service a portal request is guest code;
the guest is stopped inside the doorbell.  Nothing services the queue
until the doorbell returns, which is waiting on the queue.  Secondary
cores do not rescue it: RISC OS 5.30 does not schedule on them, and they
are not running our callback chain.

So the portal is **not** the recovery mechanism for doorbell-path
transfers, by construction.  Those take Phase 1.  The portal serves the
accesses the host initiates on its own, where no guest is parked in a
doorbell waiting for it.

## Phase 1: touch, ring, retry

**Host:** one new return code, `RC_NOTMAPPED`, in place of today's
collapse of a walk failure into `BADADDR` or a generic fault.  The
diagnostic page-table dump stays — it earned its keep once already.

**Guest, in every module that hands the host a buffer** (HostFS's
transfers, HostNet's iovecs and sockaddrs, GVFill's sprite paths):
before ringing, touch one byte per page of the range, in the mode the
buffer belongs to, so the fault handler that maps the page is the one
the OS would have used anyway.  It costs an `LDRB` a page.  It is also
the correct semantics, not a workaround: the buffer is about to be read,
and making it present first is what the OS's own DMA-using code does.
An `RC_NOTMAPPED` that survives the touch (a race the touch missed) is
answered by touching again and retrying once — the address and length
are in hand.

Rough size: ~30 lines a module, ~10 host-side, and no protocol beyond
the one return code.

## Phase 2: the shape

Two halves, riding the vmchannel device, changing nothing about the
doorbell:

- **Guest:** a module, `Portal` — soft-loadable, `*RMKill`-able, no
  writable statics outside the workspace, init that refuses politely
  when the device lacks the portal feature bit (the `ensure_vmch`
  pattern).
- **Host:** a request ring in a page the module donates at handshake.
  No new MMIO region.

**Delivery is ticker + callback**, the HostNet pattern verbatim:
`OS_CallEvery` at 50 Hz, `OS_AddCallBack` from the tick, service in
USR-mode callback.  Worst case 20 ms of latency, which is nothing to
operations that are setup-time by nature — translate-then-DMA, copy a
sockaddr.  A real IRQ needs a HAL device entry we do not own; deferred
until a consumer is latency-sensitive, and none on this list is.

**Handshake:** at init the module rings the existing doorbell with
`C_PORTAL_HELLO` — protocol version, feature bits, the donated page's
address.  The host records it and starts issuing requests.  Absence of
the hello leaves the host exactly as it is today: a stock ROM, or a farm
instance with the module unplugged, degrades by construction, the
property HostFS already has.

## The ring

Fixed-size request slots and response slots, each with a sequence
count.  The guest takes requests in order, one at a time from the
callback; responses are written before the sequence increments.  The
discipline is the framebuffer seqlock's: writer increments to odd,
fills, increments to even; a host reader that sees odd, or a changed
counter after reading, reads again.  Eight slots; a host that overflows
waits on the ticker cadence rather than overwriting — the portal is
never on a guest's critical path, so overflow is a host bug to be
counted, not a guest emergency.

## The operations

Four, no more, each with a length cap and a real error block:

| Op | Guest does | Host gains |
| --- | --- | --- |
| `MAP_ENSURE(va, len)` | touches the range page by page, in the buffer's mode; reports pages mapped and refusals | the walk cannot fail on this range again |
| `TRANSLATE(va, len)` | validates the range, then walks the now-complete tables itself; answers up to eight (phys, len) runs | physical addresses with consent — retires `cpu_translate_for_debug` at each call site that adopts it |
| `PIN(va, len)` / `UNPIN` | notes the range; the portal defers where it can, refuses where it cannot | stability for a transfer that spans guest execution |
| `COPY_IN/OUT(buf, va, len)` | validated copy, in the callback | small transfers with no walk at all |

`MAP_ENSURE` is the one that answers the lazy-page problem for
host-initiated consumers: the blitter takes a sprite plot, the host's
walk hits a hole, the host queues `MAP_ENSURE`, the next tick faults the
pages in through the OS's own handler, the host retries the DMA — and a
plot that would have been a fallback (or, before the DMA-fault fix,
silent garbage) simply completes.

`PIN` is named, specified, and built last or never: no transfer today
outlives a single doorbell or vector claim.  Written down so the
decision is recorded rather than revisited.

## What this is not

- **Not a SWI proxy.**  The host cannot ask the guest to execute
  arbitrary calls.  Four memory operations, versioned, declining-able.
  The QMP hardening was the guest not reaching host surfaces uninvited;
  this is the mirror — the host's power over the guest stays narrow,
  named, and auditable.  No execution op, ever, however convenient.
- **Not a replacement for the debug walk on day one.**  Consumers adopt
  at their own pace; the walk is the fallback while the portal is
  absent.  `cpu_translate_for_debug` retires when its last caller does.
- **Not host-side page-table work.**  Writing descriptors from outside
  the OS is the one alternative strictly worse than the disease.

## Testing ladder

1. `*PortalInfo`, with counters — asks serviced, pages mapped,
   refusals.  "The guest never asked" and "the guest refused" must be
   tellable apart, the lesson `*HostNetInfo` already encodes.
2. A farm instance with the module unplugged: every consumer falls back
   to today's path, verified by the counters showing zero portal
   traffic.
3. The adversarial leg: the host asks for a range with a genuine hole —
   an area the handler refuses to grow — and the guest answers a
   refusal block the host honours.  The refusal path is a feature, not
   an error in the test.
4. Phase 1 verified by constructing the lazy case on purpose: a sprite
   area extended past its touched pages, plotted through GVFill, before
   and after the touch loop.

## Acceptance

Phase 1: a HostFS transfer of a buffer spanning untouched lazy pages
completes without `RC_NOTMAPPED` leaving the host; the same sprite plot
through GVFill takes the blitter path, not the SpriteExtend fallback.

Phase 2: the blitter's sprite reads answer from `TRANSLATE` runs with
the module present, fall back to the debug walk with it absent, and the
counters say which happened.
