/*
 * High-resolution timer thread
 *
 * See include/qemu/hrtimer.h. One thread, one list of armed timers, one
 * wait. On Windows the wait is a high-resolution waitable timer, which
 * takes its due time in 100 ns units and honours it to within a few tens
 * of microseconds; elsewhere it is a GCond wait with a microsecond
 * deadline. A re-arm from another thread wakes the wait so the new
 * deadline is taken into account at once.
 *
 * Deadlines are on QEMU_CLOCK_VIRTUAL, which runs with the host's
 * monotonic clock while the machine runs and stands still while it is
 * stopped; the thread wakes at the host time the deadline would fall on,
 * and simply waits again if the virtual clock has not got there.
 *
 * Firing happens under the BQL: a timer is taken off the list and its
 * callback run only after the lock is held, so a guest write that
 * re-arms it (also under the BQL) cannot slip in between the two.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/hrtimer.h"
#include "qemu/main-loop.h"
#include "qemu/queue.h"
#include "qemu/rcu.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#ifdef _WIN32
#include <mmsystem.h>            /* timeBeginPeriod */
#endif

struct HRTimer {
    HRTimerCB *cb;
    void *opaque;
    int64_t deadline_ns;
    bool armed;
    QTAILQ_ENTRY(HRTimer) next;
};

/* The wait is capped so a stopped machine is re-checked now and then */
#define HRTIMER_MAX_WAIT_NS     (100 * SCALE_MS)
#define HRTIMER_DUE_BATCH       16

static struct {
    GMutex lock;                        /* zero-initialised is valid */
    GCond cond;                         /* POSIX wait and wake */
    QTAILQ_HEAD(, HRTimer) armed;
    bool running;
#ifdef _WIN32
    HANDLE timer;                       /* waitable timer, high-res if possible */
    HANDLE wake;                        /* auto-reset: a deadline changed */
#endif
} hr = {
    .armed = QTAILQ_HEAD_INITIALIZER(hr.armed),
};

/* Wait for up to wait_ns, or until woken; called and returns with the
 * list lock held. */
static void hrtimer_wait(int64_t wait_ns)
{
#ifdef _WIN32
    LARGE_INTEGER due;
    HANDLE handles[2] = { hr.timer, hr.wake };

    due.QuadPart = -MAX(wait_ns / 100, (int64_t)1);   /* relative, 100 ns */
    SetWaitableTimer(hr.timer, &due, 0, NULL, NULL, FALSE);
    g_mutex_unlock(&hr.lock);
    WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    g_mutex_lock(&hr.lock);
#else
    g_cond_wait_until(&hr.cond, &hr.lock,
                      g_get_monotonic_time() + wait_ns / SCALE_US);
#endif
}

/* Wake the wait so it re-reads the deadlines; called with the lock held */
static void hrtimer_wake(void)
{
#ifdef _WIN32
    SetEvent(hr.wake);
#else
    g_cond_signal(&hr.cond);
#endif
}

static void *hrtimer_thread(void *arg)
{
    HRTimer *due[HRTIMER_DUE_BATCH];
    HRTimer *t, *tnext;
    int64_t now, next;
    int n, i;

    rcu_register_thread();
    g_mutex_lock(&hr.lock);
    for (;;) {
        now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        next = INT64_MAX;
        QTAILQ_FOREACH(t, &hr.armed, next) {
            next = MIN(next, t->deadline_ns);
        }
        if (next > now) {
            hrtimer_wait(MIN(next - now, (int64_t)HRTIMER_MAX_WAIT_NS));
            continue;
        }

        /* Something is due. Take the BQL first, then decide what fires,
         * so nothing re-arms behind our back between the two. */
        g_mutex_unlock(&hr.lock);
        bql_lock();
        g_mutex_lock(&hr.lock);
        now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        n = 0;
        QTAILQ_FOREACH_SAFE(t, &hr.armed, next, tnext) {
            if (t->deadline_ns <= now && n < HRTIMER_DUE_BATCH) {
                QTAILQ_REMOVE(&hr.armed, t, next);
                t->armed = false;
                due[n++] = t;
            }
        }
        g_mutex_unlock(&hr.lock);
        for (i = 0; i < n; i++) {
            due[i]->cb(due[i]->opaque);
        }
        bql_unlock();
        g_mutex_lock(&hr.lock);
    }
    return NULL;
}

/* Called with the lock held */
static void hrtimer_start_thread(void)
{
    QemuThread thread;

#ifdef _WIN32
    hr.timer = CreateWaitableTimerExW(NULL, NULL,
                                      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
    if (!hr.timer) {
        /* Before Windows 10 1803: a plain waitable timer, so ask for the
         * finest scheduler period the kernel gives on request. */
        hr.timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
        timeBeginPeriod(1);
    }
    hr.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
#endif
    qemu_thread_create(&thread, "hrtimer", hrtimer_thread, NULL,
                       QEMU_THREAD_DETACHED);
    hr.running = true;
}

HRTimer *hrtimer_new(HRTimerCB *cb, void *opaque)
{
    HRTimer *t = g_new0(HRTimer, 1);

    t->cb = cb;
    t->opaque = opaque;
    g_mutex_lock(&hr.lock);
    if (!hr.running) {
        hrtimer_start_thread();
    }
    g_mutex_unlock(&hr.lock);
    return t;
}

void hrtimer_mod_ns(HRTimer *t, int64_t deadline_ns)
{
    g_mutex_lock(&hr.lock);
    if (!t->armed) {
        QTAILQ_INSERT_TAIL(&hr.armed, t, next);
        t->armed = true;
    }
    t->deadline_ns = deadline_ns;
    hrtimer_wake();
    g_mutex_unlock(&hr.lock);
}

void hrtimer_del(HRTimer *t)
{
    g_mutex_lock(&hr.lock);
    if (t->armed) {
        QTAILQ_REMOVE(&hr.armed, t, next);
        t->armed = false;
    }
    g_mutex_unlock(&hr.lock);
}
