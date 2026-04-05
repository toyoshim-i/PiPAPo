/*
 * fd.c — System-wide file descriptor pool (VFS module)
 *
 * Manages a flat array of struct file objects indexed by descriptor ID.
 * Core-side code resolves per-process fd → descriptor ID via fd_map[],
 * then calls mod_vfs.fd_*(desc_id, ...) to perform operations.
 *
 * VFS is process-agnostic — it never sees pid or pcb_t.
 */

#include "kernel/vfs/fd.h"
#include "kernel/vfs/file.h"
#include "kernel/vfs/tty.h"

#include <stddef.h>
#include <string.h>

#include "common/errno.h"
#include "kernel/common/mod/mod_core.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/vfs/devfs.h"
#include "kernel/common/core/proc_info.h" /* pcb_t — only for fd_stdio_init compat */
#include "kernel/common/config.h"

/* ── System-wide file descriptor pool ──────────────────────────────────────── */

static struct file fd_pool[FILE_MAX];

/* Descriptor IDs for tty stdin/stdout/stderr (allocated at init) */
static int stdio_descs[3] = {-1, -1, -1};

static int fd_pool_alloc(void) {
  for (int i = 0; i < FILE_MAX; i++) {
    if (fd_pool[i].refcnt == 0 && fd_pool[i].ops == NULL) {
      memset(&fd_pool[i], 0, sizeof(struct file));
      return i;
    }
  }
  return -1;
}

/* ── VFS bridge file_ops ───────────────────────────────────────────────────── */

static long vfs_bridge_read(struct file *f, page_id_t page,
                            uint16_t off, size_t n) {
  if (!f->vnode || !f->vnode->mount || !f->vnode->mount->ops ||
      !f->vnode->mount->ops->read)
    return -(long)EBADF;
  long ret = f->vnode->mount->ops->read(f->vnode, page, off, n, f->offset);
  if (ret > 0) f->offset += (uint32_t)ret;
  return ret;
}

static long vfs_bridge_write(struct file *f, page_id_t page,
                             uint16_t off, size_t n) {
  if (!f->vnode || !f->vnode->mount || !f->vnode->mount->ops ||
      !f->vnode->mount->ops->write)
    return -(long)EBADF;

  /* O_APPEND: always write at end of file */
  if (f->flags & O_APPEND) f->offset = f->vnode->size;
  long ret = f->vnode->mount->ops->write(f->vnode, page, off, n, f->offset);
  if (ret > 0) f->offset += (uint32_t)ret;
  return ret;
}

static int vfs_bridge_close(struct file *f) {
  if (f->vnode) mod_vfs.vnode_release(f->vnode);
  f->vnode = NULL;
  return 0;
}

static const struct file_ops vfs_bridge_ops = {
    vfs_bridge_read,  vfs_bridge_write,
    vfs_bridge_close, NULL, /* ioctl */
    NULL,                   /* poll */
};

/* ── Pool init (replaces fd_pool_init) ───────────────────────────────────── */

void fd_pool_init(void) {
  /* Pre-allocate entries 0-2 for tty stdin/stdout/stderr.
   * These have a permanent base ref and are never freed. */
  void *console = tty_get_console_dev();
  for (int i = 0; i < 3; i++) {
    memset(&fd_pool[i], 0, sizeof(struct file));
    fd_pool[i].ops = &tty_fops;
    fd_pool[i].priv = console;
    fd_pool[i].flags = (i == 0) ? O_RDONLY : O_WRONLY;
    fd_pool[i].refcnt = 1; /* permanent base ref */
    stdio_descs[i] = i;
  }
}

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

int vfs_fd_stdio_desc(int which) {
  if (which < 0 || which > 2) return -1;
  fd_pool[stdio_descs[which]].refcnt++;
  return stdio_descs[which];
}

void vfs_fd_acquire(int desc) {
  if (desc < 0 || desc >= FILE_MAX) return;
  fd_pool[desc].refcnt++;
}

void vfs_fd_release(int desc) {
  if (desc < 0 || desc >= FILE_MAX) return;
  struct file *f = &fd_pool[desc];
  if (f->refcnt > 0u) f->refcnt--;
  if (f->refcnt == 0u) {
    if (f->ops && f->ops->close) f->ops->close(f);
    memset(f, 0, sizeof(struct file));
  }
}

/* ── fd_open ───────────────────────────────────────────────────────────────── */

int vfs_fd_open(const char *path, int flags, int mode) {
  if (!path) return -EINVAL;

  vnode_t *vn = NULL;
  int err = mod_vfs.lookup(path, &vn);

  /* O_CREAT: if file doesn't exist, create it */
  if (err == -ENOENT && ((uint32_t)flags & O_CREAT)) {
    vnode_t *parent = NULL;
    char namebuf[VFS_NAME_MAX + 1];
    err = mod_vfs.lookup_parent(path, &parent, namebuf,
                                (int)sizeof(namebuf));
    if (err) return err;
    if (parent->type != VNODE_DIR) {
      mod_vfs.vnode_release(parent);
      return -ENOTDIR;
    }
    if (!parent->mount || !parent->mount->ops ||
        !parent->mount->ops->create) {
      mod_vfs.vnode_release(parent);
      return -ENOSYS;
    }
    if (parent->mount->flags & MNT_RDONLY) {
      mod_vfs.vnode_release(parent);
      return -EROFS;
    }
    err = parent->mount->ops->create(parent, namebuf, (uint32_t)mode, &vn);
    mod_vfs.vnode_release(parent);
    if (err) return err;
  } else if (err) {
    return err;
  }

  /* O_TRUNC: truncate existing file to zero length */
  if (((uint32_t)flags & O_TRUNC) && vn->type == VNODE_FILE) {
    if (vn->mount && vn->mount->ops && vn->mount->ops->truncate) {
      int terr = vn->mount->ops->truncate(vn, 0);
      if (terr) {
        mod_vfs.vnode_release(vn);
        return terr;
      }
    } else {
      vn->size = 0;
    }
  }

  /* TTY device detection */
  if (vn->type == VNODE_DEV && vn->fs_priv) {
    const char *devname = *(const char **)vn->fs_priv;
    int tty_idx = -1;
    if (devname) {
      if (strcmp(devname, "ttyS0") == 0)
        tty_idx = TTY_SERIAL;
      else if (strcmp(devname, "tty1") == 0)
        tty_idx = TTY_DISPLAY;
      else if (strcmp(devname, "console") == 0)
        tty_idx = TTY_SERIAL;
      else if (strcmp(devname, "tty") == 0)
        tty_idx = TTY_SERIAL;
    }
    if (tty_idx >= 0) {
      int desc = fd_pool_alloc();
      if (desc < 0) {
        mod_vfs.vnode_release(vn);
        return -ENOMEM;
      }
      fd_pool[desc].ops = &tty_fops;
      fd_pool[desc].priv = tty_get_dev(tty_idx);
      fd_pool[desc].flags = (uint32_t)flags;
      fd_pool[desc].refcnt = 1;
      fd_pool[desc].vnode = NULL;
      fd_pool[desc].offset = 0;
      mod_vfs.vnode_release(vn);
      return desc;
    }
  }

  /* Regular file: allocate pool entry */
  int desc = fd_pool_alloc();
  if (desc < 0) {
    mod_vfs.vnode_release(vn);
    return -ENOMEM;
  }

  fd_pool[desc].ops = &vfs_bridge_ops;
  fd_pool[desc].priv = NULL;
  fd_pool[desc].flags = (uint32_t)flags;
  fd_pool[desc].refcnt = 1;
  fd_pool[desc].vnode = vn;
  fd_pool[desc].offset = ((uint32_t)flags & O_APPEND) ? vn->size : 0;
  return desc;
}

/* ── I/O ───────────────────────────────────────────────────────────────────── */

long vfs_fd_read(int desc, page_id_t page, uint16_t off, size_t n) {
  if (desc < 0 || desc >= FILE_MAX) return -(long)EBADF;
  struct file *f = &fd_pool[desc];
  if (!f->ops || !f->ops->read) return -(long)EBADF;
  return f->ops->read(f, page, off, n);
}

long vfs_fd_write(int desc, page_id_t page, uint16_t off, size_t n) {
  if (desc < 0 || desc >= FILE_MAX) return -(long)EBADF;
  struct file *f = &fd_pool[desc];
  if (!f->ops || !f->ops->write) return -(long)EBADF;
  return f->ops->write(f, page, off, n);
}

int vfs_fd_ioctl(int desc, uint32_t cmd, void *arg) {
  if (desc < 0 || desc >= FILE_MAX) return -EBADF;
  struct file *f = &fd_pool[desc];
  if (!f->ops) return -EBADF;
  if (!f->ops->ioctl) return -ENOTTY;
  return f->ops->ioctl(f, cmd, arg);
}

int vfs_fd_poll(int desc) {
  if (desc < 0 || desc >= FILE_MAX) return 0;
  struct file *f = &fd_pool[desc];
  if (f->ops && f->ops->poll) return f->ops->poll(f);
  return POLLIN | POLLOUT; /* no poll → assume ready */
}

/* ── File state ────────────────────────────────────────────────────────────── */

long vfs_fd_lseek(int desc, long off, int whence) {
  if (desc < 0 || desc >= FILE_MAX) return -(long)EBADF;
  struct file *f = &fd_pool[desc];
  if (!f->vnode) return -(long)ESPIPE;

  long new_off;
  switch (whence) {
    case SEEK_SET:
      new_off = off;
      break;
    case SEEK_CUR:
      new_off = (long)f->offset + off;
      break;
    case SEEK_END:
      new_off = (long)f->vnode->size + off;
      break;
    default:
      return -(long)EINVAL;
  }
  if (new_off < 0) return -(long)EINVAL;
  f->offset = (uint32_t)new_off;
  return new_off;
}

int vfs_fd_fstat(int desc, void *buf) {
  if (desc < 0 || desc >= FILE_MAX || !buf) return -EBADF;
  struct file *f = &fd_pool[desc];

  /* tty files (no vnode): synthesize minimal char-device stat */
  if (!f->vnode) {
    struct stat tty_st;
    memset(&tty_st, 0, sizeof(tty_st));
    tty_st.st_mode = S_IFCHR | 0666u;
    tty_st.st_nlink = 1;
    memcpy(buf, &tty_st, sizeof(tty_st));
    return 0;
  }

  if (!f->vnode->mount || !f->vnode->mount->ops ||
      !f->vnode->mount->ops->stat)
    return -ENOSYS;

  return f->vnode->mount->ops->stat(f->vnode, (struct stat *)buf);
}

/* Sentinel: directory fully read */
#define GETDENTS_EOF 0xFFFFFFFFu

long vfs_fd_getdents(int desc, void *buf, size_t count) {
  if (desc < 0 || desc >= FILE_MAX || !buf) return -(long)EBADF;
  struct file *f = &fd_pool[desc];
  if (!f->vnode || f->vnode->type != VNODE_DIR) return -(long)ENOTDIR;
  if (!f->vnode->mount || !f->vnode->mount->ops ||
      !f->vnode->mount->ops->readdir)
    return -(long)ENOSYS;
  if (f->offset == GETDENTS_EOF) return 0;

  size_t max_entries = count / sizeof(struct dirent);
  if (max_entries == 0) return -(long)EINVAL;

  uint32_t cookie = f->offset;
  int n = f->vnode->mount->ops->readdir(f->vnode, (struct dirent *)buf,
                                        max_entries, &cookie);
  if (n > 0)
    f->offset = (cookie == 0) ? GETDENTS_EOF : cookie;
  else if (n == 0)
    f->offset = GETDENTS_EOF;
  return (long)n;
}

long vfs_fd_getdents64(int desc, void *buf, long count) {
  if (desc < 0 || desc >= FILE_MAX || !buf) return -(long)EBADF;
  struct file *f = &fd_pool[desc];
  if (!f->vnode || f->vnode->type != VNODE_DIR) return -(long)ENOTDIR;
  if (!f->vnode->mount || !f->vnode->mount->ops ||
      !f->vnode->mount->ops->readdir)
    return -(long)ENOSYS;
  if (f->offset == GETDENTS_EOF) return 0;

  struct dirent entries[8];
  uint32_t cookie = f->offset;
  int n = f->vnode->mount->ops->readdir(f->vnode, entries, 8, &cookie);
  if (n < 0) return (long)n;
  if (n == 0) {
    f->offset = GETDENTS_EOF;
    return 0;
  }

  /* Pack into linux dirent64 format */
  uint8_t *out = (uint8_t *)buf;
  long total = 0;
  for (int i = 0; i < n; i++) {
    size_t name_len = strlen(entries[i].d_name);
    uint16_t reclen = (uint16_t)((19 + name_len + 1 + 7) & ~7u);
    if (total + reclen > count) break;

    memset(out + total, 0, reclen);
    uint64_t ino = entries[i].d_ino;
    memcpy(out + total, &ino, 8);
    int64_t d_off = (int64_t)(f->offset + (uint32_t)i + 1);
    memcpy(out + total + 8, &d_off, 8);
    memcpy(out + total + 16, &reclen, 2);
    out[total + 18] = entries[i].d_type;
    memcpy(out + total + 19, entries[i].d_name, name_len + 1);
    total += reclen;
  }

  f->offset = (cookie == 0) ? GETDENTS_EOF : cookie;
  return total;
}

int vfs_fd_fstatfs(int desc, void *buf) {
  if (desc < 0 || desc >= FILE_MAX || !buf) return -EBADF;
  struct file *f = &fd_pool[desc];

  mount_entry_t *mnt = NULL;
  if (f->vnode && f->vnode->mount) mnt = f->vnode->mount;

  struct kernel_statfs ksf;
  __builtin_memset(&ksf, 0, sizeof(ksf));
  if (mnt && mnt->ops && mnt->ops->statfs) mnt->ops->statfs(mnt, &ksf);
  __builtin_memcpy(buf, &ksf, sizeof(ksf));
  return 0;
}

long vfs_fd_fcntl(int desc, int cmd, long arg) {
  if (desc < 0 || desc >= FILE_MAX) return -(long)EBADF;
  struct file *f = &fd_pool[desc];

  /* F_GETFL(3), F_SETFL(4) */
  switch (cmd) {
    case 3: /* F_GETFL */
      return (long)f->flags;
    case 4: /* F_SETFL */
      f->flags = (f->flags & O_ACCMODE) | ((uint32_t)arg & ~O_ACCMODE);
      return 0;
    default:
      return -(long)EINVAL;
  }
}

/* ── Pipe support ──────────────────────────────────────────────────────────── */

/* Called from pipe.c to allocate pool entries for pipe ends. */
int fd_pool_alloc_pipe(const struct file_ops *ops, void *priv,
                       uint32_t flags) {
  int desc = fd_pool_alloc();
  if (desc < 0) return -ENOMEM;
  fd_pool[desc].ops = ops;
  fd_pool[desc].priv = priv;
  fd_pool[desc].flags = flags;
  fd_pool[desc].refcnt = 1;
  fd_pool[desc].vnode = NULL;
  fd_pool[desc].offset = 0;
  return desc;
}

/* ── Wait channel (for poll blocking) ──────────────────────────────────────── */

void *vfs_fd_get_priv(int desc) {
  if (desc < 0 || desc >= FILE_MAX) return NULL;
  return fd_pool[desc].priv;
}

/* ── Compat: fd_stdio_init (transition wrapper) ────────────────────────────── */

void fd_stdio_init(pcb_t *p) {
  for (int i = 0; i < 3; i++)
    p->fd_map[i] = (int16_t)vfs_fd_stdio_desc(i);
}
