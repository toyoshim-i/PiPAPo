/*
 * pipe.c — Kernel pipe implementation
 *
 * A pipe is a unidirectional byte stream backed by an SRAM ring buffer.
 * Two struct file objects (read end + write end) share a single pipe_t.
 *
 * Blocking: when the buffer is empty (reader) or full (writer), the
 * calling process is marked PROC_BLOCKED with wait_channel pointing at
 * the pipe_t.  The svc_restart mechanism (svc.S) causes the SVC to
 * re-execute with original arguments when the process is woken.
 *
 * Wake-up: pipe_write wakes blocked readers after adding data;
 * pipe_read wakes blocked writers after consuming data; pipe_close
 * wakes the other end so it can detect EOF / EPIPE.
 */

#include <stdint.h>
#include <string.h>

#include "../common/errno.h"
#include "../common/mod/mod_core.h"
#include "../proc/proc.h"
#include "fd.h"
#include "file.h"
#include "config.h"

/* Forward declarations for pool operations (fd.c) */
extern void vfs_fd_release(int desc);

/* ── Pipe configuration ─────────────────────────────────────────────────────
 */

#define PIPE_BUF_SIZE 512u /* power of 2 for cheap modulo via & mask */
#define PIPE_MASK (PIPE_BUF_SIZE - 1u)
#define PIPE_MAX 4 /* max concurrent pipes                   */

/* ── Pipe structure ─────────────────────────────────────────────────────────
 */

typedef struct {
  uint8_t buf[PIPE_BUF_SIZE];
  uint16_t head;   /* write position (producer advances) */
  uint16_t tail;   /* read position  (consumer advances) */
  uint8_t readers; /* number of open read ends  */
  uint8_t writers; /* number of open write ends */
  uint8_t in_use;  /* 1 = allocated, 0 = free   */
} pipe_t;

static pipe_t pipe_pool[PIPE_MAX]; /* ~2 KB in BSS */

/* ── Pool helpers ───────────────────────────────────────────────────────────
 */

static pipe_t *pipe_alloc(void) {
  for (int i = 0; i < PIPE_MAX; i++) {
    if (!pipe_pool[i].in_use) {
      pipe_t *p = &pipe_pool[i];
      memset(p, 0, sizeof(*p));
      p->in_use = 1;
      p->readers = 1;
      p->writers = 1;
      return p;
    }
  }
  return NULL;
}

static void pipe_free(pipe_t *p) { p->in_use = 0; }

static uint16_t pipe_used(pipe_t *p) {
  return (uint16_t)((p->head - p->tail) & PIPE_MASK);
}

static uint16_t pipe_space(pipe_t *p) {
  /* Keep 1-byte gap to distinguish full from empty */
  return (uint16_t)(PIPE_BUF_SIZE - 1u - pipe_used(p));
}

/* ── File operations ────────────────────────────────────────────────────────
 */

static long pipe_read(struct file *f, char *buf, size_t n) {
  pipe_t *p = f->priv;
  uint16_t avail = pipe_used(p);

  if (avail > 0) {
    /* Copy min(avail, n) bytes from ring buffer */
    size_t count = (n < avail) ? n : avail;
    for (size_t i = 0; i < count; i++) {
      buf[i] = (char)p->buf[p->tail];
      p->tail = (uint16_t)((p->tail + 1u) & PIPE_MASK);
    }
    /* Wake any blocked writers — we freed space */
    mod_core.sched_wakeup(p);
    return (long)count;
  }

  /* Buffer empty */
  if (p->writers == 0) return 0; /* EOF — no writers left */

  /* Block: wait for data */
  current->wait_channel = p;
  current->state = PROC_BLOCKED;
  mod_core.set_svc_restart();
  mod_core.sched_yield();
  return 0; /* ignored — SVC_Handler restores original frame[0] */
}

static long pipe_write(struct file *f, const char *buf, size_t n) {
  pipe_t *p = f->priv;

  if (p->readers == 0) return -(long)EPIPE; /* broken pipe — no readers */

  uint16_t space = pipe_space(p);

  if (space > 0) {
    /* Copy min(space, n) bytes into ring buffer */
    size_t count = (n < space) ? n : space;
    for (size_t i = 0; i < count; i++) {
      p->buf[p->head] = (uint8_t)buf[i];
      p->head = (uint16_t)((p->head + 1u) & PIPE_MASK);
    }
    /* Wake any blocked readers — we added data */
    mod_core.sched_wakeup(p);
    return (long)count;
  }

  /* Buffer full — block: wait for space */
  current->wait_channel = p;
  current->state = PROC_BLOCKED;
  mod_core.set_svc_restart();
  mod_core.sched_yield();
  return 0; /* ignored — SVC_Handler restores original frame[0] */
}

static int pipe_close(struct file *f) {
  pipe_t *p = f->priv;

  if (f->flags == O_RDONLY)
    p->readers--;
  else
    p->writers--;

  /* Wake the other end so it can detect EOF / EPIPE */
  mod_core.sched_wakeup(p);

  if (p->readers == 0 && p->writers == 0) pipe_free(p);

  /* Note: file_free is called by fd_free when refcnt reaches 0.
   * Do NOT call file_free here — that would be a double-free. */
  return 0;
}

/* ── File ops vtables ───────────────────────────────────────────────────────
 */

static const struct file_ops pipe_read_ops = {pipe_read, NULL, pipe_close, NULL,
                                              NULL};
static const struct file_ops pipe_write_ops = {NULL, pipe_write, pipe_close,
                                               NULL, NULL};

/* ── vfs_fd_pipe_create ─────────────────────────────────────────────────────
 *
 * Allocate a pipe and two pool entries for read/write ends.
 * Returns 0 on success (desc IDs in *rdesc, *wdesc), negative errno on error.
 * Called via mod_vfs.fd_pipe_create().
 */

int vfs_fd_pipe_create(int *rdesc, int *wdesc) {
  if (!rdesc || !wdesc) return -EINVAL;

  pipe_t *p = pipe_alloc();
  if (!p) return -ENOMEM;

  int rd = fd_pool_alloc_pipe(&pipe_read_ops, p, O_RDONLY);
  if (rd < 0) {
    pipe_free(p);
    return rd;
  }

  int wd = fd_pool_alloc_pipe(&pipe_write_ops, p, O_WRONLY);
  if (wd < 0) {
    vfs_fd_release(rd);
    pipe_free(p);
    return wd;
  }

  *rdesc = rd;
  *wdesc = wd;
  return 0;
}

/* ── sys_pipe (core-callable wrapper) ──────────────────────────────────────
 *
 * On non-i16 platforms, syscall dispatch calls this directly.
 * On i16, it goes through mod_vfs.fd_pipe_create + core fd_map assignment.
 */

long sys_pipe(int *fds) {
  if (!fds) return -(long)EINVAL;

  int rdesc, wdesc;
  int err = vfs_fd_pipe_create(&rdesc, &wdesc);
  if (err) return (long)err;

  /* Find two free fd_map slots */
  int rfd = -1, wfd = -1;
  for (int i = 0; i < FD_MAX && (rfd < 0 || wfd < 0); i++) {
    if (current->fd_map[i] == FD_DESC_NONE) {
      if (rfd < 0) rfd = i;
      else wfd = i;
    }
  }

  if (rfd < 0 || wfd < 0) {
    vfs_fd_release(rdesc);
    vfs_fd_release(wdesc);
    return -(long)EMFILE;
  }

  current->fd_map[rfd] = (int16_t)rdesc;
  current->fd_map[wfd] = (int16_t)wdesc;
  fds[0] = rfd;
  fds[1] = wfd;
  return 0;
}
