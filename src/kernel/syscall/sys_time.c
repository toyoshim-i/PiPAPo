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
