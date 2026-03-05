/*
 * sys_time.c — Time-related syscall implementations
 *
 *   sys_nanosleep(req, rem) — sleep for at least the duration in *req
 */

#include "syscall.h"
#include "../proc/proc.h"
#include "../proc/sched.h"
#include "../errno.h"
#include "config.h"
#include <stdint.h>

/*
 * struct timespec — POSIX nanosecond-resolution time value.
 * Matches the layout used by musl libc on 32-bit ARM.
 */
struct timespec {
    long tv_sec;    /* seconds          */
    long tv_nsec;   /* nanoseconds [0, 999999999] */
};

/* ── sys_nanosleep ──────────────────────────────────────────────────────────── */

/*
 * Put the calling process to sleep for at least the duration in *req.
 *
 * Conversion: ticks = tv_sec * PPAP_TICK_HZ + tv_nsec / (1e9 / PPAP_TICK_HZ)
 *   With PPAP_TICK_HZ = 100:  1 tick = 10 ms, 1 ns/tick = 10_000_000 ns/tick
 *
 * Minimum sleep is 1 tick (10 ms) to avoid a no-op for sub-tick requests.
 *
 * Phase 1 limitations:
 *   - rem is not filled in (no signal interruption in Phase 1).
 *   - Sleep duration has SysTick resolution (10 ms).
 *   - tv_sec is capped at UINT32_MAX / PPAP_TICK_HZ to prevent wrap-around.
 */
long sys_nanosleep(void *req, void *rem)
{
    (void)rem;
    if (!req)
        return -(long)EINVAL;

    /* On svc_restart re-entry: check for pending signal first */
    if (current->sig_pending & ~current->sig_blocked) {
        current->sleep_until = 0;
        return -(long)EINTR;
    }

    /* On svc_restart re-entry: check if sleep has expired */
    if (current->sleep_until != 0 &&
        (int32_t)(sched_get_ticks() - current->sleep_until) >= 0) {
        current->sleep_until = 0;
        return 0;
    }

    /* First entry (sleep_until == 0): compute deadline */
    if (current->sleep_until == 0) {
        const struct timespec *ts = (const struct timespec *)req;
        if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L)
            return -(long)EINVAL;

        const uint32_t NS_PER_TICK = 1000000000u / PPAP_TICK_HZ;
        uint32_t ticks = (uint32_t)ts->tv_sec * PPAP_TICK_HZ
                       + (uint32_t)ts->tv_nsec / NS_PER_TICK;
        if (ticks == 0u)
            ticks = 1u;

        current->sleep_until = sched_get_ticks() + ticks;
    }

    /* Block with svc_restart so SVC re-executes when woken */
    current->state = PROC_SLEEPING;
    svc_restart[core_id()] = 1;
    sched_yield();
    return 0;   /* ignored — SVC restores original args */
}

/* ── Time conversion helper ─────────────────────────────────────────────────── */

/* Nanoseconds per SysTick tick */
#define NS_PER_TICK  (1000000000u / PPAP_TICK_HZ)

/* ── sys_clock_gettime32 ───────────────────────────────────────────────────── */
/*
 * 32-bit timespec: { long tv_sec; long tv_nsec; }
 * clk_id: CLOCK_REALTIME(0), CLOCK_MONOTONIC(1), etc. — we treat all the same.
 */
long sys_clock_gettime32(long clk_id, void *tp)
{
    (void)clk_id;
    if (!tp)
        return -(long)EINVAL;

    uint32_t ticks = sched_get_ticks();
    uint32_t sec  = ticks / PPAP_TICK_HZ;
    uint32_t frac = ticks % PPAP_TICK_HZ;

    long *ts = (long *)tp;
    ts[0] = (long)sec;
    ts[1] = (long)(frac * NS_PER_TICK);
    return 0;
}

/* ── sys_clock_gettime64 ───────────────────────────────────────────────────── */
/*
 * 64-bit timespec: { int64_t tv_sec; int64_t tv_nsec; }
 * musl time64 first-try path.
 */
long sys_clock_gettime64(long clk_id, void *tp)
{
    (void)clk_id;
    if (!tp)
        return -(long)EINVAL;

    uint32_t ticks = sched_get_ticks();
    uint32_t sec  = ticks / PPAP_TICK_HZ;
    uint32_t frac = ticks % PPAP_TICK_HZ;

    int64_t *ts = (int64_t *)tp;
    ts[0] = (int64_t)sec;
    ts[1] = (int64_t)(frac * NS_PER_TICK);
    return 0;
}

/* ── sys_gettimeofday ──────────────────────────────────────────────────────── */
/*
 * struct timeval { long tv_sec; long tv_usec; }
 */
long sys_gettimeofday(void *tv, void *tz)
{
    (void)tz;
    if (!tv)
        return -(long)EINVAL;

    uint32_t ticks = sched_get_ticks();
    uint32_t sec  = ticks / PPAP_TICK_HZ;
    uint32_t frac = ticks % PPAP_TICK_HZ;

    long *t = (long *)tv;
    t[0] = (long)sec;
    t[1] = (long)(frac * (1000000u / PPAP_TICK_HZ));   /* microseconds */
    return 0;
}

/* ── sys_clock_nanosleep32 ─────────────────────────────────────────────────── */
/*
 * clock_nanosleep(clk, flags, req, rem)
 * 32-bit timespec { long tv_sec; long tv_nsec; }
 */
long sys_clock_nanosleep32(long clk, long flags, const void *req, void *rem)
{
    (void)clk; (void)flags; (void)rem;
    if (!req)
        return -(long)EINVAL;

    /* On svc_restart re-entry: check for pending signal first */
    if (current->sig_pending & ~current->sig_blocked) {
        current->sleep_until = 0;
        return -(long)EINTR;
    }

    /* On svc_restart re-entry: check if sleep has expired */
    if (current->sleep_until != 0 &&
        (int32_t)(sched_get_ticks() - current->sleep_until) >= 0) {
        current->sleep_until = 0;
        return 0;
    }

    /* First entry: compute deadline */
    if (current->sleep_until == 0) {
        const long *ts = (const long *)req;
        if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000L)
            return -(long)EINVAL;

        uint32_t ticks = (uint32_t)ts[0] * PPAP_TICK_HZ
                       + (uint32_t)ts[1] / NS_PER_TICK;
        if (ticks == 0u)
            ticks = 1u;

        current->sleep_until = sched_get_ticks() + ticks;
    }

    /* Block with svc_restart */
    current->state = PROC_SLEEPING;
    svc_restart[core_id()] = 1;
    sched_yield();
    return 0;
}

/* ── sys_clock_nanosleep64 ─────────────────────────────────────────────────── */
/*
 * 64-bit timespec { int64_t tv_sec; int64_t tv_nsec; }
 */
long sys_clock_nanosleep64(long clk, long flags, const void *req, void *rem)
{
    (void)clk; (void)flags; (void)rem;
    if (!req)
        return -(long)EINVAL;

    /* On svc_restart re-entry: check for pending signal first */
    if (current->sig_pending & ~current->sig_blocked) {
        current->sleep_until = 0;
        return -(long)EINTR;
    }

    /* On svc_restart re-entry: check if sleep has expired */
    if (current->sleep_until != 0 &&
        (int32_t)(sched_get_ticks() - current->sleep_until) >= 0) {
        current->sleep_until = 0;
        return 0;
    }

    /* First entry: compute deadline */
    if (current->sleep_until == 0) {
        const int64_t *ts = (const int64_t *)req;
        if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000LL)
            return -(long)EINVAL;

        uint32_t ticks = (uint32_t)ts[0] * PPAP_TICK_HZ
                       + (uint32_t)ts[1] / NS_PER_TICK;
        if (ticks == 0u)
            ticks = 1u;

        current->sleep_until = sched_get_ticks() + ticks;
    }

    /* Block with svc_restart */
    current->state = PROC_SLEEPING;
    svc_restart[core_id()] = 1;
    sched_yield();
    return 0;
}
