/*
 * fd.c — File descriptor table helpers
 *
 * See fd.h for the public API.
 */

#include "fd.h"
#include "tty.h"
#include "../errno.h"

/* ── fd_stdio_init ──────────────────────────────────────────────────────────── */

void fd_stdio_init(pcb_t *p)
{
    p->fd_table[0] = &tty_stdin;    /* stdin  — read from UART  */
    p->fd_table[1] = &tty_stdout;   /* stdout — write to UART   */
    p->fd_table[2] = &tty_stderr;   /* stderr — write to UART   */
}

/* ── fd_alloc ───────────────────────────────────────────────────────────────── */

int fd_alloc(pcb_t *p, struct file *f)
{
    for (int i = 0; i < FD_MAX; i++) {
        if (!p->fd_table[i]) {
            p->fd_table[i] = f;
            f->refcnt++;
            return i;
        }
    }
    return -EMFILE;
}

/* ── fd_free ────────────────────────────────────────────────────────────────── */

void fd_free(pcb_t *p, int fd)
{
    if (fd < 0 || fd >= FD_MAX)
        return;
    struct file *f = p->fd_table[fd];
    if (!f)
        return;
    p->fd_table[fd] = NULL;
    if (f->refcnt > 0u)
        f->refcnt--;
    if (f->refcnt == 0u && f->ops && f->ops->close)
        f->ops->close(f);
}

/* ── fd_get ─────────────────────────────────────────────────────────────────── */

struct file *fd_get(pcb_t *p, int fd)
{
    if (fd < 0 || fd >= FD_MAX)
        return NULL;
    return p->fd_table[fd];
}

/* ── fd_inherit ────────────────────────────────────────────────────────────── */

void fd_inherit(pcb_t *child, const pcb_t *parent)
{
    for (int i = 0; i < FD_MAX; i++) {
        struct file *f = parent->fd_table[i];
        child->fd_table[i] = f;
        if (f)
            f->refcnt++;
    }
}

/* ── fd_close_all ──────────────────────────────────────────────────────────── */

void fd_close_all(pcb_t *p)
{
    for (int i = 0; i < FD_MAX; i++)
        fd_free(p, i);
}
