# Sound: what RISC OS asks for, and the least we can build to answer it

RISC OS on the emulated Pi 4 is silent. This is the design for making it
not be — researched against the ROM's own sources rather than guessed at,
because the sound path turns out to be a conversation, not a chip.

**The short version.** There is no audio hardware to emulate. RISC OS's
`BCMSound` is, in its own words, a "VCHIQ audio service controller": it
opens the VCHIQ service `'AUDS'` and ships PCM to the VideoCore over it.
So sound is nine message types and one bulk transfer on a channel the
fork already owns — `hw/misc/bcm2835_vchiq.c` — handed to QEMU's audio
backend, which is CoreAudio on macOS and DirectSound on Windows and is
already compiled into both builds. No PWM, no I2S, no VideoCore.

That is the design principle landing about as well as it ever will:
> rather than emulating videocore; we should emulate the functions needed
> by RISC OS … let's be as soft and fake as possible and emulate hardware
> only if we are desperate

## 1. What is there today: nothing, on both sides

QEMU models no Raspberry Pi audio at all. The I2S block is an
unimplemented-device stub (`create_unimp(s, &s->i2s, "bcm2835-i2s", …)` in
`hw/arm/bcm2835_peripherals.c`), there is no PWM audio model, and
`hw/audio/` contains nothing Broadcom. Nothing needs deleting; there is
simply nothing to build on.

On the fork's side the VCHIQ peer answers CONNECT and then **refuses every
service the guest opens** — `AUDS`, `GCMD`, `DISP`, `TVSV` — by replying
`VCHIQ_MSG_CLOSE` to the OPEN. `DESIGN.md` §11 records why that refusal
was right, and specifically that "accepting `AUDS` walks BCMSound into a
blocking `MsgQueue`". §2 is what that sentence actually means.

## 2. Why accepting AUDS hangs today, exactly

From `BCMSound`'s own source (ROOL,
`RiscOS/Sources/HWSupport/Sound/BCMSound`, `s/Module`):

```arm
SendWithResult
        ; Entry: sp contains message to send
        MOV     r0, #0
        STRB    r0, GotResult
        ...
        SWI     XVCHIQ_MsgQueue
        EXIT    VS
        ; Wait for response
        ; TODO - Sleep
        MRS     r1, CPSR
        BIC     r2, r1, #I32_bit
        MSR     CPSR_c, r2          ; force IRQs on
10
        LDRB    r0, GotResult
        TEQ     r0, #0
        BEQ     %BT10               ; spin. no timeout, no deadline.
```

An unbounded busy-wait, exactly the shape of the `VCHIQ_Connect` problem
the fork already solved. It is reached from four places, and each is a
message that **must** be answered with `VC_AUDIO_MSG_TYPE_RESULT`:

| Call site | Message | When |
| --- | --- | --- |
| `KillModule` | `CLOSE` | module death |
| `AudioSetRate` | `CLOSE` then `CONFIG` | every sample-rate change |
| mixer | `CONTROL` | volume/mute |

Note what is *not* in that list. Module init sends only `OPEN`, with
`VCHI_FLAGS_BLOCK_UNTIL_QUEUED`, and does not wait for a result:

```arm
        LDR     r0, =&41554453 ; 'AUDS'
        ...
        SWI     XVCHIQ_ServiceOpen
        ; Open audio service
        MOV     r1, #VC_AUDIO_MSG_TYPE_OPEN
        SWI     XVCHIQ_MsgQueue
        SWI     XVCHIQ_ServiceRelease
```

So **accepting the service costs nothing at boot**. The hang arrives later,
at the first `AudioSetRate`. A peer that accepts `AUDS` and answers
`RESULT` to `CLOSE`, `CONFIG` and `CONTROL` never meets it.

## 3. The whole conversation

`vc_vchi_audioserv_defs.h` in Linux's `vc04_services/bcm2835-audio` is the
de facto spec, and BCMSound's own equates agree with it message for
message. A message is `s32 type` plus 16 bytes of payload — 20 bytes.

| Dir | Message | Payload | Sent when | Reply needed |
| --- | --- | --- | --- | --- |
| →VC | `OPEN` (4) | — | init; and again on every rate change | none |
| →VC | `CLOSE` (5) | — | rate change, module death | **`RESULT`** |
| →VC | `CONFIG` (2) | channels, samplerate, bps | rate change | **`RESULT`** |
| →VC | `CONTROL` (3) | volume, dest | mixer | **`RESULT`** |
| →VC | `START` (6) | — | DMA enable | none |
| →VC | `STOP` (7) | draining | DMA disable | none |
| →VC | `WRITE` (8) | count, cookie1, cookie2, silence, max_packet | per buffer, then a bulk transfer | **`COMPLETE`**, repeatedly |
| VC→ | `RESULT` (0) | success | | |
| VC→ | `COMPLETE` (1) | count, cookie1, cookie2 | | |

`CONFIG` is always **2 channels, 16 bits**, at one of nine rates from
BCMSound's `AudioRateTable`: 8000, 11025, 12000, 16000, 22050, 24000,
32000, 44100, 48000. That is exactly an `audsettings` — `AUDIO_FORMAT_S16`,
`nchannels = 2`, little-endian.

Cookies are free: BCMSound sends `WRITE` with `cookie1 = cookie2 = 0` and
ignores them on the way back. (Linux sends `'BCMA'`/`'DATA'` and checks.)
Echoing whatever arrived costs nothing and keeps both clients happy.

## 4. `COMPLETE` is the clock — and that is the whole trick

This is the part worth getting right, because it is what makes the design
cheap. From `VCHIQCallback`:

```arm
        ; The audio service typically sends multiple VC_AUDIO_MSG_TYPE_COMPLETE
        ; responses to each buffer we send
        ; r0 contains the number of bytes consumed by this message, with bit 30
        ; acting as a 'audio starved' flag.
```

BCMSound accumulates that byte count into `IdleBytes`; each time it crosses
`BuffSize` it counts a completed buffer, increments `IdleBuffers` and calls
SoundDMA's callback once per buffer. SoundDMA then fills the next buffer,
and the RTSupport thread sends it.

**So the rate at which we send `COMPLETE` is the rate at which RISC OS
generates sound.** There is no other clock in the loop.

Which means the pacing should come from the audio backend itself. QEMU's
output API is a pull model — `audio_be_open_out()` takes a callback
invoked with the number of bytes the backend can accept — and that
callback fires at the real rate the host's sound card consumes. Report in
`COMPLETE` exactly what `audio_be_write()` took, and the guest is clocked
by the actual output device: no timer to tune, no drift against the host,
and the "starved" bit available to say so honestly when the host underruns.

The fork already has a precedent for this shape — the vsync generator on
`system/hrtimer.c` delivering the signal RISC OS waits on rather than
modelling the thing that would have produced it. Sound is the same idea
with the clock supplied by CoreAudio instead of a timer thread.

## 5. The one piece of real protocol work: bulk receive

The PCM does not travel in the message. `BufferCallback` sends the `WRITE`
header and then:

```arm
        LDR     r0, VCHIQService
        ADR     r1, Buff0
        LDR     r1, [r1, r7, LSL #2]    ; the SoundDMA buffer
        LDR     r2, BuffSize
        MOV     r3, #VCHI_FLAGS_BLOCK_UNTIL_QUEUED
        SWI     XVCHIQ_BulkQueueTransmit
```

The peer today knows five message types (`PADDING`, `CONNECT`, `OPEN`,
`OPENACK`, `CLOSE`). It needs two more. From RISC OS's own
`vchiq_core` header — the guest's copy, so authoritative for what will
appear on the wire:

```c
#define VCHIQ_MSG_BULK_TX            7  /* + (srcport, dstport), data, size  */
#define VCHIQ_MSG_BULK_TX_DONE       9  /* + (srcport, dstport), actual      */
```

On a Pi the VideoCore is the bulk **master** — the guest's parser asserts
`WARN_ON(!state->is_master)` on receiving a `BULK_TX`, and
`WARN_ON(state->is_master)` on a `BULK_TX_DONE`. We play the VideoCore, so
we are the master: we receive `BULK_TX`, do the copy ourselves, and reply
`BULK_TX_DONE` carrying `actual`.

The `data` word is **not** the buffer. `vchiq_prepare_bulk_data` sets
`bulk->data` to the bus address of a `PAGELIST_T`:

```c
typedef struct pagelist_struct {
    u32 length;
    u16 type;      /* PAGELIST_WRITE = 0 for a transmit */
    u16 offset;    /* where the data starts in the first page */
    u32 addrs[1];  /* page-aligned address | (consecutive page count - 1) */
} PAGELIST_T;
```

with the run-length packing done explicitly:

```c
addrs[k++] = addr | ((len >> PAGE_SHIFT) - 1);
```

So walking it is: for each entry, `base = addr & ~0xfff`,
`pages = (addr & 0xfff) + 1`; the data begins at `offset` in the first page
and runs for `length` bytes. Twenty-odd lines against
`dma_memory_read`, which the peer is already doing arithmetic on guest
memory to support. BCMSound sets `WantUnalignedBulkTx = 0`, so the
fragment path (`PAGELIST_READ_WITH_FRAGMENTS`, partial cache lines) is not
needed for playback.

## 6. Where it meets QEMU

The audio backend is already there. `-audio help` on the macOS build
lists `coreaudio`; the Windows build has `dsound`. QEMU 11.1's device-side
API is small:

```c
AudioBackend *audio_be;                  /* DEFINE_AUDIO_PROPERTIES(…) */
audio_be_check(&s->audio_be, errp);      /* in realize */
s->voice = audio_be_open_out(s->audio_be, s->voice, "bcm2835-vchiq",
                             s, audio_cb, &as);
size_t n = audio_be_write(s->audio_be, s->voice, buf, len);
audio_be_set_active_out(s->audio_be, s->voice, on);
```

One ring buffer sits between the two sides: the VCHIQ side fills it from
bulk transfers, `audio_cb` drains it into `audio_be_write`, and the number
of bytes drained becomes the next `COMPLETE`. Both ends already run under
the BQL, which is where the peer queues messages and rings the doorbell
today.

Priming matters. On `START`, BCMSound sends a burst of buffers up front —
"it looks like the videocore is only happy if it thinks it has at least
50ms of data available" — computed as `SampleRate/(BuffSize*5)`, rounded
up, forced even, minimum two. The ring must be able to take that burst
before the first `COMPLETE` goes back, or the guest stalls at the start of
every sound.

## 7. The shape of the work

- **S1 — accept, and answer.** Accept the `AUDS` open (keep refusing
  `GCMD`, `DISP`, `TVSV`), decode the nine audio message types, answer
  `RESULT{0}` to `CLOSE`, `CONFIG` and `CONTROL`. Discard the audio.
  *Proves the hang is gone and RISC OS's sound system comes up.* The
  `Sound` module's `*Speaker` and a `*Play` should complete rather than
  wedge, with silence.
- **S2 — bulk receive.** `BULK_TX` → walk the pagelist → copy → reply
  `BULK_TX_DONE`. Reply `COMPLETE` immediately for the full count.
  *Proves the data arrives and the guest keeps feeding.* Dump the PCM to a
  `.wav` and look at it; the fork already prefers a file it can inspect to
  a thing it must witness (`METAL_SHOT_EVERY`, `-display none` screendumps).
- **S3 — real output.** Open the voice from `CONFIG`, write from the ring
  in `audio_cb`, and move `COMPLETE` onto what the backend actually took.
  *This is where it makes a noise.*
- **S4 — the edges.** Rate changes mid-stream (`CLOSE`/`OPEN`/`CONFIG`
  without a gap), `STOP` with draining, the starved bit, `CONTROL` volume
  applied rather than merely acknowledged, and migration state for the
  ring.

S1 is small and self-contained, and it is worth doing on its own even if
nothing follows: it removes a known hang from a path RISC OS takes
whenever anything touches the sample rate.

## 8. What we deliberately do not do

- **No PWM or I2S model.** The headphone jack on real hardware is PWM, and
  BCMSound's own comment notes the PWM driver "is capable of reproducing
  VIDC rates exactly". None of that is reachable from RISC OS on a Pi 4,
  which goes through the GPU. Modelling it would be emulating hardware the
  guest never touches.
- **No `GCMD`, `DISP` or `TVSV`.** They stay refused. `DESIGN.md` §11 is
  clear that accepting `TVSV` arms two more untimed spins, and declining
  `DISP` is what keeps mode setting on the property channel that QEMU
  models completely.
- **No ROM patching.** `tools/patch-rom-nogenet.py` exists because
  EtherGENET dereferences a null pointer no device model can prevent.
  BCMSound has no such bug: it asks reasonable questions and waits for
  answers. Answering them is smaller than patching, and survives the next
  ROM release.
- **No audio input.** RISC OS has no capture path here worth the trouble.

## 9. Open questions, to be settled on the wire

The project's method is to read the structures out of a live guest before
writing a line, and these are the things to read:

1. **Does `BULK_TX_DONE` want `actual` alone, or a longer payload?** The
   header comment says `actual`; confirm against what the guest's parser
   reads before trusting it.
2. **How large is `BuffSize`?** It comes from SoundDMA via
   `AudioCustomDMAEnable`, and it sets the size of every bulk and the
   granularity of the ring.
3. **What does RISC OS do with a short `COMPLETE`?** The accumulate-and-
   carry loop suggests any chunking is fine, but the "multiple COMPLETE
   responses per buffer" comment is the only documentation there is.
4. **Which thread does `audio_cb` run on, and is the BQL held?** The peer
   queues messages and rings a doorbell; both must happen under it.
5. **Does anything set a rate before the desktop appears?** If so, S1
   changes a boot path and not merely a runtime one, and wants the same
   before-and-after boot timing the rest of the fork gets.

## Sources

Read for this design, all of them primary:

- `RiscOS/Sources/HWSupport/Sound/BCMSound`, `s/Module` — the client, and
  the authority on what the ROM actually sends and waits for
- `RiscOS/Sources/HWSupport/VCHIQ`, `vc04_services/interface/vchiq_arm/` —
  the guest's own VCHIQ, for the wire vocabulary and the pagelist
- Linux `drivers/staging/vc04_services/bcm2835-audio/` — the second
  implementation of the same protocol, useful for telling what is required
  from what BCMSound merely happens to do
- `riscos-pi4/DESIGN.md` §11 — why the services are refused today
