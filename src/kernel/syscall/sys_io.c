/*
 * sys_io.c — I/O syscall implementations
 *
 *   sys_write(fd, buf, n) — write n bytes from buf to file descriptor fd
 *   sys_read (fd, buf, n) — read  up to n bytes from fd into buf
 *
 * Both syscalls dispatch through the fd table in the current process's PCB.
 * The fd table is populated with the tty driver (Step 10 — fd_stdio_init).
 * Until then, all fds are NULL and calls return -EBADF.
 */

#include "syscall.h"
#include "../proc/proc.h"
#include "../fd/file.h"
#include "../errno.h"
#include <stddef.h>

/* ── sys_write ──────────────────────────────────────────────────────────────── */

long sys_write(long fd, const char *buf, size_t n)
{
    if (fd < 0 || (uint32_t)fd >= FD_MAX)
        return -(long)EBADF;

    struct file *f = current->fd_table[(uint32_t)fd];
    if (!f || !f->ops || !f->ops->write)
        return -(long)EBADF;

    return f->ops->write(f, buf, n);
}

/* ── sys_read ───────────────────────────────────────────────────────────────── */

long sys_read(long fd, char *buf, size_t n)
{
    if (fd < 0 || (uint32_t)fd >= FD_MAX)
        return -(long)EBADF;

    struct file *f = current->fd_table[(uint32_t)fd];
    if (!f || !f->ops || !f->ops->read)
        return -(long)EBADF;

    return f->ops->read(f, buf, n);
}
