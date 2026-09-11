/*
 * BCM2835 VideoCore mailbox channel 3 - VCHIQ.
 *
 * Just enough of the VideoCore's half of VCHIQ to let a guest finish
 * connecting and then get on with its life. This is not a VideoCore: it
 * completes the initialisation handshake, answers the guest's CONNECT, and
 * refuses every service the guest then tries to open.
 *
 * That refusal is the point. RISC OS 5 runs BCMSound before its video driver,
 * and BCMSound's module init calls VCHIQ_Connect unconditionally and waits on
 * a semaphore with no timeout, no deadline and no register to poll -- so a
 * guest whose channel-3 message goes unanswered stops there forever, and the
 * video driver never even starts. Answering the connect and declining the
 * services gets the boot moving again, leaves the display to the property
 * channel, and costs a fraction of what emulating the real interface would.
 *
 * The protocol is not documented by Broadcom; the Linux driver under
 * drivers/staging/vc04_services is the de facto specification, and the
 * structure offsets here were additionally read out of a live guest.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/display/bcm2835_fb.h"
#include "hw/misc/bcm2835_mbox_defs.h"
#include "hw/misc/bcm2835_property.h"
#include "hw/misc/bcm2835_vchiq.h"
#include "hw/core/qdev-properties.h"
#include "system/dma.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "trace.h"

static uint32_t vchiq_ld(BCM2835VchiqState *s, uint32_t addr)
{
    return ldl_le_phys(&s->dma_as, addr);
}

static uint16_t vchiq_ld16(BCM2835VchiqState *s, uint32_t addr)
{
    return lduw_le_phys(&s->dma_as, addr);
}

static void vchiq_st(BCM2835VchiqState *s, uint32_t addr, uint32_t val)
{
    stl_le_phys(&s->dma_as, addr, val);
}

/* Fire one of the guest's own remote events and ring the VC->ARM doorbell */
static void vchiq_signal_guest(BCM2835VchiqState *s, uint32_t event)
{
    vchiq_st(s, event + VCHIQ_EV_FIRED, 1);
    s->bell0 |= VCHIQ_BELL_RUNG;
    trace_bcm2835_vchiq_bell(s->bell0);
    qemu_set_irq(s->bell_irq, 1);
}

/*
 * The slot our next message goes in. tx_pos is a monotonic byte position
 * across the whole stream, exactly as the guest treats its own, and the
 * slot for it comes from the queue we published at connect time and the
 * guest appends to as it finishes with our slots.
 */
static bool vchiq_tx_slot(BCM2835VchiqState *s, uint32_t *slot)
{
    uint32_t index = s->tx_pos / s->slot_size;
    uint32_t published, entry;

    published = vchiq_ld(s, s->master_base + VCHIQ_SS_SLOT_QUEUE_RECYCLE);
    if (index >= published) {
        /* The guest has not given this one back yet. It only happens if it
         * has stopped reading us, which is a fault on its side, not ours. */
        qemu_log_mask(LOG_GUEST_ERROR, "%s: slot %u not yet recycled "
                      "(%u published)\n", __func__, index, published);
        return false;
    }
    entry = vchiq_ld(s, s->master_base + VCHIQ_SS_SLOT_QUEUE
                        + (index % s->per_side) * 4);
    if (entry >= s->max_slots) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: slot queue entry %u out of "
                      "range\n", __func__, entry);
        return false;
    }
    *slot = entry;
    return true;
}

/*
 * Hand a slot of the guest's back to it, now that we have read every
 * message in it. The guest does exactly this for our slots -- appends the
 * index to the owner's queue, bumps its recycle counter and fires its
 * recycle event -- and it blocks in VCHIQ_MsgQueue once it has run out,
 * so this is not housekeeping: without it the guest stops talking after
 * as many messages as its slots hold, silently and with nothing to see.
 */
static void vchiq_recycle_slot(BCM2835VchiqState *s, uint32_t slot)
{
    uint32_t n = vchiq_ld(s, s->slave_base + VCHIQ_SS_SLOT_QUEUE_RECYCLE);

    vchiq_st(s, s->slave_base + VCHIQ_SS_SLOT_QUEUE + (n % s->per_side) * 4,
             slot);
    vchiq_st(s, s->slave_base + VCHIQ_SS_SLOT_QUEUE_RECYCLE, n + 1);
    trace_bcm2835_vchiq_recycle(slot, n + 1);
    vchiq_signal_guest(s, s->slave_base + VCHIQ_SS_RECYCLE);
}

/*
 * Append a message, with an optional payload of whole words, and return
 * true if it went. A message never straddles a slot: the rest of the slot
 * is filled with a padding message and the next one started, which is what
 * the guest's own sender does and what its parser expects.
 */
static bool vchiq_queue_msg_data(BCM2835VchiqState *s, uint32_t msgid,
                                 const uint32_t *payload, uint32_t size)
{
    uint32_t stride = QEMU_ALIGN_UP(size + VCHIQ_MSG_HDR_SIZE, 8);
    uint32_t space = s->slot_size - (s->tx_pos % s->slot_size);
    uint32_t slot, hdr, i;

    if (stride > s->slot_size) {
        qemu_log_mask(LOG_UNIMP, "%s: message of %u bytes exceeds a slot\n",
                      __func__, size);
        return false;
    }
    if (space < stride) {
        if (!vchiq_tx_slot(s, &slot)) {
            return false;
        }
        hdr = s->slot0 + slot * s->slot_size + (s->tx_pos % s->slot_size);
        vchiq_st(s, hdr, VCHIQ_MAKE_MSG(VCHIQ_MSG_PADDING, 0, 0));
        vchiq_st(s, hdr + 4, space - VCHIQ_MSG_HDR_SIZE);
        s->tx_pos += space;
    }
    if (!vchiq_tx_slot(s, &slot)) {
        return false;
    }

    hdr = s->slot0 + slot * s->slot_size + (s->tx_pos % s->slot_size);
    vchiq_st(s, hdr, msgid);
    vchiq_st(s, hdr + 4, size);
    for (i = 0; i < size; i += 4) {
        vchiq_st(s, hdr + VCHIQ_MSG_HDR_SIZE + i, payload[i / 4]);
    }
    s->tx_slot = slot;
    s->tx_pos += stride;
    vchiq_st(s, s->master_base + VCHIQ_SS_TX_POS, s->tx_pos);

    trace_bcm2835_vchiq_tx(msgid, VCHIQ_MSG_TYPE(msgid), size);
    return true;
}

static bool vchiq_queue_msg(BCM2835VchiqState *s, uint32_t msgid, uint32_t size)
{
    return vchiq_queue_msg_data(s, msgid, NULL, size);
}

/* ------------------------------------------------------------------ */
/*
 * The AUDS audio service.
 *
 * RISC OS reaches the speaker through this and nothing else: BCMSound is
 * a "VCHIQ audio service controller" in its own words, so there is no
 * audio hardware to model -- riscos-pi4/SOUND.md is the research.
 *
 * Two things make it work. The first is that three of the messages are
 * sent from an unbounded spin on a byte -- BCMSound's SendWithResult,
 * with interrupts forced on, no timeout and no deadline -- so CLOSE,
 * CONFIG and CONTROL must be answered with a RESULT or the guest never
 * comes back. That is why the service was refused until now.
 *
 * The second is that COMPLETE is the clock. BCMSound counts the bytes we
 * report in it and calls SoundDMA once per buffer's worth, so the rate we
 * send COMPLETE is the rate RISC OS generates sound at. Here that is
 * paced on the virtual clock from the format CONFIG asked for; sprint 3
 * moves it onto the audio backend, whose own consumption is the host's
 * real sound card and cannot drift against it.
 *
 * This sprint plays nothing. It takes the bulk transfers and reports them
 * complete without reading a byte, which is enough to prove the guest's
 * whole sound path comes up and keeps turning.
 */

/*
 * Gather the bytes a bulk transfer describes.
 *
 * The wire carries the bus address of a pagelist, not of the buffer: the
 * guest's pages are scattered, and the list names their runs. Each entry
 * is a page-aligned address with the count of further consecutive pages
 * packed into the low twelve bits -- the same trick, and the same
 * arithmetic, as create_pagelist on the other side. Everything here
 * indexes guest memory with numbers the guest chose, so each one is
 * bounded before it is used.
 *
 * Returns the number of bytes gathered into s->bulk_buf, 0 on nonsense.
 */
static uint32_t vchiq_bulk_gather(BCM2835VchiqState *s, uint32_t pagelist)
{
    uint32_t length, offset, type, got = 0, entry_index = 0;
    uint64_t first = 0;

    if (!pagelist) {
        return 0;
    }
    length = vchiq_ld(s, pagelist + VCHIQ_PAGELIST_LENGTH);
    type = vchiq_ld16(s, pagelist + VCHIQ_PAGELIST_TYPE);
    offset = vchiq_ld16(s, pagelist + VCHIQ_PAGELIST_OFFSET);

    if (!length || length > VCHIQ_BULK_MAX || offset >= VCHIQ_PAGE_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: pagelist at 0x%08x claims "
                      "%u bytes at offset %u\n", __func__, pagelist,
                      length, offset);
        return 0;
    }
    if (type != VCHIQ_PAGELIST_WRITE) {
        /* A transmit from the guest is always a WRITE list; anything else
         * is a direction we do not implement rather than a broken list. */
        qemu_log_mask(LOG_UNIMP, "%s: pagelist type %u\n", __func__, type);
        return 0;
    }

    if (s->bulk_buf_size < length) {
        s->bulk_buf = g_realloc(s->bulk_buf, length);
        s->bulk_buf_size = length;
    }

    while (got < length) {
        uint32_t entry, pages;
        uint64_t base, avail, n;

        /* One entry per run, and a run is at least one page: more entries
         * than pages means the list is not one. */
        if (entry_index > length / VCHIQ_PAGE_SIZE + 1) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: pagelist at 0x%08x does not "
                          "cover its %u bytes\n", __func__, pagelist, length);
            return 0;
        }
        entry = vchiq_ld(s, pagelist + VCHIQ_PAGELIST_ADDRS + entry_index * 4);
        base = (uint64_t)(entry & ~VCHIQ_PL36_COUNT_MASK)
               << VCHIQ_PL36_ADDR_SHIFT;
        pages = (entry & VCHIQ_PL36_COUNT_MASK) + 1;

        avail = (uint64_t)pages * VCHIQ_PAGE_SIZE - offset;
        n = MIN(avail, length - got);
        if (dma_memory_read(&s->dma_as, base + offset, s->bulk_buf + got, n,
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: unreadable page at 0x%"
                          PRIx64 "\n", __func__, base + offset);
            return 0;
        }
        if (!entry_index) {
            first = base + offset;
        }
        got += n;
        offset = 0;             /* only the first run starts part-way in */
        entry_index++;
    }
    trace_bcm2835_vchiq_bulk(pagelist, first, got, entry_index);
    return got;
}

/*
 * The capture file. There is nothing to listen to until sprint 3, so the
 * way to know whether the samples are real is to look at them: this is a
 * plain WAV, playable while it is still being written because the sizes
 * in the header are kept up to date as it grows.
 */
static void vchiq_wav_write(BCM2835VchiqState *s, const uint8_t *buf,
                            uint32_t len)
{
    uint8_t hdr[44] = { 0 };
    uint32_t rate = s->auds_rate;
    uint32_t channels = s->auds_channels;
    uint32_t bits = s->auds_bps;
    uint32_t byte_rate, block_align;

    if (!s->wav_path || !len || !rate || !channels || !bits) {
        return;
    }
    if (!s->wav) {
        s->wav = fopen(s->wav_path, "wb");
        if (!s->wav) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: cannot write %s\n",
                          __func__, s->wav_path);
            g_free(s->wav_path);
            s->wav_path = NULL;
            return;
        }
        s->wav_bytes = 0;
        fseek(s->wav, sizeof(hdr), SEEK_SET);
    }
    if (fwrite(buf, 1, len, s->wav) != len) {
        return;
    }
    s->wav_bytes += len;

    byte_rate = rate * channels * (bits / 8);
    block_align = channels * (bits / 8);
    memcpy(hdr, "RIFF", 4);
    stl_le_p(hdr + 4, 36 + s->wav_bytes);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    stl_le_p(hdr + 16, 16);             /* PCM chunk size */
    stw_le_p(hdr + 20, 1);              /* PCM */
    stw_le_p(hdr + 22, channels);
    stl_le_p(hdr + 24, rate);
    stl_le_p(hdr + 28, byte_rate);
    stw_le_p(hdr + 32, block_align);
    stw_le_p(hdr + 34, bits);
    memcpy(hdr + 36, "data", 4);
    stl_le_p(hdr + 40, s->wav_bytes);

    /* Rewrite the header every time, so the file is always playable */
    fseek(s->wav, 0, SEEK_SET);
    fwrite(hdr, 1, sizeof(hdr), s->wav);
    fseek(s->wav, 0, SEEK_END);
    fflush(s->wav);
}

/* The loudest sample in a buffer, which is how silence is told from a
 * tune without anything to listen with. 16-bit signed is all RISC OS
 * ever asks for; anything else reports nothing rather than guessing. */
static uint32_t vchiq_pcm_peak(const BCM2835VchiqState *s,
                               const uint8_t *buf, uint32_t len)
{
    uint32_t peak = 0, i;

    if (s->auds_bps != 16) {
        return 0;
    }
    for (i = 0; i + 1 < len; i += 2) {
        int32_t v = (int16_t)lduw_le_p(buf + i);

        if (v < 0) {
            v = -v;
        }
        if ((uint32_t)v > peak) {
            peak = v;
        }
    }
    return peak;
}

static void auds_reply(BCM2835VchiqState *s, const uint32_t *msg)
{
    vchiq_queue_msg_data(s, VCHIQ_MAKE_MSG(VCHIQ_MSG_DATA,
                                           VCHIQ_AUDS_VC_PORT, s->auds_port),
                         msg, VC_AUDIO_MSG_SIZE);
}

static void auds_result(BCM2835VchiqState *s, int32_t success)
{
    uint32_t msg[VC_AUDIO_MSG_WORDS] = { 0 };

    msg[0] = VC_AUDIO_MSG_TYPE_RESULT;
    msg[1] = (uint32_t)success;
    auds_reply(s, msg);
}

static void auds_complete(BCM2835VchiqState *s, uint32_t bytes)
{
    uint32_t msg[VC_AUDIO_MSG_WORDS] = { 0 };

    /* count, then the two cookies from the WRITE this answers. BCMSound
     * sends zeroes and ignores them coming back; Linux's driver checks
     * them, so echo what arrived rather than invent anything. */
    msg[0] = VC_AUDIO_MSG_TYPE_COMPLETE;
    msg[1] = bytes;
    msg[2] = s->auds_cookie1;
    msg[3] = s->auds_cookie2;
    trace_bcm2835_vchiq_auds_complete(bytes);
    auds_reply(s, msg);
}

/*
 * The sound card's pull. Whatever it takes out of the ring is what the
 * guest is told has played, so the reports come out at the rate the host
 * actually consumes audio -- no timer to tune and nothing to drift
 * against. Called from the audio timer on the main loop, BQL held, which
 * is what makes it safe to queue a message and ring the doorbell here.
 */
static void auds_audio_cb(void *opaque, int avail)
{
    BCM2835VchiqState *s = opaque;
    uint32_t reported = 0;

    if (!s->auds_open || !s->voice) {
        return;
    }
    while (avail > 0 && s->ring_used) {
        uint32_t run = MIN((uint32_t)avail, s->ring_used);
        size_t took;

        /* One contiguous piece at a time: the ring wraps, the API does not */
        run = MIN(run, s->ring_size - s->ring_tail);
        took = audio_be_write(s->audio_be, s->voice, s->ring + s->ring_tail,
                              run);
        if (!took) {
            break;
        }
        s->ring_tail = (s->ring_tail + took) % s->ring_size;
        s->ring_used -= took;
        reported += took;
        avail -= took;
    }
    if (reported) {
        trace_bcm2835_vchiq_auds_played(reported, s->ring_used);
        auds_complete(s, reported);
        vchiq_signal_guest(s, s->slave_base + VCHIQ_SS_TRIGGER);
    }
}

/* Open, or re-open at a new format. Silent failure leaves s->voice NULL
 * and the virtual-clock fallback in charge. */
static void auds_open_voice(BCM2835VchiqState *s)
{
    struct audsettings as;

    if (!s->audio_be || s->auds_bps != 16 || !s->auds_rate ||
        !s->auds_channels) {
        return;
    }
    as.freq = s->auds_rate;
    as.nchannels = s->auds_channels;
    as.fmt = AUDIO_FORMAT_S16;
    as.big_endian = false;

    s->voice = audio_be_open_out(s->audio_be, s->voice, TYPE_BCM2835_VCHIQ,
                                 s, auds_audio_cb, &as);
    if (!s->voice) {
        qemu_log_mask(LOG_UNIMP, "%s: no voice at %u Hz, %u channels\n",
                      __func__, s->auds_rate, s->auds_channels);
        return;
    }
    if (!s->ring) {
        s->ring_size = VCHIQ_AUDS_RING_BYTES;
        s->ring = g_malloc(s->ring_size);
    }
    s->ring_head = s->ring_tail = s->ring_used = 0;
    trace_bcm2835_vchiq_auds_voice(s->auds_rate, s->auds_channels);
}

/* Put a buffer's samples where the sound card will find them. */
static void auds_ring_push(BCM2835VchiqState *s, const uint8_t *buf,
                           uint32_t len)
{
    uint32_t space = s->ring_size - s->ring_used;

    if (len > space) {
        /*
         * Only reachable if the host stopped consuming: the guest sends
         * more only when we report, so it cannot outrun us by itself.
         * Drop the oldest rather than the newest, so what plays next is
         * what the guest sent most recently.
         */
        uint32_t drop = len - space;

        qemu_log_mask(LOG_GUEST_ERROR, "%s: audio ring full, dropping %u "
                      "bytes\n", __func__, drop);
        s->ring_tail = (s->ring_tail + drop) % s->ring_size;
        s->ring_used -= drop;
    }
    while (len) {
        uint32_t run = MIN(len, s->ring_size - s->ring_head);

        memcpy(s->ring + s->ring_head, buf, run);
        s->ring_head = (s->ring_head + run) % s->ring_size;
        s->ring_used += run;
        buf += run;
        len -= run;
    }
}

/* Bytes a second of the format the guest last configured, 0 if none */
static uint32_t auds_byte_rate(const BCM2835VchiqState *s)
{
    return s->auds_channels * (s->auds_bps / 8) * s->auds_rate;
}

/*
 * Report the audio that has played since we last looked.
 *
 * The earlier shape of this booked a deadline per buffer, which could be
 * overrun by a guest that ran ahead and then never caught up -- measured
 * at 146 reports a second early and 44 late, against the 86 the format
 * asks for. This holds one number instead: the virtual time at which
 * everything taken so far will have finished playing. What has played is
 * whatever that leaves behind, so the pacing cannot run fast, and a slow
 * tick is made up on the next one rather than lost.
 */
static void auds_timer_fire(void *opaque)
{
    BCM2835VchiqState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t rate = auds_byte_rate(s);
    uint64_t played;

    if (!rate || !s->auds_outstanding) {
        return;
    }
    if (now >= s->auds_played_ns) {
        played = s->auds_outstanding;           /* all of it, and then some */
    } else {
        uint64_t left = (uint64_t)(s->auds_played_ns - now) * rate
                        / NANOSECONDS_PER_SECOND;

        played = left < s->auds_outstanding ? s->auds_outstanding - left : 0;
    }

    if (played) {
        s->auds_outstanding -= played;
        auds_complete(s, played);
        vchiq_signal_guest(s, s->slave_base + VCHIQ_SS_TRIGGER);
    }
    if (s->auds_outstanding) {
        hrtimer_mod_ns(s->auds_timer, now + VCHIQ_AUDS_TICK_NS);
    }
}

/* Take a buffer's worth of audio and put it on the clock */
static void auds_queue_playback(BCM2835VchiqState *s, uint32_t bytes)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t rate = auds_byte_rate(s);

    if (s->voice && bytes) {
        /* The sound card is the clock; auds_audio_cb does the reporting */
        auds_ring_push(s, s->bulk_buf, bytes);
        return;
    }
    if (!rate || !bytes) {
        /* No format yet: answer at once rather than lose the buffer */
        auds_complete(s, bytes);
        vchiq_signal_guest(s, s->slave_base + VCHIQ_SS_TRIGGER);
        return;
    }

    /* Where the audio already taken runs out, or now if it has */
    if (s->auds_played_ns < now) {
        s->auds_played_ns = now;
    }
    s->auds_played_ns += (int64_t)bytes * NANOSECONDS_PER_SECOND / rate;
    s->auds_outstanding += bytes;
    hrtimer_mod_ns(s->auds_timer, now + VCHIQ_AUDS_TICK_NS);
}

static void auds_close(BCM2835VchiqState *s)
{
    s->auds_open = false;
    s->auds_running = false;
    s->auds_outstanding = 0;
    s->auds_played_ns = 0;
    s->ring_head = s->ring_tail = s->ring_used = 0;
    if (s->voice) {
        audio_be_set_active_out(s->audio_be, s->voice, false);
    }
    if (s->auds_timer) {
        hrtimer_del(s->auds_timer);
    }
}

/* One audio message out of a DATA carrying it. Returns replies queued. */
static unsigned auds_handle_msg(BCM2835VchiqState *s, uint32_t hdr,
                                uint32_t size)
{
    uint32_t body = hdr + VCHIQ_MSG_HDR_SIZE;
    uint32_t type;

    if (size < 4) {
        return 0;
    }
    type = vchiq_ld(s, body);
    trace_bcm2835_vchiq_auds_msg(type, size);

    switch (type) {
    case VC_AUDIO_MSG_TYPE_CONFIG:
        if (size >= 16) {
            s->auds_channels = vchiq_ld(s, body + 4);
            s->auds_rate = vchiq_ld(s, body + 8);
            s->auds_bps = vchiq_ld(s, body + 12);
            trace_bcm2835_vchiq_auds_config(s->auds_channels, s->auds_rate,
                                            s->auds_bps);
            auds_open_voice(s);
        }
        auds_result(s, 0);
        return 1;

    case VC_AUDIO_MSG_TYPE_CONTROL:
        /* Volume and destination. Acknowledged, not yet applied. */
        auds_result(s, 0);
        return 1;

    case VC_AUDIO_MSG_TYPE_CLOSE:
        /* The audio session, not the VCHIQ service: BCMSound closes and
         * reopens it around every rate change. */
        s->auds_running = false;
        s->auds_outstanding = 0;
        s->auds_played_ns = 0;
        if (s->voice) {
            audio_be_set_active_out(s->audio_be, s->voice, false);
        }
        auds_result(s, 0);
        return 1;

    case VC_AUDIO_MSG_TYPE_OPEN:
        return 0;               /* sent without waiting for a result */

    case VC_AUDIO_MSG_TYPE_START:
        s->auds_running = true;
        s->auds_played_ns = 0;
        if (s->voice) {
            audio_be_set_active_out(s->audio_be, s->voice, true);
        }
        return 0;

    case VC_AUDIO_MSG_TYPE_STOP:
        s->auds_running = false;
        if (s->voice) {
            audio_be_set_active_out(s->audio_be, s->voice, false);
        }
        return 0;

    case VC_AUDIO_MSG_TYPE_WRITE:
        /* The header only: the samples follow as a bulk transfer. Keep
         * the cookies to echo in the COMPLETE that answers it. */
        if (size >= 16) {
            s->auds_cookie1 = vchiq_ld(s, body + 8);
            s->auds_cookie2 = vchiq_ld(s, body + 12);
        }
        return 0;

    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled audio message type %u\n",
                      __func__, type);
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/*
 * The dispmanx display service -- the pointer, said yes to.
 *
 * The ROM's hardware pointer is a dispmanx element over this service
 * (BCMVideo s/HWPointer): it converts the kernel's 2 bpp shape into a
 * 32x32 ARGB resource, bulk-writes the image, and adds/moves/removes an
 * element in start-change-submit transactions.  Answering the subset
 * the pointer uses gives the guest a real hardware pointer with no
 * guest-side code of ours: the ROM does the conversion (including its
 * anti-fringe fill of transparent pixels) and the host composites --
 * riscos-pi4/GPUDESIGN.md section 2.
 *
 * Every command outside the subset is refused with -1, so the overlay
 * machinery in BCMVideo s/GVOverlay stays inert while the service is
 * open -- the refusal discipline of DESIGN.md section 11, scoped to
 * the call instead of the service.
 */

static void disp_reply(BCM2835VchiqState *s, const uint32_t *msg,
                       uint32_t words)
{
    vchiq_queue_msg_data(s,
                         VCHIQ_MAKE_MSG(VCHIQ_MSG_DATA,
                                        VCHIQ_DISP_VC_PORT, s->disp_port),
                         msg, words * 4);
}

static uint32_t disp_mint_handle(BCM2835VchiqState *s)
{
    if (++s->disp_next_handle == 0) {
        s->disp_next_handle = 1;
    }
    return s->disp_next_handle;
}

/*
 * DisplayGetInfo answers the physical display's size, from which the
 * ROM derives the desktop-to-display scale and offset
 * (Dispmanx_CalcDisplayScaleOffset).  Answering the mode's own size
 * makes that arithmetic the identity: the sprite's destination
 * rectangle is then guest pixels outright, which is what the compositor
 * assumes.
 */
static void disp_display_size(uint32_t *w, uint32_t *h)
{
    Object *obj = object_resolve_path_type("", TYPE_BCM2835_PROPERTY, NULL);
    BCM2835FBConfig cfg;

    /*
     * The physical display, which is the EDID's preferred timing -- not
     * whatever the framebuffer happens to be when the question is asked.
     *
     * The guest asks this once, early, and scales its pointer into the
     * answer for the rest of the session. Answering with the current
     * framebuffer meant answering 640x480, because that is the mode the
     * boot passes through before it settles: at 800x600 the resulting
     * 1.25x error was small enough to look like nothing, and at 1920x1200
     * it made the pointer a third of its proper size and moved it in
     * three-pixel steps. The display is what does not change.
     */
    *w = 800;
    *h = 600;
    if (obj) {
        bcm2835_property_preferred_mode(BCM2835_PROPERTY(obj), w, h);
        return;
    }

    /* No property device: fall back to the framebuffer, as before */
    obj = object_resolve_path_type("", TYPE_BCM2835_FB, NULL);
    if (obj) {
        bcm2835_fb_get_config(BCM2835_FB(obj), &cfg);
        if (cfg.xres && cfg.yres) {
            *w = cfg.xres;
            *h = cfg.yres;
        }
    }
}

/*
 * UpdateSubmit.  Dispmanx applies an update at vsync, so the
 * transaction becomes visible here, all of it or none: the image is
 * never read half-written and the compositor never sees half a move.
 */
static void disp_commit(BCM2835VchiqState *s)
{
    if (s->disp_stage_len) {
        memcpy(s->ptr_image, s->disp_stage, s->disp_stage_len);
        s->disp_stage_len = 0;
    }
    if (s->disp_tx_pending) {
        s->ptr_visible = s->disp_tx_visible;
        s->ptr_x = s->disp_tx_x;
        s->ptr_y = s->disp_tx_y;
        s->ptr_w = s->disp_tx_w;
        s->ptr_h = s->disp_tx_h;
        /* the sprite's own resolution: the resource's, never the dest
         * rect's -- the plane is mode-independent and the rect may be
         * scaled by the ROM's display arithmetic */
        s->ptr_img_w = s->disp_res_w;
        s->ptr_img_h = s->disp_res_h;
        s->disp_tx_pending = false;
        trace_bcm2835_vchiq_disp_pointer(s->ptr_visible, s->ptr_x, s->ptr_y,
                                         s->ptr_w, s->ptr_h);
    }
    s->ptr_gen++;
}

static void disp_reset(BCM2835VchiqState *s)
{
    s->disp_resource = 0;
    s->disp_res_w = 0;
    s->disp_res_h = 0;
    s->disp_element = 0;
    s->disp_bulk_len = 0;
    s->disp_stage_len = 0;
    s->disp_tx_pending = false;
    s->ptr_visible = false;
    s->ptr_w = 0;
    s->ptr_h = 0;
    s->ptr_img_w = 0;
    s->ptr_img_h = 0;
    s->ptr_gen++;               /* the compositor drops what it had */
}

/*
 * One dispmanx message out of a DATA carrying it.  The reply protocol
 * is Dispmanx_Send's (BCMVideo/s/Dispmanx:237-309): a reply is wanted
 * for any command without the NoReply bit -- four bytes, twenty for
 * DisplayGetInfo -- and the client dequeues FIFO, so replies go back
 * strictly in ask order.
 *
 * Message layouts are read out of the ROM's senders: ElementAdd is
 * HWP_RTRoutine's 108-byte block with the destination rectangle at
 * +16 and the resource at +32; ElementChangeAttributes is its 64-byte
 * block with the rectangle at +32.
 */
static unsigned disp_handle_msg(BCM2835VchiqState *s, uint32_t hdr,
                                uint32_t size)
{
    uint32_t body = hdr + VCHIQ_MSG_HDR_SIZE;
    uint32_t word[5] = { 0 };
    uint32_t cmd, op;

    if (size < 4) {
        return 0;
    }
    cmd = vchiq_ld(s, body);
    op = cmd & ~EDISPMAN_NO_REPLY;
    trace_bcm2835_vchiq_disp_msg(op, cmd & EDISPMAN_NO_REPLY);

    switch (op) {
    case EDISPMAN_DISPLAY_OPEN:
        word[0] = disp_mint_handle(s);
        disp_reply(s, word, 1);
        return 1;

    case EDISPMAN_DISPLAY_GET_INFO: {
        uint32_t w, h;

        disp_display_size(&w, &h);
        s->disp_info_w = w;
        s->disp_info_h = h;
        trace_bcm2835_vchiq_disp_getinfo(w, h);
        word[0] = 0;               /* result */
        word[1] = w;
        word[2] = h;
        word[3] = 0;               /* transform */
        word[4] = 1;               /* format: RGB565 */
        disp_reply(s, word, 5);
        return 1;
    }

    case EDISPMAN_RESOURCE_CREATE:
        /* {type, width, height}: the pointer's 32x32 ARGB */
        if (size >= 16) {
            uint32_t w = vchiq_ld(s, body + 8);
            uint32_t h = vchiq_ld(s, body + 12);

            if (w && h && w <= VCHIQ_DISP_SPRITE_MAX
                && h <= VCHIQ_DISP_SPRITE_MAX) {
                s->disp_resource = disp_mint_handle(s);
                s->disp_res_w = w;
                s->disp_res_h = h;
                word[0] = s->disp_resource;
                disp_reply(s, word, 1);
                return 1;
            }
        }
        word[0] = (uint32_t)-1;
        disp_reply(s, word, 1);
        return 1;

    case EDISPMAN_BULK_WRITE:
        /* {resource, offset, length}; the bytes follow as a bulk */
        s->disp_bulk_len = 0;
        if (size >= 16 && vchiq_ld(s, body + 4) == s->disp_resource) {
            uint32_t len = vchiq_ld(s, body + 12);

            if (len && len <= VCHIQ_DISP_IMAGE_BYTES) {
                s->disp_bulk_len = len;
            }
        }
        return 0;                   /* NoReply */

    case EDISPMAN_UPDATE_START:
        s->disp_tx_pending = false; /* a fresh transaction */
        word[0] = disp_mint_handle(s);
        disp_reply(s, word, 1);
        return 1;

    case EDISPMAN_ELEMENT_ADD: {
        uint32_t layer = size >= 16 ? vchiq_ld(s, body + 12) : 0;

        if (size >= 36 && layer == EDISPMAN_LAYER_POINTER
            && vchiq_ld(s, body + 32) == s->disp_resource) {
            s->disp_tx_pending = true;
            s->disp_tx_visible = true;
            s->disp_tx_x = (int32_t)vchiq_ld(s, body + 16);
            s->disp_tx_y = (int32_t)vchiq_ld(s, body + 20);
            s->disp_tx_w = (int32_t)vchiq_ld(s, body + 24);
            s->disp_tx_h = (int32_t)vchiq_ld(s, body + 28);
            s->disp_element = disp_mint_handle(s);
            word[0] = s->disp_element;
        } else {
            word[0] = (uint32_t)-1; /* not a pointer element: refused */
        }
        disp_reply(s, word, 1);
        return 1;
    }

    case EDISPMAN_ELEMENT_CHANGE_ATTRIBUTES:
        if (size >= 48 && vchiq_ld(s, body + 8) == s->disp_element) {
            s->disp_tx_pending = true;
            s->disp_tx_x = (int32_t)vchiq_ld(s, body + 32);
            s->disp_tx_y = (int32_t)vchiq_ld(s, body + 36);
            s->disp_tx_w = (int32_t)vchiq_ld(s, body + 40);
            s->disp_tx_h = (int32_t)vchiq_ld(s, body + 44);
        }
        return 0;                   /* NoReply */

    case EDISPMAN_ELEMENT_REMOVE:
        if (size >= 12 && vchiq_ld(s, body + 8) == s->disp_element) {
            s->disp_tx_pending = true;
            s->disp_tx_visible = false;
        }
        return 0;                   /* NoReply */

    case EDISPMAN_UPDATE_SUBMIT:
        disp_commit(s);
        word[0] = 0;
        disp_reply(s, word, 1);
        return 1;

    case EDISPMAN_RESOURCE_DELETE:
        if (size >= 8 && vchiq_ld(s, body + 4) == s->disp_resource) {
            s->disp_resource = 0;
        }
        word[0] = 0;
        disp_reply(s, word, 1);
        return 1;

    default:
        /* DisplayClose and every command the pointer does not use */
        if (!(cmd & EDISPMAN_NO_REPLY)) {
            word[0] = (uint32_t)-1;
            disp_reply(s, word, 1);
            return 1;
        }
        return 0;
    }
}

/* The committed pointer sprite, for the compositor (ui/metal).  UI
 * thread, no BQL: the generation is a seqlock, so a read that raced a
 * commit is reported stale and the caller keeps the previous frame's
 * sprite rather than a mixed one. */
bool bcm2835_vchiq_get_cursor(VchiqCursor *out)
{
    /* The device is resolved once and kept: this runs every frame from
     * the UI thread, and a per-frame walk of the object tree is both a
     * millisecond the compositor does not have and a read of a tree
     * other threads own (a sampled stuck frame showed the walk).  The
     * machine is built before the first frame, so the cache can only
     * ever fill with the one and only instance. */
    static BCM2835VchiqState *cached;
    BCM2835VchiqState *s;
    uint32_t gen;

    if (!cached) {
        Object *obj = object_resolve_path_type("", TYPE_BCM2835_VCHIQ, NULL);

        if (!obj) {
            return false;
        }
        cached = BCM2835_VCHIQ(obj);
    }
    s = cached;

    gen = qatomic_read(&s->ptr_gen);
    smp_rmb();
    out->generation = gen;
    out->visible = s->ptr_visible;
    out->x = s->ptr_x;
    out->y = s->ptr_y;
    out->w = s->ptr_w;
    out->h = s->ptr_h;
    out->img_w = s->ptr_img_w;
    out->img_h = s->ptr_img_h;
    out->disp_w = s->disp_info_w;
    out->disp_h = s->disp_info_h;
    out->argb = s->ptr_image;
    smp_rmb();
    out->stale = qatomic_read(&s->ptr_gen) != gen;
    return true;
}

/*
 * The guest has handed us slot zero. Set up our half of it, answer its
 * CONNECT, and wake it.
 */
static void vchiq_init_from_slot_zero(BCM2835VchiqState *s, uint32_t value)
{
    uint32_t magic, slot_zero_size, max_slots, per_side, shared_size;
    uint32_t slot_first, slot_last, i;

    /*
     * The low nibble carried the channel; the rest is a VideoCore bus
     * address. Which 1GB alias it uses depends on the SoC (0x40000000 on a
     * BCM2835, 0xC0000000 afterwards) and dma_as covers them all, so do not
     * second-guess it: the magic word is the real test of whether this is
     * pointing at a slot zero.
     */
    s->slot0 = value & ~0xfu;
    if (!s->slot0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: null slot zero address\n",
                      __func__);
        return;
    }

    magic = vchiq_ld(s, s->slot0 + VCHIQ_SZ_MAGIC);
    slot_zero_size = vchiq_ld(s, s->slot0 + VCHIQ_SZ_SLOT_ZERO_SIZE);
    s->slot_size = vchiq_ld(s, s->slot0 + VCHIQ_SZ_SLOT_SIZE);
    max_slots = vchiq_ld(s, s->slot0 + VCHIQ_SZ_MAX_SLOTS);
    per_side = vchiq_ld(s, s->slot0 + VCHIQ_SZ_MAX_SLOTS_PER_SIDE);

    trace_bcm2835_vchiq_slot_zero(s->slot0, magic, s->slot_size, max_slots);

    /*
     * Everything from here on indexes guest memory using numbers the guest
     * gave us, so each one is bounded before it is used. A slot is one page
     * in every implementation, but allow a range rather than insist.
     */
    if (magic != VCHIQ_SLOT_MAGIC ||
        s->slot_size < 2 * VCHIQ_MSG_HDR_SIZE || s->slot_size > 0x10000 ||
        !max_slots || max_slots > 4096 ||
        !per_side || per_side > max_slots) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: slot zero at 0x%08x does not look like VCHIQ "
                      "(magic 0x%08x, slot_size %u, max_slots %u, per_side %u)\n",
                      __func__, s->slot0, magic, s->slot_size, max_slots,
                      per_side);
        s->slot0 = 0;
        return;
    }
    s->max_slots = max_slots;
    s->per_side = per_side;

    /*
     * The debug array is last in each shared state and its size is a build
     * option, so derive the stride from slot_zero_size rather than assume it.
     * It must at least hold the fixed fields and the slot queue.
     */
    if (slot_zero_size < VCHIQ_SZ_MASTER + max_slots * 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: slot_zero_size %u too small\n",
                      __func__, slot_zero_size);
        s->slot0 = 0;
        return;
    }
    shared_size = (slot_zero_size - VCHIQ_SZ_MASTER - max_slots * 4) / 2;
    if (shared_size < VCHIQ_SS_SLOT_QUEUE + per_side * 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: shared state of %u bytes cannot hold a %u-entry "
                      "slot queue\n", __func__, shared_size, per_side);
        s->slot0 = 0;
        return;
    }
    s->master_base = s->slot0 + VCHIQ_SZ_MASTER;
    s->slave_base = s->master_base + shared_size;

    slot_first = vchiq_ld(s, s->master_base + VCHIQ_SS_SLOT_FIRST);
    slot_last = vchiq_ld(s, s->master_base + VCHIQ_SS_SLOT_LAST);
    if (slot_last < slot_first || slot_last >= max_slots ||
        slot_last - slot_first + 1 > per_side) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad slot range %u..%u for a "
                      "%u-entry queue\n", __func__, slot_first, slot_last,
                      per_side);
        s->slot0 = 0;
        return;
    }

    /*
     * The guest zeroes our slot queue and leaves it for us to fill. It indexes
     * straight into it when parsing our messages, so leaving it at zero would
     * have it read slot zero itself as message data.
     */
    for (i = 0; i <= slot_last - slot_first; i++) {
        vchiq_st(s, s->master_base + VCHIQ_SS_SLOT_QUEUE + i * 4,
                 slot_first + i);
    }
    vchiq_st(s, s->master_base + VCHIQ_SS_SLOT_QUEUE_RECYCLE,
             slot_last - slot_first + 1);

    s->tx_slot = slot_first;
    s->tx_pos = 0;
    s->rx_pos = 0;

    /*
     * Arm our trigger before anything else: the guest only rings the ARM->VC
     * doorbell when it sees this armed, so without it we would never hear
     * about the messages it queues.
     */
    vchiq_st(s, s->master_base + VCHIQ_SS_TRIGGER + VCHIQ_EV_ARMED, 1);
    vchiq_st(s, s->master_base + VCHIQ_SS_INITIALISED, 1);

    /*
     * Answer the connect. The guest's wait is on a counting semaphore, so it
     * does not matter whether it is already blocked or has not got there yet.
     */
    if (vchiq_queue_msg(s, VCHIQ_MAKE_MSG(VCHIQ_MSG_CONNECT, 0, 0), 0)) {
        s->connected = true;
        vchiq_signal_guest(s, s->slave_base + VCHIQ_SS_TRIGGER);
    }
}

/* Drain the messages the guest has queued for us since we last looked */
static void vchiq_parse_guest_messages(BCM2835VchiqState *s)
{
    uint32_t tx_pos, replies = 0;
    int guard = 256;

    if (!s->slot0) {
        return;
    }

    /* Consume our trigger but stay armed, so the guest keeps ringing */
    vchiq_st(s, s->master_base + VCHIQ_SS_TRIGGER + VCHIQ_EV_FIRED, 0);
    vchiq_st(s, s->master_base + VCHIQ_SS_TRIGGER + VCHIQ_EV_ARMED, 1);

    tx_pos = vchiq_ld(s, s->slave_base + VCHIQ_SS_TX_POS);

    while (s->rx_pos != tx_pos && guard-- > 0) {
        uint32_t qidx = (s->rx_pos / s->slot_size) % s->per_side;
        uint32_t slot = vchiq_ld(s, s->slave_base + VCHIQ_SS_SLOT_QUEUE
                                    + qidx * 4);
        uint32_t hdr, msgid, size, type;

        /* The queue entry and the header both come from the guest */
        if (slot >= s->max_slots) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: slot queue entry %u out of "
                          "range\n", __func__, slot);
            break;
        }
        hdr = s->slot0 + slot * s->slot_size + (s->rx_pos % s->slot_size);
        msgid = vchiq_ld(s, hdr);
        size = vchiq_ld(s, hdr + 4);
        type = VCHIQ_MSG_TYPE(msgid);
        if (size > s->slot_size - VCHIQ_MSG_HDR_SIZE) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: message of %u bytes cannot "
                          "fit a slot\n", __func__, size);
            break;
        }

        /*
         * Advance, and if that leaves this slot behind, give it back: the
         * guest cannot send us anything more once its slots are all with
         * us.
         */
        {
            uint32_t was = s->rx_pos / s->slot_size;

            s->rx_pos += QEMU_ALIGN_UP(size + VCHIQ_MSG_HDR_SIZE, 8);
            if (s->rx_pos / s->slot_size != was) {
                vchiq_recycle_slot(s, slot);
            }
        }

        switch (type) {
        case VCHIQ_MSG_OPEN:
        {
            /*
             * Payload is { fourcc, client_id, version, version_min }.
             * 'AUDS' is the sound path and we answer it; the rest are
             * closed straight back, which the guest treats as a clean
             * refusal and carries on. Refusing them is deliberate --
             * DESIGN.md section 11 has why.
             */
            uint32_t fourcc = vchiq_ld(s, hdr + VCHIQ_MSG_HDR_SIZE);
            uint32_t srcport = VCHIQ_MSG_SRCPORT(msgid);

            if (fourcc == VCHIQ_FOURCC_AUDS && !s->auds_open) {
                uint32_t ack = VCHIQ_AUDS_VERSION;   /* a short, low half */

                s->auds_open = true;
                s->auds_port = srcport;
                trace_bcm2835_vchiq_auds_open(srcport);
                replies += vchiq_queue_msg_data(s,
                    VCHIQ_MAKE_MSG(VCHIQ_MSG_OPENACK,
                                   VCHIQ_AUDS_VC_PORT, srcport),
                    &ack, 4);
            } else if (fourcc == VCHIQ_FOURCC_DISP && !s->disp_open) {
                uint32_t ack = VCHIQ_DISP_VERSION;

                s->disp_open = true;
                s->disp_port = srcport;
                trace_bcm2835_vchiq_disp_open(srcport);
                replies += vchiq_queue_msg_data(s,
                    VCHIQ_MAKE_MSG(VCHIQ_MSG_OPENACK,
                                   VCHIQ_DISP_VC_PORT, srcport),
                    &ack, 4);
            } else if (fourcc == VCHIQ_FOURCC_UPDH && !s->updh_open) {
                /* The notification service: opened for completeness,
                 * its ROM-side callback is an empty stub, so nothing is
                 * ever sent on it. */
                uint32_t ack = VCHIQ_DISP_VERSION;

                s->updh_open = true;
                s->updh_port = srcport;
                replies += vchiq_queue_msg_data(s,
                    VCHIQ_MAKE_MSG(VCHIQ_MSG_OPENACK,
                                   VCHIQ_UPDH_VC_PORT, srcport),
                    &ack, 4);
            } else {
                trace_bcm2835_vchiq_open(fourcc, srcport);
                replies += vchiq_queue_msg(s,
                    VCHIQ_MAKE_MSG(VCHIQ_MSG_CLOSE, 0, srcport), 0);
            }
            break;
        }
        case VCHIQ_MSG_DATA:
            trace_bcm2835_vchiq_rx(msgid, type, size);
            if (s->auds_open &&
                VCHIQ_MSG_DSTPORT(msgid) == VCHIQ_AUDS_VC_PORT) {
                replies += auds_handle_msg(s, hdr, size);
            } else if (s->disp_open &&
                       VCHIQ_MSG_DSTPORT(msgid) == VCHIQ_DISP_VC_PORT) {
                replies += disp_handle_msg(s, hdr, size);
            }
            break;
        case VCHIQ_MSG_BULK_TX:
        {
            /*
             * The samples. The payload is { data, size } where data is
             * the bus address of a pagelist describing where they are,
             * not of the buffer itself. The transfer must be completed
             * whatever we do with them, or the guest's bulk queue fills
             * and BCMSound blocks in BulkQueueTransmit.
             */
            uint32_t bulk_page = size >= 8
                               ? vchiq_ld(s, hdr + VCHIQ_MSG_HDR_SIZE) : 0;
            uint32_t bulk_size = size >= 8
                               ? vchiq_ld(s, hdr + VCHIQ_MSG_HDR_SIZE + 4) : 0;

            trace_bcm2835_vchiq_rx(msgid, type, size);
            if (s->auds_open &&
                VCHIQ_MSG_DSTPORT(msgid) == VCHIQ_AUDS_VC_PORT) {
                uint32_t got = vchiq_bulk_gather(s, bulk_page);

                if (got) {
                    trace_bcm2835_vchiq_auds_pcm(got,
                        vchiq_pcm_peak(s, s->bulk_buf, got));
                    vchiq_wav_write(s, s->bulk_buf, got);
                }
                replies += vchiq_queue_msg_data(s,
                    VCHIQ_MAKE_MSG(VCHIQ_MSG_BULK_TX_DONE,
                                   VCHIQ_AUDS_VC_PORT, s->auds_port),
                    &bulk_size, 4);
                /* Only what we actually read can be played on */
                auds_queue_playback(s, MIN(got, bulk_size));
            } else if (s->disp_open &&
                       VCHIQ_MSG_DSTPORT(msgid) == VCHIQ_DISP_VC_PORT) {
                /* The sprite image the ROM converted.  It lands in the
                 * staging buffer and is committed whole at the next
                 * UpdateSubmit, so the compositor can never read a
                 * half-written shape. */
                uint32_t got = vchiq_bulk_gather(s, bulk_page);

                if (got && s->disp_bulk_len) {
                    uint32_t n = MIN(got, s->disp_bulk_len);

                    memcpy(s->disp_stage, s->bulk_buf, n);
                    s->disp_stage_len = n;
                }
                replies += vchiq_queue_msg_data(s,
                    VCHIQ_MAKE_MSG(VCHIQ_MSG_BULK_TX_DONE,
                                   VCHIQ_DISP_VC_PORT, s->disp_port),
                    &bulk_size, 4);
            }
            break;
        }
        case VCHIQ_MSG_CLOSE:
            trace_bcm2835_vchiq_rx(msgid, type, size);
            if (s->auds_open &&
                VCHIQ_MSG_DSTPORT(msgid) == VCHIQ_AUDS_VC_PORT) {
                /* Closing the service, not the audio session: answer so
                 * the guest's own close completes rather than times out. */
                replies += vchiq_queue_msg(s,
                    VCHIQ_MAKE_MSG(VCHIQ_MSG_CLOSE,
                                   VCHIQ_AUDS_VC_PORT, s->auds_port), 0);
                auds_close(s);
            } else if (s->disp_open &&
                       VCHIQ_MSG_DSTPORT(msgid) == VCHIQ_DISP_VC_PORT) {
                replies += vchiq_queue_msg(s,
                    VCHIQ_MAKE_MSG(VCHIQ_MSG_CLOSE,
                                   VCHIQ_DISP_VC_PORT, s->disp_port), 0);
                s->disp_open = false;
                disp_reset(s);
            } else if (s->updh_open &&
                       VCHIQ_MSG_DSTPORT(msgid) == VCHIQ_UPDH_VC_PORT) {
                replies += vchiq_queue_msg(s,
                    VCHIQ_MAKE_MSG(VCHIQ_MSG_CLOSE,
                                   VCHIQ_UPDH_VC_PORT, s->updh_port), 0);
                s->updh_open = false;
            }
            break;
        case VCHIQ_MSG_CONNECT:
        case VCHIQ_MSG_PADDING:
            trace_bcm2835_vchiq_rx(msgid, type, size);
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "%s: unhandled VCHIQ message type %u "
                          "(msgid 0x%08x)\n", __func__, type, msgid);
            break;
        }
    }

    if (replies) {
        vchiq_signal_guest(s, s->slave_base + VCHIQ_SS_TRIGGER);
    }
}

/* The mailbox channel-3 window */

static uint64_t bcm2835_vchiq_mbox_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    switch (offset) {
    case MBOX_AS_DATA:
        /*
         * VCHIQ messages are fire and forget: the guest never reads a reply
         * from the mailbox, and queueing one here would fill a FIFO nothing
         * drains, eventually blocking the property channel too.
         */
        return 0;

    case MBOX_AS_PENDING:
        return 0;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        return 0;
    }
}

static void bcm2835_vchiq_mbox_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    BCM2835VchiqState *s = opaque;

    switch (offset) {
    case MBOX_AS_DATA:
        trace_bcm2835_vchiq_mbox_write((uint32_t)value);
        vchiq_init_from_slot_zero(s, (uint32_t)value);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        break;
    }
}

static const MemoryRegionOps bcm2835_vchiq_mbox_ops = {
    .read = bcm2835_vchiq_mbox_read,
    .write = bcm2835_vchiq_mbox_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* The doorbells */

static uint64_t bcm2835_vchiq_bell_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    BCM2835VchiqState *s = opaque;
    uint32_t val;

    switch (offset) {
    case VCHIQ_BELL0:
        /*
         * Read to clear. The guest's VCHIQ device declares no device-specific
         * interrupt clear, so this read is the only thing that lowers the
         * line -- and the interrupt is level triggered, so failing to lower
         * it here would put the guest in an interrupt storm.
         */
        val = s->bell0;
        s->bell0 = 0;
        qemu_set_irq(s->bell_irq, 0);
        return val;

    case VCHIQ_BELL2:
        return 0;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        return 0;
    }
}

static void bcm2835_vchiq_bell_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    BCM2835VchiqState *s = opaque;

    switch (offset) {
    case VCHIQ_BELL2:
        /* The guest has queued something for us */
        vchiq_parse_guest_messages(s);
        break;

    case VCHIQ_BELL0:
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset %"HWADDR_PRIx"\n",
                      __func__, offset);
        break;
    }
}

static const MemoryRegionOps bcm2835_vchiq_bell_ops = {
    .read = bcm2835_vchiq_bell_read,
    .write = bcm2835_vchiq_bell_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const VMStateDescription vmstate_bcm2835_vchiq = {
    .name = TYPE_BCM2835_VCHIQ,
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(bell0, BCM2835VchiqState),
        VMSTATE_UINT32(slot0, BCM2835VchiqState),
        VMSTATE_UINT32(master_base, BCM2835VchiqState),
        VMSTATE_UINT32(slave_base, BCM2835VchiqState),
        VMSTATE_UINT32(slot_size, BCM2835VchiqState),
        VMSTATE_UINT32(max_slots, BCM2835VchiqState),
        VMSTATE_UINT32(per_side, BCM2835VchiqState),
        VMSTATE_UINT32(tx_slot, BCM2835VchiqState),
        VMSTATE_UINT32(tx_pos, BCM2835VchiqState),
        VMSTATE_UINT32(rx_pos, BCM2835VchiqState),
        VMSTATE_BOOL(connected, BCM2835VchiqState),
        VMSTATE_BOOL(auds_open, BCM2835VchiqState),
        VMSTATE_UINT32(auds_port, BCM2835VchiqState),
        VMSTATE_UINT32(auds_rate, BCM2835VchiqState),
        VMSTATE_UINT32(auds_channels, BCM2835VchiqState),
        VMSTATE_UINT32(auds_bps, BCM2835VchiqState),
        VMSTATE_UINT32(auds_cookie1, BCM2835VchiqState),
        VMSTATE_UINT32(auds_cookie2, BCM2835VchiqState),
        VMSTATE_BOOL(auds_running, BCM2835VchiqState),
        VMSTATE_BOOL(disp_open, BCM2835VchiqState),
        VMSTATE_UINT32(disp_port, BCM2835VchiqState),
        VMSTATE_BOOL(updh_open, BCM2835VchiqState),
        VMSTATE_UINT32(updh_port, BCM2835VchiqState),
        VMSTATE_UINT32(disp_next_handle, BCM2835VchiqState),
        VMSTATE_UINT32(disp_resource, BCM2835VchiqState),
        VMSTATE_UINT32(disp_res_w, BCM2835VchiqState),
        VMSTATE_UINT32(disp_res_h, BCM2835VchiqState),
        VMSTATE_UINT32(disp_element, BCM2835VchiqState),
        VMSTATE_UINT32(disp_info_w, BCM2835VchiqState),
        VMSTATE_UINT32(disp_info_h, BCM2835VchiqState),
        VMSTATE_UINT32(disp_res_w, BCM2835VchiqState),
        VMSTATE_UINT32(disp_res_h, BCM2835VchiqState),
        VMSTATE_BOOL(ptr_visible, BCM2835VchiqState),
        VMSTATE_INT32(ptr_x, BCM2835VchiqState),
        VMSTATE_INT32(ptr_y, BCM2835VchiqState),
        VMSTATE_INT32(ptr_w, BCM2835VchiqState),
        VMSTATE_INT32(ptr_h, BCM2835VchiqState),
        VMSTATE_UINT32(ptr_img_w, BCM2835VchiqState),
        VMSTATE_UINT32(ptr_img_h, BCM2835VchiqState),
        VMSTATE_UINT32(ptr_gen, BCM2835VchiqState),
        VMSTATE_UINT32_ARRAY(ptr_image, BCM2835VchiqState,
                             VCHIQ_DISP_SPRITE_MAX * VCHIQ_DISP_SPRITE_MAX),
        VMSTATE_END_OF_LIST()
    }
};

static void bcm2835_vchiq_init(Object *obj)
{
    BCM2835VchiqState *s = BCM2835_VCHIQ(obj);

    memory_region_init_io(&s->iomem_mbox, obj, &bcm2835_vchiq_mbox_ops, s,
                          TYPE_BCM2835_VCHIQ "-mbox", 0x10);
    /* Reached from bcm2835_mbox, which reads back from us in the same call */
    s->iomem_mbox.disable_reentrancy_guard = true;

    memory_region_init_io(&s->iomem_bell, obj, &bcm2835_vchiq_bell_ops, s,
                          TYPE_BCM2835_VCHIQ "-bell", 0x10);

    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem_mbox);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem_bell);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->bell_irq);
}

static void bcm2835_vchiq_reset(DeviceState *dev)
{
    BCM2835VchiqState *s = BCM2835_VCHIQ(dev);

    s->bell0 = 0;
    s->slot0 = 0;
    s->master_base = 0;
    s->slave_base = 0;
    s->slot_size = 0;
    s->max_slots = 0;
    s->per_side = 0;
    s->tx_slot = 0;
    s->tx_pos = 0;
    s->rx_pos = 0;
    s->connected = false;
    if (s->wav) {
        fclose(s->wav);
        s->wav = NULL;
        s->wav_bytes = 0;
    }
    auds_close(s);
    s->auds_port = 0;
    s->auds_rate = 0;
    s->auds_channels = 0;
    s->auds_bps = 0;
    s->auds_cookie1 = 0;
    s->auds_cookie2 = 0;
    /* The services close with the machine, and the sprite goes too */
    s->auds_open = false;
    s->disp_open = false;
    s->updh_open = false;
    s->updh_port = 0;
    disp_reset(s);
}

static void bcm2835_vchiq_realize(DeviceState *dev, Error **errp)
{
    BCM2835VchiqState *s = BCM2835_VCHIQ(dev);
    Object *obj;

    obj = object_property_get_link(OBJECT(dev), "dma-mr", &error_abort);
    s->dma_mr = MEMORY_REGION(obj);
    address_space_init(&s->dma_as, s->dma_mr, TYPE_BCM2835_VCHIQ "-memory");

    /*
     * The sound card, if this machine has one. Not having one is not an
     * error: the guest's sound loop still has to turn, so the fallback
     * paces the reports on the virtual clock instead.
     */
    if (!audio_be_check(&s->audio_be, NULL)) {
        s->audio_be = NULL;
        warn_report("bcm2835-vchiq: no audio backend; RISC OS will play "
                    "to nothing");
    }

    /* The fallback clock. One deadline, fired under the BQL, the same
     * thread the vsync generator and the system timer run on. */
    s->auds_timer = hrtimer_new(auds_timer_fire, s);

    bcm2835_vchiq_reset(dev);
}

static const Property bcm2835_vchiq_props[] = {
    DEFINE_AUDIO_PROPERTIES(BCM2835VchiqState, audio_be),
    /* Where to write what the guest plays, for looking at before there
     * is anything to listen to. Empty means do not capture. */
    DEFINE_PROP_STRING("wav", BCM2835VchiqState, wav_path),
};

static void bcm2835_vchiq_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, bcm2835_vchiq_props);
    dc->realize = bcm2835_vchiq_realize;
    device_class_set_legacy_reset(dc, bcm2835_vchiq_reset);
    dc->vmsd = &vmstate_bcm2835_vchiq;
}

static const TypeInfo bcm2835_vchiq_info = {
    .name          = TYPE_BCM2835_VCHIQ,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835VchiqState),
    .class_init    = bcm2835_vchiq_class_init,
    .instance_init = bcm2835_vchiq_init,
};

static void bcm2835_vchiq_register_types(void)
{
    type_register_static(&bcm2835_vchiq_info);
}

type_init(bcm2835_vchiq_register_types)
