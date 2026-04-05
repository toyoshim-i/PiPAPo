/*
 * sys_fs.c — Filesystem syscall implementations
 *
 * File descriptor syscalls resolve fd_map[fd] → descriptor ID, then
 * delegate to mod_vfs.fd_*(desc, ...) in the VFS module.
 *
 * Path-based syscalls (stat, mkdir, unlink, etc.) route through
 * mod_vfs.lookup / mod_vfs.lookup_parent as before.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "common/errno.h"
#include "kernel/common/config.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/proc/proc.h"
#include "kernel/core/syscall/syscall.h"

static int sys_copy_path(char *dst, uintptr_t path_ptr) {
  if (path_ptr == 0u) return -(long)EINVAL;
  return sys_copy_user_string(dst, VFS_PATH_MAX, path_ptr);
}

/* ── sys_open ────────────────────────────────────────────────────────────────
 */

long sys_open(uintptr_t path_ptr, long flags, long mode) {
  char path[VFS_PATH_MAX];
  int rc = sys_copy_path(path, path_ptr);

  if (rc < 0) return (long)rc;

  int desc = mod_vfs.fd_open(path, (int)flags, (int)mode);
  if (desc < 0) return (long)desc;

  /* Find free fd_map slot */
  for (int i = 0; i < FD_MAX; i++) {
    if (current->fd_map[i] == FD_DESC_NONE) {
      current->fd_map[i] = (int16_t)desc;
      return (long)i;
    }
  }

  mod_vfs.fd_release(desc);
  return -(long)EMFILE;
}

/* ── sys_close ───────────────────────────────────────────────────────────────
 */

long sys_close(long fd) {
  if (fd < 0 || (uint32_t)fd >= FD_MAX) return -(long)EBADF;
  int16_t desc = current->fd_map[(uint32_t)fd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;

  current->fd_map[(uint32_t)fd] = FD_DESC_NONE;
  mod_vfs.fd_release(desc);
  return 0;
}

/* ── sys_lseek ───────────────────────────────────────────────────────────────
 */

long sys_lseek(long fd, long off, long whence) {
  if (fd < 0 || (uint32_t)fd >= FD_MAX) return -(long)EBADF;
  int16_t desc = current->fd_map[(uint32_t)fd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;
  return mod_vfs.fd_lseek(desc, off, (int)whence);
}

/* ── sys_stat ────────────────────────────────────────────────────────────────
 */

long sys_stat(uintptr_t path_ptr, uintptr_t buf_ptr) {
  char path[VFS_PATH_MAX];
  struct stat st;
  int rc = sys_copy_path(path, path_ptr);

  if (rc < 0) return (long)rc;
  if (buf_ptr == 0u) return -(long)EINVAL;

  vnode_t *vn = NULL;
  int err = mod_vfs.lookup(path, &vn);
  if (err) return (long)err;

  err = mod_vfs.vnode_stat(vn, &st);
  mod_vfs.vnode_release(vn);
  if (err) return (long)err;
  rc = sys_copy_to_user(buf_ptr, &st, sizeof(st));
  if (rc < 0) return (long)rc;
  return 0;
}

/* ── sys_fstat ───────────────────────────────────────────────────────────────
 */

long sys_fstat(long fd, uintptr_t buf_ptr) {
  struct stat st;
  int rc;

  if (fd < 0 || (uint32_t)fd >= FD_MAX) return -(long)EBADF;
  if (buf_ptr == 0u) return -(long)EINVAL;
  int16_t desc = current->fd_map[(uint32_t)fd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;
  rc = mod_vfs.fd_fstat(desc, &st);
  if (rc) return (long)rc;
  rc = sys_copy_to_user(buf_ptr, &st, sizeof(st));
  if (rc < 0) return (long)rc;
  return 0;
}

/* ── sys_getdents ────────────────────────────────────────────────────────────
 */

long sys_getdents(long fd, uintptr_t buf_ptr, size_t count) {
  struct dirent entries[32];
  size_t tmp_count = count;
  int rc;

  if (fd < 0 || (uint32_t)fd >= FD_MAX) return -(long)EBADF;
  if (buf_ptr == 0u) return -(long)EINVAL;
  int16_t desc = current->fd_map[(uint32_t)fd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;
  if (tmp_count > sizeof(entries)) tmp_count = sizeof(entries);
  rc = (int)mod_vfs.fd_getdents(desc, entries, tmp_count);
  if (rc <= 0) return (long)rc;
  if (sys_copy_to_user(buf_ptr, entries, (size_t)rc * sizeof(entries[0])) < 0)
    return -(long)EFAULT;
  return (long)rc;
}

/* ── sys_getcwd ──────────────────────────────────────────────────────────────
 */

long sys_getcwd(uintptr_t buf_ptr, size_t size) {
  char out[VFS_PATH_MAX];
  size_t len;

  if (buf_ptr == 0u || size == 0) return -(long)EINVAL;

  const char *cwd = current->cwd;
  len = __builtin_strlen(cwd);

  if (len == 0) {
    if (size < 2) return -(long)ERANGE;
    out[0] = '/';
    out[1] = '\0';
    if (sys_copy_to_user(buf_ptr, out, 2) < 0) return -(long)EFAULT;
    return 2;
  }

  if (len + 1 > size) return -(long)ERANGE;
  __builtin_memcpy(out, cwd, len + 1);
  if (sys_copy_to_user(buf_ptr, out, len + 1) < 0) return -(long)EFAULT;
  return (long)(len + 1);
}

/* ── sys_chdir ───────────────────────────────────────────────────────────────
 */

long sys_chdir(uintptr_t path_ptr) {
  char path[VFS_PATH_MAX];
  int rc = sys_copy_path(path, path_ptr);
  const char *target_path = path;

  if (rc < 0) return (long)rc;

  vnode_t *vn = NULL;
  int err = mod_vfs.lookup(target_path, &vn);
  if (err) return (long)err;

  if (vn->type != VNODE_DIR) {
    mod_vfs.vnode_release(vn);
    return -(long)ENOTDIR;
  }
  mod_vfs.vnode_release(vn);

  /* Resolve relative path to absolute, then normalize */
  char abs_path[VFS_PATH_MAX];
  if (target_path[0] != '/') {
    const char *cwd = current->cwd[0] ? current->cwd : "/";
    int cwdlen = (int)__builtin_strlen(cwd);
    int pathlen = (int)__builtin_strlen(target_path);
    if (cwdlen == 1) {
      abs_path[0] = '/';
      __builtin_memcpy(abs_path + 1, target_path, (size_t)pathlen + 1);
    } else {
      __builtin_memcpy(abs_path, cwd, (size_t)cwdlen);
      abs_path[cwdlen] = '/';
      __builtin_memcpy(abs_path + cwdlen + 1, target_path, (size_t)pathlen + 1);
    }
    target_path = abs_path;
  }

  char normalized[VFS_PATH_MAX];
  int nlen =
      mod_vfs.path_normalize(target_path, normalized, (int)sizeof(normalized));
  if (nlen < 0) return (long)nlen;

  if ((size_t)nlen >= sizeof(current->cwd)) return -(long)ENAMETOOLONG;
  __builtin_memcpy(current->cwd, normalized, (size_t)nlen + 1);
  return 0;
}

/* ── sys_dup ────────────────────────────────────────────────────────────────
 */

long sys_dup(long oldfd) {
  if (oldfd < 0 || (uint32_t)oldfd >= FD_MAX) return -(long)EBADF;
  int16_t desc = current->fd_map[(uint32_t)oldfd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;

  for (int i = 0; i < FD_MAX; i++) {
    if (current->fd_map[i] == FD_DESC_NONE) {
      current->fd_map[i] = desc;
      mod_vfs.fd_acquire(desc);
      return (long)i;
    }
  }
  return -(long)EMFILE;
}

/* ── sys_dup2 ───────────────────────────────────────────────────────────────
 */

long sys_dup2(long oldfd, long newfd) {
  if (oldfd < 0 || (uint32_t)oldfd >= FD_MAX) return -(long)EBADF;
  if (newfd < 0 || (uint32_t)newfd >= FD_MAX) return -(long)EBADF;
  int16_t desc = current->fd_map[(uint32_t)oldfd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;
  if (oldfd == newfd) return newfd;

  /* Close newfd if open */
  int16_t old_desc = current->fd_map[(uint32_t)newfd];
  if (old_desc != FD_DESC_NONE) {
    current->fd_map[(uint32_t)newfd] = FD_DESC_NONE;
    mod_vfs.fd_release(old_desc);
  }

  current->fd_map[(uint32_t)newfd] = desc;
  mod_vfs.fd_acquire(desc);
  return newfd;
}

/* ── sys_mkdir ───────────────────────────────────────────────────────────────
 */

long sys_mkdir(uintptr_t path_ptr, long mode) {
  char path[VFS_PATH_MAX];
  int rc = sys_copy_path(path, path_ptr);

  if (rc < 0) return (long)rc;

  vnode_t *parent = NULL;
  char namebuf[VFS_NAME_MAX + 1];
  int err = mod_vfs.lookup_parent(path, &parent, namebuf, (int)sizeof(namebuf));
  if (err) return (long)err;

  if (parent->type != VNODE_DIR) {
    mod_vfs.vnode_release(parent);
    return -(long)ENOTDIR;
  }
  if (!parent->mount || !parent->mount->ops || !parent->mount->ops->mkdir) {
    mod_vfs.vnode_release(parent);
    return -(long)ENOSYS;
  }
  if (parent->mount->flags & MNT_RDONLY) {
    mod_vfs.vnode_release(parent);
    return -(long)EROFS;
  }

  err = parent->mount->ops->mkdir(parent, namebuf, (uint32_t)mode);
  mod_vfs.vnode_release(parent);
  return (long)err;
}

/* ── sys_unlink ──────────────────────────────────────────────────────────────
 */

long sys_unlink(uintptr_t path_ptr) {
  char path[VFS_PATH_MAX];
  int rc = sys_copy_path(path, path_ptr);

  if (rc < 0) return (long)rc;

  vnode_t *parent = NULL;
  char namebuf[VFS_NAME_MAX + 1];
  int err = mod_vfs.lookup_parent(path, &parent, namebuf, (int)sizeof(namebuf));
  if (err) return (long)err;

  if (parent->type != VNODE_DIR) {
    mod_vfs.vnode_release(parent);
    return -(long)ENOTDIR;
  }
  if (!parent->mount || !parent->mount->ops || !parent->mount->ops->unlink) {
    mod_vfs.vnode_release(parent);
    return -(long)ENOSYS;
  }
  if (parent->mount->flags & MNT_RDONLY) {
    mod_vfs.vnode_release(parent);
    return -(long)EROFS;
  }

  err = parent->mount->ops->unlink(parent, namebuf);
  mod_vfs.vnode_release(parent);
  return (long)err;
}

/* ── sys_rename ──────────────────────────────────────────────────────────────
 */

long sys_rename(uintptr_t oldpath_ptr, uintptr_t newpath_ptr) {
  char oldpath[VFS_PATH_MAX];
  char newpath[VFS_PATH_MAX];
  int rc = sys_copy_path(oldpath, oldpath_ptr);

  if (rc < 0) return (long)rc;
  rc = sys_copy_path(newpath, newpath_ptr);
  if (rc < 0) return (long)rc;

  vnode_t *old_parent = NULL;
  char old_name[VFS_NAME_MAX + 1];
  int err = mod_vfs.lookup_parent(oldpath, &old_parent, old_name,
                                  (int)sizeof(old_name));
  if (err) return (long)err;

  vnode_t *new_parent = NULL;
  char new_name[VFS_NAME_MAX + 1];
  err = mod_vfs.lookup_parent(newpath, &new_parent, new_name,
                              (int)sizeof(new_name));
  if (err) {
    mod_vfs.vnode_release(old_parent);
    return (long)err;
  }

  if (old_parent->type != VNODE_DIR || new_parent->type != VNODE_DIR) {
    err = -(int)ENOTDIR;
    goto out;
  }
  if (!old_parent->mount || old_parent->mount != new_parent->mount ||
      !old_parent->mount->ops || !old_parent->mount->ops->rename) {
    err = -(int)ENOSYS;
    goto out;
  }
  if (old_parent->mount->flags & MNT_RDONLY) {
    err = -(int)EROFS;
    goto out;
  }

  err = old_parent->mount->ops->rename(old_parent, old_name, new_parent,
                                       new_name);
out:
  mod_vfs.vnode_release(old_parent);
  mod_vfs.vnode_release(new_parent);
  return (long)err;
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Linux-compatible syscalls (musl libc ABI)
 * ══════════════════════════════════════════════════════════════════════════════
 */

/* ── Linux struct stat64 — architecture-specific layout ─────────────────── */

#if !defined(__m68k__)
struct linux_stat64 {
  uint64_t st_dev;
  uint32_t __pad1;
  uint32_t __st_ino_truncated;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint32_t __pad2;
  int64_t st_size;
  uint32_t st_blksize;
  uint64_t st_blocks;
  uint32_t st_atime;
  uint32_t st_atime_nsec;
  uint32_t st_mtime;
  uint32_t st_mtime_nsec;
  uint32_t st_ctime;
  uint32_t st_ctime_nsec;
  uint64_t st_ino;
};
#endif

#if defined(__m68k__)
#define LINUX_STAT64_SIZE 140u
#else
#define LINUX_STAT64_SIZE sizeof(struct linux_stat64)
#endif

static void fill_stat64(const struct stat *src, void *buf) {
#if defined(__m68k__)
#define PUT_BE32(base, off, v)                \
  do {                                        \
    (base)[(off)] = (uint8_t)((v) >> 24);     \
    (base)[(off) + 1] = (uint8_t)((v) >> 16); \
    (base)[(off) + 2] = (uint8_t)((v) >> 8);  \
    (base)[(off) + 3] = (uint8_t)(v);         \
  } while (0)
  uint8_t *d = (uint8_t *)buf;
  memset(d, 0, 140);
  PUT_BE32(d, 10, (uint32_t)src->st_ino);
  PUT_BE32(d, 14, src->st_mode);
  PUT_BE32(d, 18, src->st_nlink);
  PUT_BE32(d, 44, (uint32_t)src->st_size);
  PUT_BE32(d, 48, 4096);
  PUT_BE32(d, 56, ((uint32_t)src->st_size + 511u) / 512u);
  PUT_BE32(d, 88, (uint32_t)src->st_ino);
#undef PUT_BE32
#else
  struct linux_stat64 *dst = (struct linux_stat64 *)buf;
  memset(dst, 0, sizeof(*dst));
  dst->st_ino = src->st_ino;
  dst->__st_ino_truncated = src->st_ino;
  dst->st_mode = src->st_mode;
  dst->st_nlink = src->st_nlink;
  dst->st_size = (int64_t)src->st_size;
  dst->st_blksize = 4096;
  dst->st_blocks = ((uint64_t)src->st_size + 511u) / 512u;
#endif
}

/* ── sys_stat64 ──────────────────────────────────────────────────────────────
 */

long sys_stat64(uintptr_t path_ptr, uintptr_t buf_ptr) {
  char path[VFS_PATH_MAX];
  uint8_t out[LINUX_STAT64_SIZE];
  int rc = sys_copy_path(path, path_ptr);

  if (rc < 0) return (long)rc;
  if (buf_ptr == 0u) return -(long)EINVAL;

  vnode_t *vn = NULL;
  int err = mod_vfs.lookup(path, &vn);
  if (err) return (long)err;

  struct stat st;
  err = mod_vfs.vnode_stat(vn, &st);
  mod_vfs.vnode_release(vn);
  if (err) return (long)err;

  fill_stat64(&st, out);
  rc = sys_copy_to_user(buf_ptr, out, sizeof(out));
  if (rc < 0) return (long)rc;
  return 0;
}

/* ── sys_fstat64 ─────────────────────────────────────────────────────────────
 */

long sys_fstat64(long fd, uintptr_t buf_ptr) {
  uint8_t out[LINUX_STAT64_SIZE];

  if (fd < 0 || (uint32_t)fd >= FD_MAX) return -(long)EBADF;
  if (buf_ptr == 0u) return -(long)EINVAL;
  int16_t desc = current->fd_map[(uint32_t)fd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;

  struct stat st;
  int err = mod_vfs.fd_fstat(desc, &st);
  if (err) return (long)err;

  fill_stat64(&st, out);
  if (sys_copy_to_user(buf_ptr, out, sizeof(out)) < 0) return -(long)EFAULT;
  return 0;
}

/* ── sys_lstat64 ─────────────────────────────────────────────────────────────
 */

long sys_lstat64(uintptr_t path_ptr, uintptr_t buf_ptr) {
  char path[VFS_PATH_MAX];
  uint8_t out[LINUX_STAT64_SIZE];
  int rc = sys_copy_path(path, path_ptr);

  if (rc < 0) return (long)rc;
  if (buf_ptr == 0u) return -(long)EINVAL;

  vnode_t *vn = NULL;
  int err = mod_vfs.lookup_flags(path, &vn, VFS_LOOKUP_NOFOLLOW);
  if (err) return (long)err;

  struct stat st;
  err = mod_vfs.vnode_stat(vn, &st);
  mod_vfs.vnode_release(vn);
  if (err) return (long)err;

  fill_stat64(&st, out);
  if (sys_copy_to_user(buf_ptr, out, sizeof(out)) < 0) return -(long)EFAULT;
  return 0;
}

/* ── sys_getdents64 ──────────────────────────────────────────────────────────
 */

long sys_getdents64(long fd, uintptr_t buf_ptr, long count) {
  uint8_t out[256];
  long tmp_count = count;

  if (fd < 0 || (uint32_t)fd >= FD_MAX) return -(long)EBADF;
  if (buf_ptr == 0u) return -(long)EINVAL;
  int16_t desc = current->fd_map[(uint32_t)fd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;
  if (tmp_count > (long)sizeof(out)) tmp_count = (long)sizeof(out);
  tmp_count = mod_vfs.fd_getdents64(desc, out, tmp_count);
  if (tmp_count <= 0) return tmp_count;
  if (sys_copy_to_user(buf_ptr, out, (size_t)tmp_count) < 0)
    return -(long)EFAULT;
  return tmp_count;
}

/* ── sys_llseek ──────────────────────────────────────────────────────────────
 */

long sys_llseek(long fd, long off_hi, long off_lo, uintptr_t result_ptr,
                long whence) {
  (void)off_hi;
  long pos = sys_lseek(fd, off_lo, whence);
  if (pos < 0) return pos;

  if (result_ptr != 0u) {
    int64_t res = (int64_t)pos;
    if (sys_copy_to_user(result_ptr, &res, sizeof(res)) < 0)
      return -(long)EFAULT;
  }
  return 0;
}

/* ── sys_fcntl64 ─────────────────────────────────────────────────────────────
 */

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030

long sys_fcntl64(long fd, long cmd, long arg) {
  if (fd < 0 || (uint32_t)fd >= FD_MAX) return -(long)EBADF;
  int16_t desc = current->fd_map[(uint32_t)fd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;

  switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
      /* Find lowest fd >= arg */
      for (long i = arg; i < FD_MAX; i++) {
        if (current->fd_map[i] == FD_DESC_NONE) {
          current->fd_map[i] = desc;
          mod_vfs.fd_acquire(desc);
          return i;
        }
      }
      return -(long)EMFILE;

    case F_GETFD:
      return 0;
    case F_SETFD:
      return 0;

    case F_GETFL:
    case F_SETFL:
      return mod_vfs.fd_fcntl(desc, (int)cmd, arg);

    default:
      return -(long)EINVAL;
  }
}

/* ── sys_access ──────────────────────────────────────────────────────────────
 */

long sys_access(uintptr_t path_ptr, long mode) {
  char path[VFS_PATH_MAX];
  int rc = sys_copy_path(path, path_ptr);

  (void)mode;
  if (rc < 0) return (long)rc;

  vnode_t *vn = NULL;
  int err = mod_vfs.lookup(path, &vn);
  if (err) return (long)err;

  mod_vfs.vnode_release(vn);
  return 0;
}

/* ── sys_readlink ────────────────────────────────────────────────────────────
 */

long sys_readlink(uintptr_t path_ptr, uintptr_t buf_ptr, long bufsiz) {
  char path[VFS_PATH_MAX];
  char out[VFS_PATH_MAX];
  int rc = sys_copy_path(path, path_ptr);

  if (rc < 0) return (long)rc;
  if (buf_ptr == 0u || bufsiz <= 0) return -(long)EINVAL;

  if (strcmp(path, "/proc/self/exe") == 0) {
    const char *exe = "/bin/busybox";
    size_t len = strlen(exe);
    if ((long)len > bufsiz) len = (size_t)bufsiz;
    rc = sys_copy_to_user(buf_ptr, exe, len);
    if (rc < 0) return (long)rc;
    return (long)len;
  }

  vnode_t *vn = NULL;
  int err = mod_vfs.lookup_flags(path, &vn, VFS_LOOKUP_NOFOLLOW);
  if (err) return (long)err;

  if (vn->type != VNODE_SYMLINK) {
    mod_vfs.vnode_release(vn);
    return -(long)EINVAL;
  }
  size_t out_size = (size_t)bufsiz;
  if (out_size > sizeof(out)) out_size = sizeof(out);
  long ret = mod_vfs.vnode_readlink(vn, out, out_size);
  mod_vfs.vnode_release(vn);
  if (ret > 0) {
    rc = sys_copy_to_user(buf_ptr, out, (size_t)ret);
    if (rc < 0) return (long)rc;
  }
  return ret;
}

/* ── sys_rmdir ───────────────────────────────────────────────────────────────
 */

long sys_rmdir(uintptr_t path_ptr) { return sys_unlink(path_ptr); }

/* ── sys_umask ───────────────────────────────────────────────────────────────
 */

long sys_umask(long mask) {
  uint32_t old = current->umask_val;
  current->umask_val = (uint32_t)mask & 0777u;
  return (long)old;
}

/* ── sys_mount ───────────────────────────────────────────────────────────────
 */

long sys_mount(uintptr_t source_ptr, uintptr_t target_ptr, uintptr_t fstype_ptr,
               long flags, uintptr_t data_ptr) {
  char source[VFS_PATH_MAX];
  char target[VFS_PATH_MAX];
  char fstype[VFS_NAME_MAX + 1];
  const char *source_path = NULL;
  int rc;

  if (source_ptr != 0u) {
    rc = sys_copy_user_string(source, sizeof(source), source_ptr);
    if (rc < 0) return (long)rc;
    source_path = source;
  }
  rc = sys_copy_user_string(target, sizeof(target), target_ptr);
  if (rc < 0) return (long)rc;
  rc = sys_copy_user_string(fstype, sizeof(fstype), fstype_ptr);
  if (rc < 0) return (long)rc;
  (void)data_ptr;
  return (long)mod_vfs.mount_by_fstype(source_path, target, fstype, flags);
}

/* ── sys_pipe ────────────────────────────────────────────────────────────────
 */

long sys_pipe(uintptr_t fds_ptr) {
  int out[2];

  if (fds_ptr == 0u) return -(long)EINVAL;

  int rdesc, wdesc;
  int err = mod_vfs.fd_pipe_create(&rdesc, &wdesc);
  if (err) return (long)err;

  int rfd = -1, wfd = -1;
  for (int i = 0; i < FD_MAX && (rfd < 0 || wfd < 0); i++) {
    if (current->fd_map[i] == FD_DESC_NONE) {
      if (rfd < 0)
        rfd = i;
      else
        wfd = i;
    }
  }

  if (rfd < 0 || wfd < 0) {
    mod_vfs.fd_release(rdesc);
    mod_vfs.fd_release(wdesc);
    return -(long)EMFILE;
  }

  current->fd_map[rfd] = (int16_t)rdesc;
  current->fd_map[wfd] = (int16_t)wdesc;
  out[0] = rfd;
  out[1] = wfd;
  if (sys_copy_to_user(fds_ptr, out, sizeof(out)) < 0) {
    current->fd_map[rfd] = FD_DESC_NONE;
    current->fd_map[wfd] = FD_DESC_NONE;
    mod_vfs.fd_release(rdesc);
    mod_vfs.fd_release(wdesc);
    return -(long)EFAULT;
  }
  return 0;
}

/* ── sys_umount2 ────────────────────────────────────────────────────────────
 */

long sys_umount2(uintptr_t target_ptr, long flags) {
  char target[VFS_PATH_MAX];
  int rc = sys_copy_path(target, target_ptr);

  (void)flags;
  if (rc < 0) return (long)rc;

  char norm[VFS_PATH_MAX];
  int nlen = mod_vfs.path_normalize(target, norm, (int)sizeof(norm));
  if (nlen < 0) return (long)nlen;
  return (long)mod_vfs.umount(norm);
}

/* ── sys_statfs64 ───────────────────────────────────────────────────────────
 */

long sys_statfs64(uintptr_t path_ptr, long sz, uintptr_t buf_ptr) {
  char path[VFS_PATH_MAX];
  (void)sz;
  int rc = sys_copy_path(path, path_ptr);

  if (rc < 0) return (long)rc;
  if (buf_ptr == 0u) return -(long)EINVAL;

  const char *remainder;
  mount_entry_t *mnt = mod_vfs.mount_find(path, &remainder);
  if (!mnt) return -(long)ENOENT;

  struct kernel_statfs ksf;
  __builtin_memset(&ksf, 0, sizeof(ksf));
  if (mnt->ops && mnt->ops->statfs) mnt->ops->statfs(mnt, &ksf);
  rc = sys_copy_to_user(buf_ptr, &ksf, sizeof(ksf));
  if (rc < 0) return (long)rc;
  return 0;
}

/* ── sys_fstatfs64 ──────────────────────────────────────────────────────────
 */

long sys_fstatfs64(long fd, long sz, uintptr_t buf_ptr) {
  struct kernel_statfs ksf;
  int rc;

  (void)sz;
  if (fd < 0 || (uint32_t)fd >= FD_MAX) return -(long)EBADF;
  if (buf_ptr == 0u) return -(long)EINVAL;
  int16_t desc = current->fd_map[(uint32_t)fd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;
  rc = mod_vfs.fd_fstatfs(desc, &ksf);
  if (rc) return (long)rc;
  rc = sys_copy_to_user(buf_ptr, &ksf, sizeof(ksf));
  if (rc < 0) return (long)rc;
  return 0;
}
