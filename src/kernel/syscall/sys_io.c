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

/* ── sys_writev ─────────────────────────────────────────────────────────────── */

/* struct iovec layout (matches musl/Linux ARM) */
struct iovec {
    void   *iov_base;
    size_t  iov_len;
};

long sys_writev(long fd, const void *iov_ptr, long iovcnt)
{
    if (iovcnt <= 0 || iovcnt > 1024)
        return -(long)EINVAL;

    const struct iovec *iov = (const struct iovec *)iov_ptr;
    long total = 0;

    for (long i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0)
            continue;
        long n = sys_write(fd, (const char *)iov[i].iov_base, iov[i].iov_len);
        if (n < 0) {
            if (total > 0)
                return total;
            return n;
        }
        total += n;
        if ((size_t)n < iov[i].iov_len)
            break;   /* short write */
    }
    return total;
}

/* ── sys_readv ──────────────────────────────────────────────────────────────── */

long sys_readv(long fd, const void *iov_ptr, long iovcnt)
{
    if (iovcnt <= 0 || iovcnt > 1024)
        return -(long)EINVAL;

    const struct iovec *iov = (const struct iovec *)iov_ptr;
    long total = 0;

    for (long i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0)
            continue;
        long n = sys_read(fd, (char *)iov[i].iov_base, iov[i].iov_len);
        if (n < 0) {
            if (total > 0)
                return total;
            return n;
        }
        total += n;
        if (n == 0 || (size_t)n < iov[i].iov_len)
            break;   /* EOF or short read */
    }
    return total;
}

/* ── sys_ioctl ──────────────────────────────────────────────────────────────── */

long sys_ioctl(long fd, long cmd, long arg)
{
    if (fd < 0 || (uint32_t)fd >= FD_MAX)
        return -(long)EBADF;

    struct file *f = current->fd_table[(uint32_t)fd];
    if (!f || !f->ops)
        return -(long)EBADF;

    if (!f->ops->ioctl)
        return -(long)ENOTTY;

    return (long)f->ops->ioctl(f, (uint32_t)cmd, (void *)(uintptr_t)arg);
}
