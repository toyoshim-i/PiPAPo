/*
 * fd.c — File descriptor table helpers
 *
 * See fd.h for the public API.
 */

#include "fd.h"
#include "file.h"
#include "tty.h"
#include "../errno.h"

/* ── fd_stdio_init ──────────────────────────────────────────────────────────── */

void fd_stdio_init(pcb_t *p)
{
    /* Point static tty files at the default console (UART / ttyS0) */
    tty_stdin.priv  = tty_get_dev(TTY_SERIAL);
    tty_stdout.priv = tty_get_dev(TTY_SERIAL);
    tty_stderr.priv = tty_get_dev(TTY_SERIAL);
    p->fd_table[0] = &tty_stdin;    /* stdin  — read from console  */
    p->fd_table[1] = &tty_stdout;   /* stdout — write to console   */
    p->fd_table[2] = &tty_stderr;   /* stderr — write to console   */
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
    if (f->refcnt == 0u) {
        if (f->ops && f->ops->close)
            f->ops->close(f);
        file_free(f);   /* return to pool (no-op for static tty files) */
    }
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
