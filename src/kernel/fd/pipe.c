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

#include "file.h"
#include "fd.h"
#include "../proc/proc.h"
#include "../proc/sched.h"
#include "../syscall/syscall.h"
#include "../errno.h"
#include <stdint.h>
#include <string.h>

/* ── Pipe configuration ───────────────────────────────────────────────────── */

#define PIPE_BUF_SIZE  512u   /* power of 2 for cheap modulo via & mask */
#define PIPE_MASK      (PIPE_BUF_SIZE - 1u)
#define PIPE_MAX         4    /* max concurrent pipes                   */

/* ── Pipe structure ───────────────────────────────────────────────────────── */

typedef struct {
    uint8_t   buf[PIPE_BUF_SIZE];
    uint16_t  head;       /* write position (producer advances) */
    uint16_t  tail;       /* read position  (consumer advances) */
    uint8_t   readers;    /* number of open read ends  */
    uint8_t   writers;    /* number of open write ends */
    uint8_t   in_use;     /* 1 = allocated, 0 = free   */
} pipe_t;

static pipe_t pipe_pool[PIPE_MAX];   /* ~2 KB in BSS */

/* ── Pool helpers ─────────────────────────────────────────────────────────── */

static pipe_t *pipe_alloc(void)
{
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!pipe_pool[i].in_use) {
            pipe_t *p = &pipe_pool[i];
            memset(p, 0, sizeof(*p));
            p->in_use  = 1;
            p->readers = 1;
            p->writers = 1;
            return p;
        }
    }
    return NULL;
}

static void pipe_free(pipe_t *p)
{
    p->in_use = 0;
}

static uint16_t pipe_used(pipe_t *p)
{
    return (uint16_t)((p->head - p->tail) & PIPE_MASK);
}

static uint16_t pipe_space(pipe_t *p)
{
    /* Keep 1-byte gap to distinguish full from empty */
    return (uint16_t)(PIPE_BUF_SIZE - 1u - pipe_used(p));
}

/* ── File operations ──────────────────────────────────────────────────────── */

static long pipe_read(struct file *f, char *buf, size_t n)
{
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
        sched_wakeup(p);
        return (long)count;
    }

    /* Buffer empty */
    if (p->writers == 0)
        return 0;   /* EOF — no writers left */

    /* Block: wait for data */
    current->wait_channel = p;
    current->state = PROC_BLOCKED;
    svc_restart = 1;
    sched_yield();
    return 0;   /* ignored — SVC_Handler restores original frame[0] */
}

static long pipe_write(struct file *f, const char *buf, size_t n)
{
    pipe_t *p = f->priv;

    if (p->readers == 0)
        return -(long)EPIPE;   /* broken pipe — no readers */

    uint16_t space = pipe_space(p);

    if (space > 0) {
        /* Copy min(space, n) bytes into ring buffer */
        size_t count = (n < space) ? n : space;
        for (size_t i = 0; i < count; i++) {
            p->buf[p->head] = (uint8_t)buf[i];
            p->head = (uint16_t)((p->head + 1u) & PIPE_MASK);
        }
        /* Wake any blocked readers — we added data */
        sched_wakeup(p);
        return (long)count;
    }

    /* Buffer full — block: wait for space */
    current->wait_channel = p;
    current->state = PROC_BLOCKED;
    svc_restart = 1;
    sched_yield();
    return 0;   /* ignored — SVC_Handler restores original frame[0] */
}

static int pipe_close(struct file *f)
{
    pipe_t *p = f->priv;

    if (f->flags == O_RDONLY)
        p->readers--;
    else
        p->writers--;

    /* Wake the other end so it can detect EOF / EPIPE */
    sched_wakeup(p);

    if (p->readers == 0 && p->writers == 0)
        pipe_free(p);

    file_free(f);
    return 0;
}

/* ── File ops vtables ─────────────────────────────────────────────────────── */

static const struct file_ops pipe_read_ops  = { pipe_read,  NULL,       pipe_close, NULL };
static const struct file_ops pipe_write_ops = { NULL,       pipe_write, pipe_close, NULL };

/* ── sys_pipe ─────────────────────────────────────────────────────────────── */

long sys_pipe(int *fds)
{
    if (!fds)
        return -(long)EINVAL;

    /* Allocate pipe */
    pipe_t *p = pipe_alloc();
    if (!p)
        return -(long)ENOMEM;

    /* Allocate read-end file */
    struct file *rf = file_alloc();
    if (!rf) {
        pipe_free(p);
        return -(long)ENOMEM;
    }
    rf->ops    = &pipe_read_ops;
    rf->priv   = p;
    rf->flags  = O_RDONLY;
    rf->refcnt = 0;
    rf->vnode  = NULL;
    rf->offset = 0;

    /* Allocate write-end file */
    struct file *wf = file_alloc();
    if (!wf) {
        file_free(rf);
        pipe_free(p);
        return -(long)ENOMEM;
    }
    wf->ops    = &pipe_write_ops;
    wf->priv   = p;
    wf->flags  = O_WRONLY;
    wf->refcnt = 0;
    wf->vnode  = NULL;
    wf->offset = 0;

    /* Allocate fds */
    int rfd = fd_alloc(current, rf);
    if (rfd < 0) {
        file_free(wf);
        file_free(rf);
        pipe_free(p);
        return (long)rfd;
    }

    int wfd = fd_alloc(current, wf);
    if (wfd < 0) {
        fd_free(current, rfd);
        file_free(wf);
        pipe_free(p);
        return (long)wfd;
    }

    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}
