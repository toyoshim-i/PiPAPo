/*
 * sys_io.c — I/O syscall implementations
 *
 *   sys_write(fd, buf, n) — write n bytes from buf to file descriptor fd
 *   sys_read (fd, buf, n) — read  up to n bytes from fd into buf
 *
 * Core resolves per-process fd → descriptor ID via fd_map[],
 * then delegates to mod_vfs.fd_read/fd_write (VFS module).
 */

#include <stddef.h>

#include "../common/errno.h"
#include "../common/mod/mod_vfs.h"
#include "../proc/proc.h"
#include "config.h"
#include "syscall.h"

/* ── sys_write ────────────────────────────────────────────────────────────────
 */

long sys_write(long fd, const char *buf, size_t n) {
  if (fd < 0 || (uint32_t)fd >= FD_MAX) return -(long)EBADF;
  int16_t desc = current->fd_map[(uint32_t)fd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;
  return mod_vfs.fd_write(desc, buf, n);
}

/* ── sys_read ─────────────────────────────────────────────────────────────────
 */

long sys_read(long fd, char *buf, size_t n) {
  if (fd < 0 || (uint32_t)fd >= FD_MAX) return -(long)EBADF;
  int16_t desc = current->fd_map[(uint32_t)fd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;
  return mod_vfs.fd_read(desc, buf, n);
}

/* ── sys_writev ───────────────────────────────────────────────────────────────
 */

#include "common/iovec.h"

long sys_writev(long fd, const void *iov_ptr, long iovcnt) {
  if (iovcnt <= 0 || iovcnt > 1024) return -(long)EINVAL;

  const struct iovec *iov = (const struct iovec *)iov_ptr;
  long total = 0;

  for (long i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) continue;
    long n = sys_write(fd, (const char *)iov[i].iov_base, iov[i].iov_len);
    if (n < 0) {
      if (total > 0) return total;
      return n;
    }
    total += n;
    if ((size_t)n < iov[i].iov_len) break; /* short write */
  }
  return total;
}

/* ── sys_readv ────────────────────────────────────────────────────────────────
 */

long sys_readv(long fd, const void *iov_ptr, long iovcnt) {
  if (iovcnt <= 0 || iovcnt > 1024) return -(long)EINVAL;

  const struct iovec *iov = (const struct iovec *)iov_ptr;
  long total = 0;

  for (long i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) continue;
    long n = sys_read(fd, (char *)iov[i].iov_base, iov[i].iov_len);
    if (n < 0) {
      if (total > 0) return total;
      return n;
    }
    total += n;
    if (n == 0 || (size_t)n < iov[i].iov_len) break; /* EOF or short read */
  }
  return total;
}

/* ── sys_ioctl ────────────────────────────────────────────────────────────────
 */

long sys_ioctl(long fd, long cmd, long arg) {
  if (fd < 0 || (uint32_t)fd >= FD_MAX) return -(long)EBADF;
  int16_t desc = current->fd_map[(uint32_t)fd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;
  return (long)mod_vfs.fd_ioctl(desc, (uint32_t)cmd,
                                (void *)(uintptr_t)arg);
}
