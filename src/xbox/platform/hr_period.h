/*
 * hr_period -- a periodic deadline that does not drift.
 *
 * Relative sleeps cannot hold a rate.  `Sleep(16)` in a loop asks for 16 ms
 * and gets whatever the scheduler tick grants -- measured here at a 30.13 ms
 * mean, so a nominal 60 Hz vblank ran at 33.2 Hz and every guest timeline
 * derived from it ran at half speed.  Two things fix that: an absolute
 * deadline accumulated once per tick, so jitter on one tick is absorbed by
 * the next instead of accumulating, and a wait primitive whose resolution is
 * finer than a scheduler tick.
 *
 * The caller owns the state, so several independent periods can run at once
 * on different threads without sharing anything.
 *
 * hr_period_wait() blocks until the next deadline and returns.  It waits on
 * the timer for all but the last HR_SPIN_MARGIN_US, then yields in a short
 * spin -- it does not busy-spin the interval.
 *
 * If a tick is missed by more than a whole period the deadline is resynced to
 * now rather than firing the backlog: a caller raising an interrupt per tick
 * must not deliver a burst to make up lost time.  hr_period_resyncs() reports
 * how often that happened so the condition stays visible.
 */
#ifndef HR_PERIOD_H
#define HR_PERIOD_H

/* How much of the tail to spin rather than wait.  A high-resolution waitable
 * timer resolves to well under this, so the spin is normally not entered. */
#define HR_SPIN_MARGIN_US 250

#if defined(_WIN32)

#include <windows.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

typedef struct {
    HANDLE        timer;
    long long     qpc_freq;
    long long     period;      /* QPC ticks */
    long long     margin;      /* QPC ticks */
    long long     next;        /* absolute QPC deadline */
    unsigned long long ticks, resyncs;
    int           high_res;
} hr_period;

static long long hr_now(void)
{
    LARGE_INTEGER v; QueryPerformanceCounter(&v);
    return (long long)v.QuadPart;
}

static void hr_period_init(hr_period *p, double hz)
{
    LARGE_INTEGER f;
    memset(p, 0, sizeof(*p));
    QueryPerformanceFrequency(&f);
    p->qpc_freq = (long long)f.QuadPart;
    p->period   = (long long)((double)p->qpc_freq / hz + 0.5);
    p->margin   = (p->qpc_freq * HR_SPIN_MARGIN_US) / 1000000;

    /* A high-resolution timer ignores the scheduler tick; without the flag
     * the wait rounds up to it and we are back where we started. */
    p->timer = CreateWaitableTimerExW(NULL, NULL,
                                      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
    p->high_res = (p->timer != NULL);
    if (!p->timer)
        p->timer = CreateWaitableTimerW(NULL, FALSE, NULL);

    p->next = hr_now();
}

/* Move the deadline on by one period.  Call once per tick, before waiting. */
static void hr_period_advance(hr_period *p)
{
    long long now;
    p->next += p->period;
    now = hr_now();
    if (now - p->next > p->period) {
        /* More than a whole period late.  Drop what was missed instead of
         * firing it back to back. */
        p->next = now + p->period;
        ++p->resyncs;
    }
    ++p->ticks;
}

/* QPC ticks left until the deadline; <= 0 once it has passed. */
static long long hr_period_remaining(const hr_period *p)
{
    return p->next - hr_now();
}

/* Wait once, for at most `remaining` ticks.  Uses the timer for all but the
 * tail margin and yields through the tail; it does not loop to the deadline,
 * so a caller that has to drop a lock around the wait stays in control of
 * when it re-acquires. */
static void hr_period_wait_once(hr_period *p, long long remaining)
{
    if (remaining <= 0) return;

    if (remaining > p->margin && p->timer) {
        LARGE_INTEGER due;
        /* Negative = relative, in 100 ns units. */
        due.QuadPart = -(((remaining - p->margin) * 10000000LL) / p->qpc_freq);
        if (due.QuadPart < 0 &&
            SetWaitableTimer(p->timer, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(p->timer, INFINITE);
    } else {
        YieldProcessor();
    }
}

/* Block until the next deadline. */
static void hr_period_wait(hr_period *p)
{
    long long remaining;
    hr_period_advance(p);
    while ((remaining = hr_period_remaining(p)) > 0)
        hr_period_wait_once(p, remaining);
}

#else /* POSIX */

#include <time.h>
#include <string.h>

typedef struct {
    long long period_ns, margin_ns, next_ns;
    unsigned long long ticks, resyncs;
    int high_res;
} hr_period;

static long long hr_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void hr_period_init(hr_period *p, double hz)
{
    memset(p, 0, sizeof(*p));
    p->period_ns = (long long)(1000000000.0 / hz + 0.5);
    p->margin_ns = HR_SPIN_MARGIN_US * 1000LL;
    p->next_ns   = hr_now();
    p->high_res  = 1;
}

static void hr_period_advance(hr_period *p)
{
    long long now;
    p->next_ns += p->period_ns;
    now = hr_now();
    if (now - p->next_ns > p->period_ns) {
        p->next_ns = now + p->period_ns;
        ++p->resyncs;
    }
    ++p->ticks;
}

static long long hr_period_remaining(const hr_period *p)
{
    return p->next_ns - hr_now();
}

static void hr_period_wait_once(hr_period *p, long long remaining)
{
    if (remaining <= 0) return;
    if (remaining > p->margin_ns) {
        struct timespec req;
        req.tv_sec  = (time_t)((remaining - p->margin_ns) / 1000000000LL);
        req.tv_nsec = (long)((remaining - p->margin_ns) % 1000000000LL);
        nanosleep(&req, NULL);
    } else {
        sched_yield();
    }
}

static void hr_period_wait(hr_period *p)
{
    long long remaining;
    hr_period_advance(p);
    while ((remaining = hr_period_remaining(p)) > 0)
        hr_period_wait_once(p, remaining);
}

#endif

#endif /* HR_PERIOD_H */
