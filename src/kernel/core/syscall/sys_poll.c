/*
 * sys_poll.c — ppoll / ppoll_time64 syscall implementation
 *
 * Implements the poll() interface used by busybox applets (top, vi, etc.)
 * to check for pending I/O on file descriptors with optional timeout.
 *
 * Blocking strategy:
 *   - Poll all fds via file_ops->poll callback
 *   - If any events match, return immediately
 *   - If timeout is zero, return 0 (non-blocking poll)
 *   - Otherwise, loop in kernel: set wait_channel, PROC_BLOCKED, sched_switch.
 *     The process is woken by either:
 *       (a) data arrival (e.g. tty_rx_notify → sched_wakeup)
 *       (b) timeout expiry (sched_tick checks PROC_BLOCKED + sleep_until)
 *       (c) signal delivery (loop returns -EINTR on next iteration)
 */

#include <stdint.h>

#include "common/errno.h"
#include "common/poll.h" /* POLLIN, POLLOUT, POLLNVAL, struct pollfd */
#include "kernel/common/config.h"
#include "kernel/common/mod/mod_core.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/syscall/syscall.h"

/* Convert timespec to ticks.  Returns 0 for zero timeout. */
static uint32_t ts32_to_ticks(const long *ts) {
  if (ts[0] < 0) return 0;
  uint32_t ticks = (uint32_t)ts[0] * PPAP_TICK_HZ +
                   (uint32_t)ts[1] / (1000000000u / PPAP_TICK_HZ);
  return ticks;
}

static uint32_t ts64_to_ticks(const int64_t *ts) {
  if (ts[0] < 0) return 0;
  uint32_t ticks = (uint32_t)ts[0] * PPAP_TICK_HZ +
                   (uint32_t)ts[1] / (1000000000u / PPAP_TICK_HZ);
  return ticks;
}

/* Core poll logic shared by ppoll and ppoll_time64. */
static long do_ppoll(struct pollfd *fds, uint32_t nfds, uint32_t timeout_ticks,
                     int has_timeout) {
  if (!fds && nfds > 0) return -(long)EINVAL;
  if (nfds > (uint32_t)FD_MAX) nfds = (uint32_t)FD_MAX;

  if (has_timeout && timeout_ticks > 0)
    current->sleep_until = mod_core.sched_get_ticks() + timeout_ticks;

  for (;;) {
    /* Check all fds for ready events */
    int ready = 0;
    for (uint32_t i = 0; i < nfds; i++) {
      fds[i].revents = 0;
      int fd = fds[i].fd;
      if (fd < 0) continue; /* negative fd → skip (Linux behaviour) */

      if (fd >= FD_MAX || current->fd_map[fd] == FD_DESC_NONE) {
        fds[i].revents = (short)POLLNVAL;
        ready++;
        continue;
      }

      int mask = mod_vfs.fd_poll(current->fd_map[fd]);

      fds[i].revents = (short)(mask & (int)fds[i].events);
      if (fds[i].revents) ready++;
    }

    if (ready > 0) {
      current->sleep_until = 0;
      return (long)ready;
    }

    /* Non-blocking poll (timeout == 0): return immediately */
    if (has_timeout && timeout_ticks == 0) {
      current->sleep_until = 0;
      return 0;
    }

    /* Check if the deadline has elapsed */
    if (has_timeout &&
        (int32_t)(mod_core.sched_get_ticks() - current->sleep_until) >= 0) {
      current->sleep_until = 0;
      return 0; /* timeout */
    }

    /* Check for pending signals */
    if (current->sig_pending & ~current->sig_blocked) {
      current->sleep_until = 0;
      return -(long)EINTR;
    }

    /* Block: wait for data or timeout.
     * Use the first polled fd's priv as wait channel — for tty fds this
     * is the tty_dev_t*, matching what tty_rx_notify() wakes. */
    void *channel = NULL;
    for (uint32_t i = 0; i < nfds; i++) {
      int fd = fds[i].fd;
      if (fd >= 0 && fd < FD_MAX && current->fd_map[fd] != FD_DESC_NONE) {
        void *priv = mod_vfs.fd_get_priv(current->fd_map[fd]);
        if (priv) {
          channel = priv;
          break;
        }
      }
    }
    mod_core.sched_block_current(channel);
    mod_core.sched_switch();
  }
}

/* ── sys_poll (SYS_POLL = 168) ───────────────────────────────────────────────
 */
/*
 * poll(fds, nfds, timeout_ms)
 *   timeout_ms < 0  → wait forever
 *   timeout_ms == 0 → non-blocking
 *   timeout_ms > 0  → wait up to timeout_ms milliseconds
 */
long sys_poll(uintptr_t fds_ptr, uint32_t nfds, long timeout_ms) {
  struct pollfd local_fds[FD_MAX];
  int has_timeout = (timeout_ms >= 0);
  uint32_t ticks = 0;
  int rc;

  if (timeout_ms > 0) ticks = (uint32_t)timeout_ms * PPAP_TICK_HZ / 1000u;
  if (nfds > (uint32_t)FD_MAX) nfds = (uint32_t)FD_MAX;
  if (nfds > 0) {
    if (fds_ptr == 0u) return -(long)EINVAL;
    rc = sys_copy_from_user(local_fds, fds_ptr, nfds * sizeof(local_fds[0]));
    if (rc < 0) return (long)rc;
  }

  rc = (int)do_ppoll(local_fds, nfds, ticks, has_timeout);
  if (nfds > 0) {
    int copy_rc =
        sys_copy_to_user(fds_ptr, local_fds, nfds * sizeof(local_fds[0]));
    if (copy_rc < 0) return (long)copy_rc;
  }
  return (long)rc;
}

/* ── sys_ppoll (SYS_PPOLL = 336) ────────────────────────────────────────────
 */
/*
 * ppoll(fds, nfds, timeout_ts, sigmask, sigsetsize)
 * timeout_ts: 32-bit struct timespec { long tv_sec; long tv_nsec; }
 */
long sys_ppoll(uintptr_t fds_ptr, uint32_t nfds, uintptr_t timeout_ptr,
               uintptr_t sigmask_ptr, uint32_t sigsetsize) {
  struct pollfd local_fds[FD_MAX];
  long timeout[2];
  int rc;

  (void)sigmask_ptr;
  (void)sigsetsize;

  int has_timeout = (timeout_ptr != 0u);
  uint32_t ticks = 0;
  if (nfds > (uint32_t)FD_MAX) nfds = (uint32_t)FD_MAX;
  if (nfds > 0) {
    if (fds_ptr == 0u) return -(long)EINVAL;
    rc = sys_copy_from_user(local_fds, fds_ptr, nfds * sizeof(local_fds[0]));
    if (rc < 0) return (long)rc;
  }
  if (has_timeout) {
    rc = sys_copy_from_user(timeout, timeout_ptr, sizeof(timeout));
    if (rc < 0) return (long)rc;
    ticks = ts32_to_ticks(timeout);
  }

  rc = (int)do_ppoll(local_fds, nfds, ticks, has_timeout);
  if (nfds > 0) {
    int copy_rc =
        sys_copy_to_user(fds_ptr, local_fds, nfds * sizeof(local_fds[0]));
    if (copy_rc < 0) return (long)copy_rc;
  }
  return (long)rc;
}

/* ── sys_ppoll_time64 (SYS_PPOLL_TIME64 = 414) ───────────────────────────── */
/*
 * ppoll_time64(fds, nfds, timeout_ts64, sigmask, sigsetsize)
 * timeout_ts64: 64-bit struct timespec { int64_t tv_sec; int64_t tv_nsec; }
 */
long sys_ppoll_time64(uintptr_t fds_ptr, uint32_t nfds, uintptr_t timeout_ptr,
                      uintptr_t sigmask_ptr, uint32_t sigsetsize) {
  struct pollfd local_fds[FD_MAX];
  int64_t timeout[2];
  int rc;

  (void)sigmask_ptr;
  (void)sigsetsize;

  int has_timeout = (timeout_ptr != 0u);
  uint32_t ticks = 0;
  if (nfds > (uint32_t)FD_MAX) nfds = (uint32_t)FD_MAX;
  if (nfds > 0) {
    if (fds_ptr == 0u) return -(long)EINVAL;
    rc = sys_copy_from_user(local_fds, fds_ptr, nfds * sizeof(local_fds[0]));
    if (rc < 0) return (long)rc;
  }
  if (has_timeout) {
    rc = sys_copy_from_user(timeout, timeout_ptr, sizeof(timeout));
    if (rc < 0) return (long)rc;
    ticks = ts64_to_ticks(timeout);
  }

  rc = (int)do_ppoll(local_fds, nfds, ticks, has_timeout);
  if (nfds > 0) {
    int copy_rc =
        sys_copy_to_user(fds_ptr, local_fds, nfds * sizeof(local_fds[0]));
    if (copy_rc < 0) return (long)copy_rc;
  }
  return (long)rc;
}
