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
#include <stdint.h>

#include "arch/arch.h"
#include "../common/errno.h"
#include "../common/mod/mod_vfs.h"
#include "../mm/mem_region.h"
#include "../proc/proc.h"
#include "config.h"
#include "syscall.h"

static int sys_io_user_ref_from_ptr(uintptr_t user_ptr,
                                    user_page_ref_t *ref) {
  page_id_t base_page = proc_page_backed_base(current);

  if (base_page == PAGE_ID_INVALID) return -(long)EFAULT;
  ref->page = arch_user_ptr_to_page(base_page, user_ptr, &ref->off);
  if (ref->page == PAGE_ID_INVALID) return -(long)EFAULT;
  return 0;
}

static void sys_io_advance_ref(user_page_ref_t *ref, size_t delta) {
  size_t pos = (size_t)ref->off + delta;

  ref->page += (page_id_t)(pos / PAGE_SIZE);
  ref->off = (uint16_t)(pos % PAGE_SIZE);
}

static void sys_io_copy_from_user_ref(void *dst, user_page_ref_t *ref,
                                      size_t len) {
  uint8_t *out = (uint8_t *)dst;

  while (len > 0) {
    size_t chunk = PAGE_SIZE - ref->off;

    if (chunk > len) chunk = len;
    mem_region_page_read(ref->page, ref->off, out, (uint16_t)chunk);
    out += chunk;
    len -= chunk;
    sys_io_advance_ref(ref, chunk);
  }
}

static int sys_io_copy_from_user(void *dst, uintptr_t user_ptr, size_t len) {
  user_page_ref_t ref;
  int rc = sys_io_user_ref_from_ptr(user_ptr, &ref);

  if (rc < 0) return rc;
  sys_io_copy_from_user_ref(dst, &ref, len);
  return 0;
}

static long sys_io_lookup_desc(long fd) {
  if (fd < 0 || (uint32_t)fd >= FD_MAX) return -(long)EBADF;
  if (current->fd_map[(uint32_t)fd] == FD_DESC_NONE) return -(long)EBADF;
  return current->fd_map[(uint32_t)fd];
}

long sys_write(long fd, uintptr_t user_ptr, size_t n) {
  user_page_ref_t ref;
  long desc = sys_io_lookup_desc(fd);
  int rc;

  if (desc < 0) return desc;

  rc = sys_io_user_ref_from_ptr(user_ptr, &ref);
  if (rc < 0) return rc;
  return mod_vfs.fd_write((int)desc, ref.page, ref.off, n);
}

long sys_read(long fd, uintptr_t user_ptr, size_t n) {
  user_page_ref_t ref;
  long desc = sys_io_lookup_desc(fd);
  int rc;

  if (desc < 0) return desc;

  rc = sys_io_user_ref_from_ptr(user_ptr, &ref);
  if (rc < 0) return rc;
  return mod_vfs.fd_read((int)desc, ref.page, ref.off, n);
}

/* ── sys_writev ───────────────────────────────────────────────────────────────
 */

#include "common/iovec.h"

long sys_writev(long fd, uintptr_t iov_ptr, long iovcnt) {
  if (iovcnt <= 0 || iovcnt > 1024) return -(long)EINVAL;

  long total = 0;

  for (long i = 0; i < iovcnt; i++) {
    struct iovec iov;
    int rc = sys_io_copy_from_user(&iov,
                                   iov_ptr + (uintptr_t)(i * sizeof(iov)),
                                   sizeof(iov));
    long n;

    if (rc < 0) return total > 0 ? total : rc;
    if (iov.iov_len == 0) continue;
    n = sys_write(fd, (uintptr_t)iov.iov_base, iov.iov_len);
    if (n < 0) {
      if (total > 0) return total;
      return n;
    }
    total += n;
    if ((size_t)n < iov.iov_len) break; /* short write */
  }
  return total;
}

/* ── sys_readv ────────────────────────────────────────────────────────────────
 */

long sys_readv(long fd, uintptr_t iov_ptr, long iovcnt) {
  if (iovcnt <= 0 || iovcnt > 1024) return -(long)EINVAL;

  long total = 0;

  for (long i = 0; i < iovcnt; i++) {
    struct iovec iov;
    int rc = sys_io_copy_from_user(&iov,
                                   iov_ptr + (uintptr_t)(i * sizeof(iov)),
                                   sizeof(iov));
    long n;

    if (rc < 0) return total > 0 ? total : rc;
    if (iov.iov_len == 0) continue;
    n = sys_read(fd, (uintptr_t)iov.iov_base, iov.iov_len);
    if (n < 0) {
      if (total > 0) return total;
      return n;
    }
    total += n;
    if (n == 0 || (size_t)n < iov.iov_len) break; /* EOF or short read */
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
