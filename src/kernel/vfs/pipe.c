/*
 * pipe.c — Kernel pipe implementation
 *
 * A pipe is a unidirectional byte stream backed by an SRAM ring buffer.
 * Two struct file objects (read end + write end) share a single pipe_t.
 *
 * Blocking: when the buffer is empty (reader) or full (writer), the
 * calling process is marked PROC_BLOCKED with wait_channel pointing at
 * the pipe_t.  pipe_read/pipe_write loop internally across sched_switch
 * until data/space is available, a signal arrives, or O_NONBLOCK makes
 * us return -EAGAIN.  No svc_restart IP-rewind is needed — the syscall
 * completes with a real status in one call.
 *
 * Wake-up: pipe_write wakes blocked readers after adding data;
 * pipe_read wakes blocked writers after consuming data; pipe_close
 * wakes the other end so it can detect EOF / EPIPE.
 */

#include <stdint.h>
#include <string.h>

#include "common/errno.h"
#include "common/fcntl.h"
#include "kernel/common/config.h"
#include "kernel/common/core/proc_info.h"
#include "kernel/common/mod/mod_core.h"
#include "kernel/vfs/fd.h"
#include "kernel/vfs/file.h"

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

static void pipe_advance(page_id_t *page, uint16_t *off) {
  (*off)++;
  if (*off == PAGE_SIZE) {
    *off = 0;
    (*page)++;
  }
}

static long pipe_read(struct file *f, page_id_t page, uint16_t off, size_t n) {
  pipe_t *p = f->priv;
  int nonblock = (f->flags & O_NONBLOCK) ? 1 : 0;

  /* Wait for data, then drain one batch.  Loop internally across
   * sched_switch so the syscall returns a real status. */
  for (;;) {
    uint16_t avail = pipe_used(p);
    if (avail > 0) {
      size_t count = (n < avail) ? n : avail;
      for (size_t i = 0; i < count; i++) {
        char ch = (char)p->buf[p->tail];
        mod_core.mem_region_page_write(page, off, &ch, 1);
        pipe_advance(&page, &off);
        p->tail = (uint16_t)((p->tail + 1u) & PIPE_MASK);
      }
      mod_core.sched_wakeup(p); /* wake blocked writers */
      return (long)count;
    }
    if (p->writers == 0) return 0; /* EOF — no writers left */
    if (nonblock) return -(long)EAGAIN;
    if (current->sig_pending & ~current->sig_blocked) return -(long)EINTR;
    current->wait_channel = p;
    current->state = PROC_BLOCKED;
    mod_core.sched_switch();
  }
}

static long pipe_write(struct file *f, page_id_t page, uint16_t off, size_t n) {
  pipe_t *p = f->priv;
  int nonblock = (f->flags & O_NONBLOCK) ? 1 : 0;

  /* Wait for space, then push one batch.  Loop internally across
   * sched_switch so the syscall returns a real status. */
  for (;;) {
    if (p->readers == 0) return -(long)EPIPE; /* broken pipe */
    uint16_t space = pipe_space(p);
    if (space > 0) {
      size_t count = (n < space) ? n : space;
      for (size_t i = 0; i < count; i++) {
        uint8_t ch;
        mod_core.mem_region_page_read(page, off, &ch, 1);
        pipe_advance(&page, &off);
        p->buf[p->head] = ch;
        p->head = (uint16_t)((p->head + 1u) & PIPE_MASK);
      }
      mod_core.sched_wakeup(p); /* wake blocked readers */
      return (long)count;
    }
    if (nonblock) return -(long)EAGAIN;
    if (current->sig_pending & ~current->sig_blocked) return -(long)EINTR;
    current->wait_channel = p;
    current->state = PROC_BLOCKED;
    mod_core.sched_switch();
  }
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
