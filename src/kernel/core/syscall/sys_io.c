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

#include "common/errno.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/proc/proc.h"
#include "kernel/common/config.h"
#include "syscall.h"

#define TCGETS 0x5401u
#define TCSETS 0x5402u
#define TCSETSW 0x5403u
#define TCSETSF 0x5404u
#define TIOCSCTTY 0x540Eu
#define TIOCGPGRP 0x540Fu
#define TIOCSPGRP 0x5410u
#define TIOCGWINSZ 0x5413u
#define TIOCSWINSZ 0x5414u
#define TIOCGSID 0x5429u

struct syscall_kernel_termios {
  uint32_t c_iflag;
  uint32_t c_oflag;
  uint32_t c_cflag;
  uint32_t c_lflag;
  uint8_t c_line;
  uint8_t c_cc[19];
};

struct syscall_winsize {
  uint16_t ws_row;
  uint16_t ws_col;
  uint16_t ws_xpixel;
  uint16_t ws_ypixel;
};

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

int sys_copy_from_user(void *dst, uintptr_t user_ptr, size_t len) {
  user_page_ref_t ref;
  int rc = proc_user_ptr_to_page_ref(current, user_ptr, &ref);

  if (rc < 0) return rc;
  sys_io_copy_from_user_ref(dst, &ref, len);
  return 0;
}

int sys_copy_to_user(uintptr_t user_ptr, const void *src, size_t len) {
  user_page_ref_t ref;
  const uint8_t *in = (const uint8_t *)src;
  int rc = proc_user_ptr_to_page_ref(current, user_ptr, &ref);

  if (rc < 0) return rc;
  while (len > 0) {
    size_t chunk = PAGE_SIZE - ref.off;

    if (chunk > len) chunk = len;
    mem_region_page_write(ref.page, ref.off, in, (uint16_t)chunk);
    in += chunk;
    len -= chunk;
    sys_io_advance_ref(&ref, chunk);
  }
  return 0;
}

int sys_copy_user_string(char *dst, size_t dst_size, uintptr_t user_ptr) {
  user_page_ref_t ref;
  int rc;

  if (!dst || dst_size == 0) return -(long)EINVAL;
  rc = proc_user_ptr_to_page_ref(current, user_ptr, &ref);
  if (rc < 0) return rc;

  for (size_t i = 0; i < dst_size; i++) {
    mem_region_page_read(ref.page, ref.off, &dst[i], 1);
    if (dst[i] == '\0') return 0;
    sys_io_advance_ref(&ref, 1);
  }
  dst[dst_size - 1] = '\0';
  return -(long)ENAMETOOLONG;
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

  rc = proc_user_ptr_to_page_ref(current, user_ptr, &ref);
  if (rc < 0) return rc;
  return mod_vfs.fd_write((int)desc, ref.page, ref.off, n);
}

long sys_read(long fd, uintptr_t user_ptr, size_t n) {
  user_page_ref_t ref;
  long desc = sys_io_lookup_desc(fd);
  int rc;

  if (desc < 0) return desc;

  rc = proc_user_ptr_to_page_ref(current, user_ptr, &ref);
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
    int rc = sys_copy_from_user(&iov,
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
    int rc = sys_copy_from_user(&iov,
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

long sys_ioctl(long fd, long cmd, uintptr_t arg_ptr) {
  if (fd < 0 || (uint32_t)fd >= FD_MAX) return -(long)EBADF;
  int16_t desc = current->fd_map[(uint32_t)fd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;

  switch ((uint32_t)cmd) {
    case TCGETS: {
      struct syscall_kernel_termios tio;
      int rc;
      if (arg_ptr == 0u) return -(long)EINVAL;
      rc = mod_vfs.fd_ioctl(desc, (uint32_t)cmd, &tio);
      if (rc < 0) return (long)rc;
      rc = sys_copy_to_user(arg_ptr, &tio, sizeof(tio));
      if (rc < 0) return (long)rc;
      return 0;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
      struct syscall_kernel_termios tio;
      int rc;
      if (arg_ptr == 0u) return -(long)EINVAL;
      rc = sys_copy_from_user(&tio, arg_ptr, sizeof(tio));
      if (rc < 0) return (long)rc;
      return (long)mod_vfs.fd_ioctl(desc, (uint32_t)cmd, &tio);
    }
    case TIOCGWINSZ: {
      struct syscall_winsize ws;
      int rc;
      if (arg_ptr == 0u) return -(long)EINVAL;
      rc = mod_vfs.fd_ioctl(desc, (uint32_t)cmd, &ws);
      if (rc < 0) return (long)rc;
      rc = sys_copy_to_user(arg_ptr, &ws, sizeof(ws));
      if (rc < 0) return (long)rc;
      return 0;
    }
    case TIOCSWINSZ: {
      struct syscall_winsize ws;
      int rc;
      if (arg_ptr == 0u) return -(long)EINVAL;
      rc = sys_copy_from_user(&ws, arg_ptr, sizeof(ws));
      if (rc < 0) return (long)rc;
      return (long)mod_vfs.fd_ioctl(desc, (uint32_t)cmd, &ws);
    }
    case TIOCGPGRP:
    case TIOCGSID: {
      int32_t value;
      int rc;
      if (arg_ptr == 0u) return -(long)EINVAL;
      rc = mod_vfs.fd_ioctl(desc, (uint32_t)cmd, &value);
      if (rc < 0) return (long)rc;
      rc = sys_copy_to_user(arg_ptr, &value, sizeof(value));
      if (rc < 0) return (long)rc;
      return 0;
    }
    case TIOCSPGRP: {
      int32_t value;
      int rc;
      if (arg_ptr == 0u) return -(long)EINVAL;
      rc = sys_copy_from_user(&value, arg_ptr, sizeof(value));
      if (rc < 0) return (long)rc;
      return (long)mod_vfs.fd_ioctl(desc, (uint32_t)cmd, &value);
    }
    case TIOCSCTTY:
      return (long)mod_vfs.fd_ioctl(desc, (uint32_t)cmd, NULL);
    default:
      /* Unknown ioctl payloads are not decoded in the syscall layer yet. */
      return (long)mod_vfs.fd_ioctl(desc, (uint32_t)cmd, NULL);
  }
}
