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
#include "hw/misc/bcm2835_mbox_defs.h"
#include "hw/misc/bcm2835_vchiq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "trace.h"

static uint32_t vchiq_ld(BCM2835VchiqState *s, uint32_t addr)
{
    return ldl_le_phys(&s->dma_as, addr);
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
        auds_result(s, 0);
        return 1;

    case VC_AUDIO_MSG_TYPE_OPEN:
        return 0;               /* sent without waiting for a result */

    case VC_AUDIO_MSG_TYPE_START:
        s->auds_running = true;
        s->auds_played_ns = 0;
        return 0;

    case VC_AUDIO_MSG_TYPE_STOP:
        s->auds_running = false;
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
            }
            break;
        case VCHIQ_MSG_BULK_TX:
        {
            /*
             * The samples themselves. The payload is { data, size } where
             * data is the bus address of a pagelist, not of the buffer --
             * this sprint acknowledges the transfer without reading it,
             * so the pagelist can wait for the one that plays the sound.
             * The transfer must still be completed or the guest's bulk
             * queue fills and BCMSound blocks in BulkQueueTransmit.
             */
            uint32_t bulk_size = size >= 8
                               ? vchiq_ld(s, hdr + VCHIQ_MSG_HDR_SIZE + 4) : 0;

            trace_bcm2835_vchiq_rx(msgid, type, size);
            if (s->auds_open &&
                VCHIQ_MSG_DSTPORT(msgid) == VCHIQ_AUDS_VC_PORT) {
                replies += vchiq_queue_msg_data(s,
                    VCHIQ_MAKE_MSG(VCHIQ_MSG_BULK_TX_DONE,
                                   VCHIQ_AUDS_VC_PORT, s->auds_port),
                    &bulk_size, 4);
                auds_queue_playback(s, bulk_size);
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
    .version_id = 2,
    .minimum_version_id = 2,
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
    auds_close(s);
    s->auds_port = 0;
    s->auds_rate = 0;
    s->auds_channels = 0;
    s->auds_bps = 0;
    s->auds_cookie1 = 0;
    s->auds_cookie2 = 0;
}

static void bcm2835_vchiq_realize(DeviceState *dev, Error **errp)
{
    BCM2835VchiqState *s = BCM2835_VCHIQ(dev);
    Object *obj;

    obj = object_property_get_link(OBJECT(dev), "dma-mr", &error_abort);
    s->dma_mr = MEMORY_REGION(obj);
    address_space_init(&s->dma_as, s->dma_mr, TYPE_BCM2835_VCHIQ "-memory");

    /* The audio clock. One deadline list, fired under the BQL, the same
     * thread the vsync generator and the system timer run on. */
    s->auds_timer = hrtimer_new(auds_timer_fire, s);

    bcm2835_vchiq_reset(dev);
}

static void bcm2835_vchiq_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

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
