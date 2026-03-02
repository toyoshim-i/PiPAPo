/*
 * sys_time.c — Time-related syscall implementations
 *
 *   sys_nanosleep(req, rem) — sleep for at least the duration in *req
 */

#include "syscall.h"
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
    if (!req)
        return -(long)EINVAL;

    const struct timespec *ts = (const struct timespec *)req;
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L)
        return -(long)EINVAL;

    /* Nanoseconds per SysTick tick */
    const uint32_t NS_PER_TICK = 1000000000u / PPAP_TICK_HZ;

    uint32_t ticks = (uint32_t)ts->tv_sec * PPAP_TICK_HZ
                   + (uint32_t)ts->tv_nsec / NS_PER_TICK;

    if (ticks == 0u)
        ticks = 1u;  /* always sleep at least one tick */

    sched_sleep(ticks);
    /* Process resumes here after being woken by sched_tick() */

    (void)rem;  /* not filled in Phase 1 */
    return 0;
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

    const long *ts = (const long *)req;
    if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000L)
        return -(long)EINVAL;

    uint32_t ticks = (uint32_t)ts[0] * PPAP_TICK_HZ
                   + (uint32_t)ts[1] / NS_PER_TICK;
    if (ticks == 0u)
        ticks = 1u;

    sched_sleep(ticks);
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

    const int64_t *ts = (const int64_t *)req;
    if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000LL)
        return -(long)EINVAL;

    uint32_t ticks = (uint32_t)ts[0] * PPAP_TICK_HZ
                   + (uint32_t)ts[1] / NS_PER_TICK;
    if (ticks == 0u)
        ticks = 1u;

    sched_sleep(ticks);
    return 0;
}
