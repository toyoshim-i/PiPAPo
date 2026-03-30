/*
 * sys_fs.c — Filesystem syscall implementations
 *
 * File descriptor syscalls resolve fd_map[fd] → descriptor ID, then
 * delegate to mod_vfs.fd_*(desc, ...) in the VFS module.
 *
 * Path-based syscalls (stat, mkdir, unlink, etc.) route through
 * mod_vfs.lookup / mod_vfs.lookup_parent as before.
 */

#include "../common/errno.h"
#include "../common/mod/mod_vfs.h"
#include "../proc/proc.h"
#include "config.h"
#include "syscall.h"

#include <stddef.h>
#include <string.h>

/* ── sys_open ────────────────────────────────────────────────────────────────
 */

long sys_open(const char *path, long flags, long mode) {
  if (!path) return -(long)EINVAL;

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

long sys_stat(const char *path, struct stat *buf) {
  if (!path || !buf) return -(long)EINVAL;

  vnode_t *vn = NULL;
  int err = mod_vfs.lookup(path, &vn);
  if (err) return (long)err;

  if (!vn->mount || !vn->mount->ops || !vn->mount->ops->stat) {
    mod_vfs.release_vnode(vn);
    return -(long)ENOSYS;
  }

  err = vn->mount->ops->stat(vn, buf);
  mod_vfs.release_vnode(vn);
  return (long)err;
}

/* ── sys_fstat ───────────────────────────────────────────────────────────────
 */

long sys_fstat(long fd, struct stat *buf) {
  if (fd < 0 || (uint32_t)fd >= FD_MAX || !buf) return -(long)EBADF;
  int16_t desc = current->fd_map[(uint32_t)fd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;
  return (long)mod_vfs.fd_fstat(desc, buf);
}

/* ── sys_getdents ────────────────────────────────────────────────────────────
 */

long sys_getdents(long fd, struct dirent *buf, size_t count) {
  if (fd < 0 || (uint32_t)fd >= FD_MAX || !buf) return -(long)EBADF;
  int16_t desc = current->fd_map[(uint32_t)fd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;
  return mod_vfs.fd_getdents(desc, buf, count);
}

/* ── sys_getcwd ──────────────────────────────────────────────────────────────
 */

long sys_getcwd(char *buf, size_t size) {
  if (!buf || size == 0) return -(long)EINVAL;

  const char *cwd = current->cwd;
  size_t len = __builtin_strlen(cwd);

  if (len == 0) {
    if (size < 2) return -(long)ERANGE;
    buf[0] = '/';
    buf[1] = '\0';
    return 2;
  }

  if (len + 1 > size) return -(long)ERANGE;
  __builtin_memcpy(buf, cwd, len + 1);
  return (long)(len + 1);
}

/* ── sys_chdir ───────────────────────────────────────────────────────────────
 */

long sys_chdir(const char *path) {
  if (!path) return -(long)EINVAL;

  vnode_t *vn = NULL;
  int err = mod_vfs.lookup(path, &vn);
  if (err) return (long)err;

  if (vn->type != VNODE_DIR) {
    mod_vfs.release_vnode(vn);
    return -(long)ENOTDIR;
  }
  mod_vfs.release_vnode(vn);

  /* Resolve relative path to absolute, then normalize */
  char abs_path[VFS_PATH_MAX];
  if (path[0] != '/') {
    const char *cwd = current->cwd[0] ? current->cwd : "/";
    int cwdlen = (int)__builtin_strlen(cwd);
    int pathlen = (int)__builtin_strlen(path);
    if (cwdlen == 1) {
      abs_path[0] = '/';
      __builtin_memcpy(abs_path + 1, path, (size_t)pathlen + 1);
    } else {
      __builtin_memcpy(abs_path, cwd, (size_t)cwdlen);
      abs_path[cwdlen] = '/';
      __builtin_memcpy(abs_path + cwdlen + 1, path, (size_t)pathlen + 1);
    }
    path = abs_path;
  }

  char normalized[VFS_PATH_MAX];
  int nlen = mod_vfs.path_normalize(path, normalized, (int)sizeof(normalized));
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

long sys_mkdir(const char *path, long mode) {
  if (!path) return -(long)EINVAL;

  vnode_t *parent = NULL;
  char namebuf[VFS_NAME_MAX + 1];
  int err = mod_vfs.lookup_parent(path, &parent, namebuf,
                                  (int)sizeof(namebuf));
  if (err) return (long)err;

  if (parent->type != VNODE_DIR) {
    mod_vfs.release_vnode(parent);
    return -(long)ENOTDIR;
  }
  if (!parent->mount || !parent->mount->ops || !parent->mount->ops->mkdir) {
    mod_vfs.release_vnode(parent);
    return -(long)ENOSYS;
  }
  if (parent->mount->flags & MNT_RDONLY) {
    mod_vfs.release_vnode(parent);
    return -(long)EROFS;
  }

  err = parent->mount->ops->mkdir(parent, namebuf, (uint32_t)mode);
  mod_vfs.release_vnode(parent);
  return (long)err;
}

/* ── sys_unlink ──────────────────────────────────────────────────────────────
 */

long sys_unlink(const char *path) {
  if (!path) return -(long)EINVAL;

  vnode_t *parent = NULL;
  char namebuf[VFS_NAME_MAX + 1];
  int err = mod_vfs.lookup_parent(path, &parent, namebuf,
                                  (int)sizeof(namebuf));
  if (err) return (long)err;

  if (parent->type != VNODE_DIR) {
    mod_vfs.release_vnode(parent);
    return -(long)ENOTDIR;
  }
  if (!parent->mount || !parent->mount->ops || !parent->mount->ops->unlink) {
    mod_vfs.release_vnode(parent);
    return -(long)ENOSYS;
  }
  if (parent->mount->flags & MNT_RDONLY) {
    mod_vfs.release_vnode(parent);
    return -(long)EROFS;
  }

  err = parent->mount->ops->unlink(parent, namebuf);
  mod_vfs.release_vnode(parent);
  return (long)err;
}

/* ── sys_rename ──────────────────────────────────────────────────────────────
 */

long sys_rename(const char *oldpath, const char *newpath) {
  if (!oldpath || !newpath) return -(long)EINVAL;

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
    mod_vfs.release_vnode(old_parent);
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
  mod_vfs.release_vnode(old_parent);
  mod_vfs.release_vnode(new_parent);
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

long sys_stat64(const char *path, void *buf) {
  if (!path || !buf) return -(long)EINVAL;

  vnode_t *vn = NULL;
  int err = mod_vfs.lookup(path, &vn);
  if (err) return (long)err;

  if (!vn->mount || !vn->mount->ops || !vn->mount->ops->stat) {
    mod_vfs.release_vnode(vn);
    return -(long)ENOSYS;
  }

  struct stat st;
  err = vn->mount->ops->stat(vn, &st);
  mod_vfs.release_vnode(vn);
  if (err) return (long)err;

  fill_stat64(&st, buf);
  return 0;
}

/* ── sys_fstat64 ─────────────────────────────────────────────────────────────
 */

long sys_fstat64(long fd, void *buf) {
  if (fd < 0 || (uint32_t)fd >= FD_MAX || !buf) return -(long)EBADF;
  int16_t desc = current->fd_map[(uint32_t)fd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;

  struct stat st;
  int err = mod_vfs.fd_fstat(desc, &st);
  if (err) return (long)err;

  fill_stat64(&st, buf);
  return 0;
}

/* ── sys_lstat64 ─────────────────────────────────────────────────────────────
 */

long sys_lstat64(const char *path, void *buf) {
  if (!path || !buf) return -(long)EINVAL;

  vnode_t *vn = NULL;
  int err = mod_vfs.lookup_flags(path, &vn, VFS_LOOKUP_NOFOLLOW);
  if (err) return (long)err;

  if (!vn->mount || !vn->mount->ops || !vn->mount->ops->stat) {
    mod_vfs.release_vnode(vn);
    return -(long)ENOSYS;
  }

  struct stat st;
  err = vn->mount->ops->stat(vn, &st);
  mod_vfs.release_vnode(vn);
  if (err) return (long)err;

  fill_stat64(&st, buf);
  return 0;
}

/* ── sys_getdents64 ──────────────────────────────────────────────────────────
 */

long sys_getdents64(long fd, void *buf, long count) {
  if (fd < 0 || (uint32_t)fd >= FD_MAX || !buf) return -(long)EBADF;
  int16_t desc = current->fd_map[(uint32_t)fd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;
  return mod_vfs.fd_getdents64(desc, buf, count);
}

/* ── sys_llseek ──────────────────────────────────────────────────────────────
 */

long sys_llseek(long fd, long off_hi, long off_lo, void *result, long whence) {
  (void)off_hi;
  long pos = sys_lseek(fd, off_lo, whence);
  if (pos < 0) return pos;

  if (result) {
    int64_t res = (int64_t)pos;
    memcpy(result, &res, sizeof(res));
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

long sys_access(const char *path, long mode) {
  (void)mode;
  if (!path) return -(long)EINVAL;

  vnode_t *vn = NULL;
  int err = mod_vfs.lookup(path, &vn);
  if (err) return (long)err;

  mod_vfs.release_vnode(vn);
  return 0;
}

/* ── sys_readlink ────────────────────────────────────────────────────────────
 */

long sys_readlink(const char *path, char *buf, long bufsiz) {
  if (!path || !buf || bufsiz <= 0) return -(long)EINVAL;

  if (strcmp(path, "/proc/self/exe") == 0) {
    const char *exe = "/bin/busybox";
    size_t len = strlen(exe);
    if ((long)len > bufsiz) len = (size_t)bufsiz;
    memcpy(buf, exe, len);
    return (long)len;
  }

  vnode_t *vn = NULL;
  int err = mod_vfs.lookup_flags(path, &vn, VFS_LOOKUP_NOFOLLOW);
  if (err) return (long)err;

  if (vn->type != VNODE_SYMLINK) {
    mod_vfs.release_vnode(vn);
    return -(long)EINVAL;
  }
  if (!vn->mount || !vn->mount->ops || !vn->mount->ops->readlink) {
    mod_vfs.release_vnode(vn);
    return -(long)EINVAL;
  }

  long ret = vn->mount->ops->readlink(vn, buf, (size_t)bufsiz);
  mod_vfs.release_vnode(vn);
  return ret;
}

/* ── sys_rmdir ───────────────────────────────────────────────────────────────
 */

long sys_rmdir(const char *path) {
  return sys_unlink(path);
}

/* ── sys_umask ───────────────────────────────────────────────────────────────
 */

long sys_umask(long mask) {
  uint32_t old = current->umask_val;
  current->umask_val = (uint32_t)mask & 0777u;
  return (long)old;
}

/* ── sys_mount ───────────────────────────────────────────────────────────────
 */

#include "../fs/devfs.h"
#include "../fs/procfs.h"
#include "../fs/tmpfs.h"
#ifdef PPAP_HAS_BLKDEV
#include "../blkdev/blkdev.h"
#include "../fs/ufs.h"
#include "../fs/vfat.h"
#endif

static int fs_str_eq(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return (*a == '\0' && *b == '\0');
}

long sys_mount(const char *source, const char *target, const char *fstype,
               long flags, const void *data) {
  (void)data;
  if (!target || !fstype) return -(long)EINVAL;

  const vfs_ops_t *ops = NULL;
  const void *dev_data = NULL;

  if (fs_str_eq(fstype, "devfs"))
    ops = &devfs_ops;
  else if (fs_str_eq(fstype, "proc") || fs_str_eq(fstype, "procfs"))
    ops = &procfs_ops;
  else if (fs_str_eq(fstype, "tmpfs"))
    ops = &tmpfs_ops;
#ifdef PPAP_HAS_BLKDEV
  else if (fs_str_eq(fstype, "vfat")) {
    ops = &vfat_ops;
    if (source) {
      const char *devname = source;
      if (devname[0] == '/' && devname[1] == 'd' && devname[2] == 'e' &&
          devname[3] == 'v' && devname[4] == '/')
        devname += 5;
      blkdev_t *bd = blkdev_find(devname);
      if (!bd) return -(long)ENODEV;
      dev_data = bd;
    }
  } else if (fs_str_eq(fstype, "ufs")) {
    ops = &ufs_ops;
    if (source) {
      const char *devname = source;
      if (devname[0] == '/' && devname[1] == 'd' && devname[2] == 'e' &&
          devname[3] == 'v' && devname[4] == '/')
        devname += 5;
      blkdev_t *bd = blkdev_find(devname);
      if (!bd) return -(long)ENODEV;
      dev_data = bd;
    }
  }
#endif
  else {
    return -(long)ENODEV;
  }

  uint8_t mnt_flags = 0;
  if ((uint32_t)flags & MS_RDONLY) mnt_flags |= MNT_RDONLY;

  return (long)mod_vfs.mount(target, ops, mnt_flags, dev_data);
}

/* ── sys_umount2 ────────────────────────────────────────────────────────────
 */

long sys_umount2(const char *target, long flags) {
  (void)flags;
  if (!target) return -(long)EINVAL;

  char norm[VFS_PATH_MAX];
  int nlen = mod_vfs.path_normalize(target, norm, (int)sizeof(norm));
  if (nlen < 0) return (long)nlen;
  return (long)mod_vfs.umount(norm);
}

/* ── sys_statfs64 ───────────────────────────────────────────────────────────
 */

long sys_statfs64(const char *path, long sz, void *buf) {
  (void)sz;
  if (!path || !buf) return -(long)EINVAL;

  const char *remainder;
  mount_entry_t *mnt = mod_vfs.find_mount(path, &remainder);
  if (!mnt) return -(long)ENOENT;

  struct kernel_statfs ksf;
  __builtin_memset(&ksf, 0, sizeof(ksf));
  if (mnt->ops && mnt->ops->statfs) mnt->ops->statfs(mnt, &ksf);
  __builtin_memcpy(buf, &ksf, sizeof(ksf));
  return 0;
}

/* ── sys_fstatfs64 ──────────────────────────────────────────────────────────
 */

long sys_fstatfs64(long fd, long sz, void *buf) {
  (void)sz;
  if (fd < 0 || (uint32_t)fd >= FD_MAX || !buf) return -(long)EBADF;
  int16_t desc = current->fd_map[(uint32_t)fd];
  if (desc == FD_DESC_NONE) return -(long)EBADF;
  return (long)mod_vfs.fd_fstatfs(desc, buf);
}
