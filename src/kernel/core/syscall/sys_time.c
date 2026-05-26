/*
 * sys_time.c — Time-related syscall implementations
 *
 *   sys_nanosleep(req, rem) — sleep for at least the duration in *req
 */

#include <stdint.h>

#include "common/errno.h"
#include "common/time.h"
#include "kernel/common/config.h"
#include "kernel/common/spinlock.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/proc/sched.h"
#include "kernel/core/syscall/syscall.h"

/* Nanoseconds per SysTick tick */
#define NS_PER_TICK (1000000000u / PPAP_TICK_HZ)

/* Kernel wallclock: seconds-since-epoch at tick 0.  Zero until
 * set via sys_settimeofday (or a future RTC seed).  Current wallclock
 * is time_boot_epoch + sched_get_ticks()/PPAP_TICK_HZ.  Readers and
 * setters pair the epoch with their tick snapshot under SPIN_TIME. */
static uint32_t time_boot_epoch;

void sys_time_now(uint32_t *sec_out, uint32_t *frac_ticks_out) {
  uint32_t saved = spin_lock_irqsave(SPIN_TIME);
  uint32_t ticks = sched_get_ticks();
  uint32_t epoch = time_boot_epoch;
  spin_unlock_irqrestore(SPIN_TIME, saved);

  uint32_t sec = epoch + ticks / PPAP_TICK_HZ;
  uint32_t frac = ticks % PPAP_TICK_HZ;
  if (sec_out) *sec_out = sec;
  if (frac_ticks_out) *frac_ticks_out = frac;
}

uint32_t time_now_sec(void) {
  uint32_t saved = spin_lock_irqsave(SPIN_TIME);
  uint32_t ticks = sched_get_ticks();
  uint32_t epoch = time_boot_epoch;
  spin_unlock_irqrestore(SPIN_TIME, saved);
  return epoch + ticks / PPAP_TICK_HZ;
}

/* Seed the kernel wallclock — pins time_now_sec() to return `sec`
 * at the current tick, so future reads add the tick delta on top.
 * Used by sys_settimeofday (user-initiated) and by target init
 * hooks that read an RTC on boot. */
void time_set_wallclock(uint32_t sec) {
  uint32_t saved = spin_lock_irqsave(SPIN_TIME);
  uint32_t ticks = sched_get_ticks();
  time_boot_epoch = sec - ticks / PPAP_TICK_HZ;
  spin_unlock_irqrestore(SPIN_TIME, saved);
}

static int timespec32_write_remaining(uintptr_t rem_ptr) {
  struct timespec rem;
  uint32_t now;
  uint32_t remaining_ticks;

  if (rem_ptr == 0u) return 0;
  now = sched_get_ticks();
  if (current->sleep_until != 0 && (int32_t)(current->sleep_until - now) > 0)
    remaining_ticks = current->sleep_until - now;
  else
    remaining_ticks = 0;
  rem.tv_sec = (long)(remaining_ticks / PPAP_TICK_HZ);
  rem.tv_nsec = (long)((remaining_ticks % PPAP_TICK_HZ) * NS_PER_TICK);
  return sys_copy_to_user(rem_ptr, &rem, sizeof(rem));
}

static int timespec64_write_remaining(uintptr_t rem_ptr) {
  int64_t rem[2];
  uint32_t now;
  uint32_t remaining_ticks;

  if (rem_ptr == 0u) return 0;
  now = sched_get_ticks();
  if (current->sleep_until != 0 && (int32_t)(current->sleep_until - now) > 0)
    remaining_ticks = current->sleep_until - now;
  else
    remaining_ticks = 0;
  rem[0] = (int64_t)(remaining_ticks / PPAP_TICK_HZ);
  rem[1] = (int64_t)((remaining_ticks % PPAP_TICK_HZ) * NS_PER_TICK);
  return sys_copy_to_user(rem_ptr, rem, sizeof(rem));
}

/* ── sys_nanosleep ────────────────────────────────────────────────────────────
 */

/*
 * Put the calling process to sleep for at least the duration in *req.
 *
 * Conversion: ticks = tv_sec * PPAP_TICK_HZ + tv_nsec / (1e9 / PPAP_TICK_HZ)
 *   With PPAP_TICK_HZ = 100:  1 tick = 10 ms, 1 ns/tick = 10_000_000 ns/tick
 *
 * Minimum sleep is 1 tick (10 ms) to avoid a no-op for sub-tick requests.
 *
 * Current limitations:
 *   - Sleep duration has SysTick resolution (10 ms).  Sub-tick requests
 *     are rounded up to 1 tick.  Removing this dependency on the periodic
 *     preemption tick is tracked in docs/proposals/hires_timer.md.
 *   - tv_sec is capped at UINT32_MAX / PPAP_TICK_HZ to prevent wrap-around.
 */
long sys_nanosleep(uintptr_t req_ptr, uintptr_t rem_ptr) {
  if (req_ptr == 0u) return -(long)EINVAL;

  struct timespec ts;
  if (sys_copy_from_user(&ts, req_ptr, sizeof(ts)) < 0) return -(long)EFAULT;
  if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L)
    return -(long)EINVAL;

  uint32_t ticks =
      (uint32_t)ts.tv_sec * PPAP_TICK_HZ + (uint32_t)ts.tv_nsec / NS_PER_TICK;
  if (ticks == 0u) ticks = 1u;
  current->sleep_until = sched_get_ticks() + ticks;

  for (;;) {
    if (current->sig_pending & ~current->sig_blocked) {
      int rc = timespec32_write_remaining(rem_ptr);
      current->sleep_until = 0;
      return rc < 0 ? -(long)EFAULT : -(long)EINTR;
    }
    if ((int32_t)(sched_get_ticks() - current->sleep_until) >= 0) {
      current->sleep_until = 0;
      return 0;
    }
    current->state = PROC_SLEEPING;
    sched_switch();
  }
}

/* ── Time conversion helper ───────────────────────────────────────────────────
 */

/* ── sys_clock_gettime32 ─────────────────────────────────────────────────────
 */
/*
 * 32-bit timespec: { long tv_sec; long tv_nsec; }
 * clk_id: CLOCK_REALTIME(0), CLOCK_MONOTONIC(1), etc. — we treat all the same.
 */
long sys_clock_gettime32(long clk_id, uintptr_t tp_ptr) {
  long ts[2];

  (void)clk_id;
  if (tp_ptr == 0u) return -(long)EINVAL;

  uint32_t sec, frac;
  sys_time_now(&sec, &frac);

  ts[0] = (long)sec;
  ts[1] = (long)(frac * NS_PER_TICK);
  if (sys_copy_to_user(tp_ptr, ts, sizeof(ts)) < 0) return -(long)EFAULT;
  return 0;
}

/* ── sys_clock_gettime64 ─────────────────────────────────────────────────────
 */
/*
 * 64-bit timespec: { int64_t tv_sec; int64_t tv_nsec; }
 * Linux time64 first-try path used by libc clock_gettime().
 */
long sys_clock_gettime64(long clk_id, uintptr_t tp_ptr) {
  int64_t ts[2];

  (void)clk_id;
  if (tp_ptr == 0u) return -(long)EINVAL;

  uint32_t sec, frac;
  sys_time_now(&sec, &frac);

  ts[0] = (int64_t)sec;
  ts[1] = (int64_t)(frac * NS_PER_TICK);
  if (sys_copy_to_user(tp_ptr, ts, sizeof(ts)) < 0) return -(long)EFAULT;
  return 0;
}

/* ── sys_gettimeofday ────────────────────────────────────────────────────────
 */
/*
 * struct timeval { long tv_sec; long tv_usec; }
 */
long sys_gettimeofday(uintptr_t tv_ptr, uintptr_t tz_ptr) {
  long tv[2];

  (void)tz_ptr;
  if (tv_ptr == 0u) return -(long)EINVAL;

  uint32_t sec, frac;
  sys_time_now(&sec, &frac);

  tv[0] = (long)sec;
  tv[1] = (long)(frac * (1000000u / PPAP_TICK_HZ)); /* microseconds */
  if (sys_copy_to_user(tv_ptr, tv, sizeof(tv)) < 0) return -(long)EFAULT;
  return 0;
}

/* ── sys_settimeofday ────────────────────────────────────────────────────── */
/*
 * Set the kernel wallclock.  tz_ptr is accepted for POSIX compatibility
 * but ignored — PPAP has no timezone concept.
 */
long sys_settimeofday(uintptr_t tv_ptr, uintptr_t tz_ptr) {
  long tv[2];

  (void)tz_ptr;
  if (tv_ptr == 0u) return -(long)EINVAL;
  if (sys_copy_from_user(tv, tv_ptr, sizeof(tv)) < 0) return -(long)EFAULT;
  if (tv[0] < 0) return -(long)EINVAL;

  time_set_wallclock((uint32_t)tv[0]);
  return 0;
}

/* ── sys_clock_nanosleep32 ───────────────────────────────────────────────────
 */
/*
 * clock_nanosleep(clk, flags, req, rem)
 * 32-bit timespec { long tv_sec; long tv_nsec; }
 */
long sys_clock_nanosleep32(long clk, long flags, uintptr_t req_ptr,
                           uintptr_t rem_ptr) {
  long ts[2];

  (void)clk;
  (void)flags;
  if (req_ptr == 0u) return -(long)EINVAL;

  if (sys_copy_from_user(ts, req_ptr, sizeof(ts)) < 0) return -(long)EFAULT;
  if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000L) return -(long)EINVAL;

  uint32_t ticks =
      (uint32_t)ts[0] * PPAP_TICK_HZ + (uint32_t)ts[1] / NS_PER_TICK;
  if (ticks == 0u) ticks = 1u;
  current->sleep_until = sched_get_ticks() + ticks;

  for (;;) {
    if (current->sig_pending & ~current->sig_blocked) {
      int rc = timespec32_write_remaining(rem_ptr);
      current->sleep_until = 0;
      return rc < 0 ? -(long)EFAULT : -(long)EINTR;
    }
    if ((int32_t)(sched_get_ticks() - current->sleep_until) >= 0) {
      current->sleep_until = 0;
      return 0;
    }
    current->state = PROC_SLEEPING;
    sched_switch();
  }
}

/* ── sys_clock_nanosleep64 ───────────────────────────────────────────────────
 */
/*
 * 64-bit timespec { int64_t tv_sec; int64_t tv_nsec; }
 */
long sys_clock_nanosleep64(long clk, long flags, uintptr_t req_ptr,
                           uintptr_t rem_ptr) {
  int64_t ts[2];

  (void)clk;
  (void)flags;
  if (req_ptr == 0u) return -(long)EINVAL;

  if (sys_copy_from_user(ts, req_ptr, sizeof(ts)) < 0) return -(long)EFAULT;
  if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000LL) return -(long)EINVAL;

  uint32_t ticks =
      (uint32_t)ts[0] * PPAP_TICK_HZ + (uint32_t)ts[1] / NS_PER_TICK;
  if (ticks == 0u) ticks = 1u;
  current->sleep_until = sched_get_ticks() + ticks;

  for (;;) {
    if (current->sig_pending & ~current->sig_blocked) {
      int rc = timespec64_write_remaining(rem_ptr);
      current->sleep_until = 0;
      return rc < 0 ? -(long)EFAULT : -(long)EINTR;
    }
    if ((int32_t)(sched_get_ticks() - current->sleep_until) >= 0) {
      current->sleep_until = 0;
      return 0;
    }
    current->state = PROC_SLEEPING;
    sched_switch();
  }
}
