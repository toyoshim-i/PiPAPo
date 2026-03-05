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
 *   - Otherwise, block with svc_restart; the process is woken by either:
 *     (a) data arrival (e.g. tty_rx_notify → sched_wakeup)
 *     (b) timeout expiry (sched_tick checks PROC_BLOCKED + sleep_until)
 *     (c) signal delivery (returns -EINTR)
 */

#include "syscall.h"
#include "../proc/proc.h"
#include "../proc/sched.h"
#include "../fd/fd.h"
#include "../fd/file.h"
#include "../fd/tty.h"
#include "../errno.h"
#include "config.h"
#include <stdint.h>

/* Linux pollfd structure (matches musl ARM layout) */
struct pollfd {
    int   fd;
    short events;
    short revents;
};

/* Convert timespec to ticks.  Returns 0 for zero timeout, UINT32_MAX
 * for negative/overflow. */
static uint32_t ts32_to_ticks(const void *ts_ptr)
{
    const long *ts = (const long *)ts_ptr;
    if (ts[0] < 0)
        return 0;
    uint32_t ticks = (uint32_t)ts[0] * PPAP_TICK_HZ
                   + (uint32_t)ts[1] / (1000000000u / PPAP_TICK_HZ);
    return ticks;
}

static uint32_t ts64_to_ticks(const void *ts_ptr)
{
    const int64_t *ts = (const int64_t *)ts_ptr;
    if (ts[0] < 0)
        return 0;
    uint32_t ticks = (uint32_t)ts[0] * PPAP_TICK_HZ
                   + (uint32_t)ts[1] / (1000000000u / PPAP_TICK_HZ);
    return ticks;
}

/* Core poll logic shared by ppoll and ppoll_time64. */
static long do_ppoll(struct pollfd *fds, uint32_t nfds,
                     uint32_t timeout_ticks, int has_timeout)
{
    if (!fds && nfds > 0)
        return -(long)EINVAL;
    if (nfds > (uint32_t)FD_MAX)
        nfds = (uint32_t)FD_MAX;

    /* Check all fds for ready events */
    int ready = 0;
    for (uint32_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        int fd = fds[i].fd;
        if (fd < 0)
            continue;   /* negative fd → skip (Linux behaviour) */

        if (fd >= FD_MAX || !current->fd_table[fd]) {
            fds[i].revents = (short)POLLNVAL;
            ready++;
            continue;
        }

        struct file *f = current->fd_table[fd];
        int mask;
        if (f->ops && f->ops->poll)
            mask = f->ops->poll(f);
        else
            mask = (int)(POLLIN | POLLOUT);  /* no poll → assume ready */

        fds[i].revents = (short)(mask & (int)fds[i].events);
        if (fds[i].revents)
            ready++;
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

    /* Check if we've timed out (on svc_restart re-entry) */
    if (current->sleep_until != 0 &&
        (int32_t)(sched_get_ticks() - current->sleep_until) >= 0) {
        current->sleep_until = 0;
        return 0;   /* timeout */
    }

    /* Check for pending signals */
    if (current->sig_pending & ~current->sig_blocked) {
        current->sleep_until = 0;
        return -(long)EINTR;
    }

    /* Set up the deadline on first entry (sleep_until still 0) */
    if (has_timeout && current->sleep_until == 0)
        current->sleep_until = sched_get_ticks() + timeout_ticks;

    /* Block: wait for data or timeout.
     * Use &tty_stdin as wait channel — handles the common case of
     * polling the tty for keyboard input (top, vi). */
    current->wait_channel = &tty_stdin;
    current->state = PROC_BLOCKED;
    svc_restart[core_id()] = 1;
    sched_yield();
    return 0;   /* ignored — SVC restores original args */
}

/* ── sys_ppoll (SYS_PPOLL = 336) ──────────────────────────────────────────── */
/*
 * ppoll(fds, nfds, timeout_ts, sigmask, sigsetsize)
 * timeout_ts: 32-bit struct timespec { long tv_sec; long tv_nsec; }
 */
long sys_ppoll(void *fds, uint32_t nfds, const void *timeout,
               const void *sigmask, uint32_t sigsetsize)
{
    (void)sigmask; (void)sigsetsize;

    int has_timeout = (timeout != NULL);
    uint32_t ticks = 0;
    if (has_timeout)
        ticks = ts32_to_ticks(timeout);

    return do_ppoll((struct pollfd *)fds, nfds, ticks, has_timeout);
}

/* ── sys_ppoll_time64 (SYS_PPOLL_TIME64 = 414) ───────────────────────────── */
/*
 * ppoll_time64(fds, nfds, timeout_ts64, sigmask, sigsetsize)
 * timeout_ts64: 64-bit struct timespec { int64_t tv_sec; int64_t tv_nsec; }
 */
long sys_ppoll_time64(void *fds, uint32_t nfds, const void *timeout,
                      const void *sigmask, uint32_t sigsetsize)
{
    (void)sigmask; (void)sigsetsize;

    int has_timeout = (timeout != NULL);
    uint32_t ticks = 0;
    if (has_timeout)
        ticks = ts64_to_ticks(timeout);

    return do_ppoll((struct pollfd *)fds, nfds, ticks, has_timeout);
}
