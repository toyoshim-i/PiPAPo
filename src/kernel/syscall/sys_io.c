/*
 * sys_io.c — I/O syscall implementations
 *
 *   sys_write(fd, buf, n) — write n bytes from buf to file descriptor fd
 *   sys_read (fd, buf, n) — read  up to n bytes from fd into buf
 *
 * User buffers are accessed via copy_from_user / copy_to_user so that
 * VFS and file-ops layers receive kernel buffers only, never user
 * pointers.  This is required on i16 where user memory lives in a
 * different segment.
 */

#include <stddef.h>

#include "../common/errno.h"
#include "../common/mod/mod_vfs.h"
#include "../mm/uaccess.h"
#include "../proc/proc.h"
#include "config.h"
#include "syscall.h"

/* Bounce buffer size — must fit on the kernel stack (2 KB on i16). */
#define IO_BOUNCE_SIZE 128

/* ── sys_write ────────────────────────────────────────────────────────────────
 */

long sys_write(long fd, uint32_t user_buf, size_t n) {
  if (fd < 0 || (uint32_t)fd >= FD_MAX) return -(long)EBADF;
  int16_t desc = current->fd_map[(uint32_t)fd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;

  char bounce[IO_BOUNCE_SIZE];
  long total = 0;
  uint32_t addr = user_buf;
  size_t remaining = n;

  while (remaining > 0) {
    size_t chunk = remaining < IO_BOUNCE_SIZE ? remaining : IO_BOUNCE_SIZE;
    int err = copy_from_user(bounce, addr, chunk);
    if (err) return total > 0 ? total : -(long)EFAULT;

    long wrote = mod_vfs.fd_write(desc, bounce, chunk);
    if (wrote < 0) return total > 0 ? total : wrote;
    total += wrote;
    if ((size_t)wrote < chunk) break; /* short write */
    addr += (uint32_t)wrote;
    remaining -= (size_t)wrote;
  }
  return total;
}

/* ── sys_read ─────────────────────────────────────────────────────────────────
 */

long sys_read(long fd, uint32_t user_buf, size_t n) {
  if (fd < 0 || (uint32_t)fd >= FD_MAX) return -(long)EBADF;
  int16_t desc = current->fd_map[(uint32_t)fd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;

  char bounce[IO_BOUNCE_SIZE];
  long total = 0;
  uint32_t addr = user_buf;
  size_t remaining = n;

  while (remaining > 0) {
    size_t chunk = remaining < IO_BOUNCE_SIZE ? remaining : IO_BOUNCE_SIZE;
    long got = mod_vfs.fd_read(desc, bounce, chunk);
    if (got < 0) return total > 0 ? total : got;
    if (got == 0) break; /* EOF */

    int err = copy_to_user(addr, bounce, (size_t)got);
    if (err) return total > 0 ? total : -(long)EFAULT;
    total += got;
    if ((size_t)got < chunk) break; /* short read */
    addr += (uint32_t)got;
    remaining -= (size_t)got;
  }
  return total;
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
    long n = sys_write(fd, (uint32_t)(uintptr_t)iov[i].iov_base,
                       iov[i].iov_len);
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
    long n = sys_read(fd, (uint32_t)(uintptr_t)iov[i].iov_base,
                      iov[i].iov_len);
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
