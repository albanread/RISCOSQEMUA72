/*
 * High-resolution timer thread
 *
 * Deadlines on the virtual clock, waited for by one host thread with the
 * finest timer the host offers, callbacks run with the BQL held. It exists
 * for guests that are driven by interrupts rather than by polling: RISC OS
 * wants its 100 Hz ticker, its vertical sync and its video driver's
 * half-frame update to arrive when they are due, not when the main loop
 * next gets round to its millisecond poll.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef QEMU_HRTIMER_H
#define QEMU_HRTIMER_H

typedef struct HRTimer HRTimer;
typedef void HRTimerCB(void *opaque);

/* A timer whose callback runs on the timer thread, BQL held. */
HRTimer *hrtimer_new(HRTimerCB *cb, void *opaque);

/* Arm for an absolute QEMU_CLOCK_VIRTUAL deadline in ns; re-arming moves
 * it. Callable from any thread, including the callback itself. */
void hrtimer_mod_ns(HRTimer *t, int64_t deadline_ns);

/* Disarm. */
void hrtimer_del(HRTimer *t);

#endif
