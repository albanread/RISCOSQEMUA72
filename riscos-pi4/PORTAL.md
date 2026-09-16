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

`MAP_ENSURE` is the one that answers the lazy-page problem for the
accesses the host initiates on its own schedule, with the guest running
freely between them — the backdrop capture on its settle timer, a
screendump of a mode change, the snapshot path.  The host queues
`MAP_ENSURE`, the next tick faults the pages in through the OS's own
handler, and the capture that would have read a hole simply reads.

The blitter, by contrast, is *not* a portal consumer, and the design
must not pretend otherwise: a blit is serviced inside the `BLIT_GO`
write, which is doorbell context — the guest that would service the
request is parked in the very op asking.  The blitter's lazy-page story
is Phase 1's: the plot fails `BLIT_RC_FAULT`, GVFill touches the
sprite's pages, rewrites `BLIT_GO`, and the plot takes.  §"The
deadlock" applies to the blitter exactly as it does to HostFS.

`PIN` is named, specified, and built last or never: no transfer today
outlives a single doorbell or vector claim.  Written down so the
decision is recorded rather than revisited.

## Code outline

Not built: the DDE has not seen the guest half, and the host half is
written against the vmchannel that exists.  It is close enough to code
that the implementation is typing, not designing.  The wire definitions
are held by both sides and compared at hello, not trusted to drift.

### The wire — one copy each side, guest and host

```c
#define PORTAL_VERSION   1
#define PORTAL_SLOTS     8
#define PORTAL_FEATURE   (1u << 4)     /* vmchannel feature bit, HELLO gate */

/* the donated page, byte offsets.  4096 bytes, ring under ~800, the
 * tail is COPY scratch. */
#define PO_KICK      0    /* uint32: request count, host increments after
                           * writing a slot; the guest polls it on tick */
#define PO_SEQ       4    /* uint32: response seqlock, odd while the
                           * guest is writing, even when done */
#define PO_NEXT      8    /* uint32: next slot to service, guest's */
#define PO_REQ(i)    (16 + (i) * 48)
#define PO_RESP(i)   (16 + PORTAL_SLOTS * 48 + (i) * 48)
#define PO_STATS     (16 + 2 * PORTAL_SLOTS * 48)
#define PO_SCRATCH   (PO_STATS + 32)   /* .. end of page, COPY buffer */

/* request slot */
#define RS_OP     0      /* uint32 */
#define RS_LEN    4
#define RS_VA     8
#define RS_COOKIE 12

/* response slot: mirrors op and cookie, then rc and payload[8] */
#define TS_RC      8      /* POR_*, below */
#define TS_PAYLOAD 12

enum {
    PO_MAP_ENSURE = 1,   /* payload[0] = pages mapped */
    PO_TRANSLATE  = 2,   /* payload: up to 8 {phys, len} runs */
    PO_COPY_IN    = 3,   /* host buffer <- guest va, via scratch */
    PO_COPY_OUT   = 4,
    PO_PIN        = 5,   /* v1 refuses: POR_REFUSED, spec'd not built */
    PO_UNPIN      = 6,
};

enum { POR_OK = 0, POR_REFUSED = 1, POR_FAULT = 2 };

/* Phase 1's one wire change, issued by the host when the walk fails:
 * "not mapped", not "bad address".  Distinct from every RC_ above. */
#define RC_NOTMAPPED 14
```

### The host — vmchannel.c

```c
static struct {
    uint32_t page;                /* donated page's guest va, 0 = absent */
    uint32_t kick;                /* our own count of issued requests */
    uint32_t answered, refused;   /* PO_STATS mirror, for the log */
} portal;

/* in the doorbell dispatch, beside the other C_ cases.  BQL held:
 * the hello is an ordinary synchronous doorbell op. */
case C_PORTAL_HELLO:
    /* arglen 8: version, feature bits, page address.  Version we do
     * not know or a page we cannot reach: answered, not remembered --
     * an absent portal is a supported configuration, not an error. */
    if (version == PORTAL_VERSION && vmch_guest_rw(page, probe, 4, false)) {
        portal.page = page;
    }
    rc = VMCH_RC_OK;
    break;

/*
 * Issue one request and wait for its cookie.  The wait is the whole
 * contract: callers are contexts the guest schedules in -- host
 * timers, QMP handlers -- and a doorbell op that calls this is the
 * deadlock the design forbids.  v1 polls the main loop; a QEMUBH is
 * the refinement, not the correction.
 */
bool vmch_portal_ask(uint32_t op, uint64_t va, uint32_t len,
                     void *resp, uint32_t resp_len)
{
    uint64_t deadline = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1000;
    uint32_t slot = portal.kick % PORTAL_SLOTS, cookie = ++portal.kick;

    if (!portal.page) {
        return false;                          /* absent: caller falls back */
    }
    vmch_st32(portal.page + PO_REQ(slot) + RS_OP, op);
    vmch_st32(portal.page + PO_REQ(slot) + RS_LEN, len);
    vmch_st32(portal.page + PO_REQ(slot) + RS_VA, (uint32_t)va);
    vmch_st32(portal.page + PO_REQ(slot) + RS_COOKIE, cookie);
    vmch_st32(portal.page + PO_KICK, cookie);  /* publish, last */
    while (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) < deadline) {
        /* seqlock read of the slot: even, then cookie ours, then rc */
        ...
    }
    return false;                              /* timeout: caller falls back */
}
```

### The guest — riscos-pi4/portal/portal.c

Against the HostNet module's proven skeleton, whole: freestanding C,
no writable statics outside the workspace, the doorbell request block
our own, the veneers identical.

```c
/* workspace, -zM shape, RMA-claimed */
struct portalws {
    uint32_t page;                    /* the donated page */
    uint32_t live;
    uint32_t cb_pending;
    /* the counters *PortalInfo prints, at PO_STATS for the host too */
    uint32_t ticks, callbacks, asks, mapped, refused;
};

init:
    ws->page = OS_Module 6 (claim 4096, RMA)       /* always mapped:
                                                      the host's writes
                                                      to it cannot walk
                                                      into a hole */
    zero the page; PO_SEQ = 0; PO_NEXT = 0
    ring C_PORTAL_HELLO (version, feature bits, ws->page)
    OS_CallEvery 2cs, tick, static_base()

final:
    OS_RemoveTickerEvent; OS_RemoveCallBack         /* the RMCkill order
                                                      * HostNet learned */
    ring C_PORTAL_HELLO (version, 0)                /* "gone": the host
                                                      * stops asking */
    OS_Module 7 (release the page)

tick:                       /* IRQ context: nothing but the callback */
    if (!live || cb_pending) return;
    cb_pending = 1;
    OS_AddCallBack callback, static_base()

callback:                   /* USR mode: app-space is touchable in mode,
                             * which is the whole trick */
    cb_pending = 0
    while (PO_NEXT != PO_KICK) {
        slot = PO_NEXT % PORTAL_SLOTS
        switch (request[slot].op) {

        case PO_MAP_ENSURE:
            /* OS_ValidateAddress first: it is the gate that says the
             * range is the guest's to fault in.  Refusals are answers,
             * not errors -- the adversarial leg of the test ladder. */
            if (!validate(va, len)) { refused++; answer(POR_REFUSED); break; }
            for (p = va & ~0xFFF; p < va + len; p += 4096) {
                (void)*(volatile uint8_t *)p;   /* the OS's own fault path
                                                  * completes the mapping */
                mapped++;
            }
            answer(POR_OK, pages);

        case PO_TRANSLATE:
            /* our own short-descriptor walk, mrc-veneer TTBCR/TTBR0/1:
             * the tables are complete now, MAP_ENSURE saw to it */
            runs = walk(va, len, payload, 8);
            answer(runs ? POR_OK : POR_REFUSED, runs);

        case PO_COPY_IN:                          /* guest -> scratch */
        case PO_COPY_OUT:                         /* scratch -> guest */
            validate, then copy by words through scratch

        case PO_PIN: case PO_UNPIN:
            refused++; answer(POR_REFUSED);      /* spec'd, not built */
        }
        PO_NEXT++;
        /* response written under the seqlock: odd, fill, even */
    }
    asks = PO_NEXT
```

The one subtlety the outline cannot show and the implementation must
not lose: `MAP_ENSURE`'s touch runs in *callback mode*, so app-space
buffers fault through the USR-mode fault handler, which is the handler
the OS would have used.  A touch from SVC would take the wrong path
for the same page.  HostNet's callback already runs USR; this relies
on nothing new.

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
