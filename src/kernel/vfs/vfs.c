/*
 * vfs.c — Virtual File System layer implementation
 *
 * Manages the mount table and vnode pool.  FS-specific drivers (romfs,
 * devfs, procfs) register via vfs_mount().  Path resolution (namei) and
 * VFS-routed syscalls are added in Phase 2 Steps 2-3.
 *
 * The vnode pool is a statically-allocated slab managed by the kmem
 * allocator — O(1) alloc/free with no per-vnode metadata overhead.
 *
 * The mount table is a simple fixed-size array.  vfs_mount_find() does a
 * longest-prefix match so that "/dev/ttyS0" resolves to the "/dev" mount
 * rather than "/".
 */

#include "kernel/vfs/vfs.h"

#include <stddef.h>

#include "common/errno.h"
#include "kernel/common/core/kmem_types.h" /* kmem_pool_t */
#include "kernel/common/mod/mod_core.h"
#include "kernel/common/spinlock.h" /* SPIN_VFS */
#include "kernel/vfs/file.h"        /* fd_pool_init */
#include "kernel/vfs/fstab.h"       /* fstab_parse, fstab_mount_all */
#include "kernel/vfs/klog.h"
#include "kernel/vfs/tty.h" /* tty_rx_notify */

/* ── Static storage ─────────────────────────────────────────────────────────
 */

/* Mount table — up to VFS_MOUNT_MAX entries.  Zero-initialised by BSS. */
mount_entry_t vfs_mount_table[VFS_MOUNT_MAX];

/* Vnode pool — VFS_VNODE_MAX objects managed by kmem. */
static vnode_t vnode_storage[VFS_VNODE_MAX];
static kmem_pool_t vnode_pool;

/* Number of active mounts (for diagnostics). */
static uint32_t mount_count;

/* ── vfs_init ───────────────────────────────────────────────────────────────
 */

void vfs_init(void) {
  /* Register UART/display loggers before any klogf output.
   * Target provides klog_init_logger() to call uart_init() +
   * klog_set_logger() — all VFS-side now, no far call needed on i16. */
  klog_init_logger();

  /* Zero the mount table (BSS guarantees this, but be explicit) */
  for (int i = 0; i < VFS_MOUNT_MAX; i++) vfs_mount_table[i].active = 0;
  mount_count = 0;

  /* Initialise the vnode slab pool */
  mod_core.kmem_pool_init(&vnode_pool, vnode_storage, sizeof(vnode_t),
                          VFS_VNODE_MAX);
  vfs_namei_init();

  /* Block device registry + loopback subsystem */
#ifdef PPAP_HAS_BLKDEV
  extern void blkdev_init(void);
  blkdev_init();
#if !defined(__ia16__)
  extern void loopback_init(void);
  loopback_init();
#endif
#endif

  klogf("VFS: initialised (%u vnodes, %u mount slots)\n",
        (unsigned)VFS_VNODE_MAX, (unsigned)VFS_MOUNT_MAX);
}

/* ── vfs_vnode_alloc / vfs_vnode_acquire / vfs_vnode_release
 * ─────────────────────────────────────
 */

vnode_t *vfs_vnode_alloc(void) {
  uint32_t saved = spin_lock_irqsave(SPIN_VFS);
  vnode_t *vn = mod_core.kmem_alloc(&vnode_pool);
  if (!vn) {
    spin_unlock_irqrestore(SPIN_VFS, saved);
    return NULL;
  }
  /* Zero the vnode and set initial refcnt */
  vn->type = VNODE_FILE;
  vn->size = 0;
  vn->mode = 0;
  vn->ino = 0;
  vn->refcnt = 1;
  vn->fs_priv = NULL;
  vn->mount = NULL;
  vn->xip_addr = NULL;
  spin_unlock_irqrestore(SPIN_VFS, saved);
  return vn;
}

void vfs_vnode_acquire(vnode_t *vn) {
  if (!vn) return;
  uint32_t saved = spin_lock_irqsave(SPIN_VFS);
  vn->refcnt++;
  spin_unlock_irqrestore(SPIN_VFS, saved);
}

void vfs_vnode_release(vnode_t *vn) {
  if (!vn) return;
  uint32_t saved = spin_lock_irqsave(SPIN_VFS);
  if (vn->refcnt > 0) vn->refcnt--;
  if (vn->refcnt == 0) mod_core.kmem_free(&vnode_pool, vn);
  spin_unlock_irqrestore(SPIN_VFS, saved);
}

uint32_t vnode_free_count(void) {
  return mod_core.kmem_free_count(&vnode_pool);
}

/* ── vfs_mount ──────────────────────────────────────────────────────────────
 */

int vfs_mount(const char *path, const vfs_ops_t *ops, uint8_t flags,
              const void *dev_data) {
  if (!path || !ops) return -EINVAL;

  uint32_t saved = spin_lock_irqsave(SPIN_VFS);

  /* Find a free slot */
  mount_entry_t *mnt = NULL;
  for (int i = 0; i < VFS_MOUNT_MAX; i++) {
    if (!vfs_mount_table[i].active) {
      mnt = &vfs_mount_table[i];
      break;
    }
  }
  if (!mnt) {
    spin_unlock_irqrestore(SPIN_VFS, saved);
    return -ENOMEM;
  }

  /* Copy the mount point path */
  size_t plen = __builtin_strlen(path);
  if (plen >= VFS_PATH_MAX) {
    spin_unlock_irqrestore(SPIN_VFS, saved);
    return -ENAMETOOLONG;
  }

  /* Strip trailing '/' except for the root mount */
  while (plen > 1 && path[plen - 1] == '/') plen--;

  __builtin_memcpy(mnt->path, path, plen);
  mnt->path[plen] = '\0';
  mnt->path_len = (uint8_t)plen;
  mnt->flags = flags;
  mnt->ops = ops;
  mnt->root = NULL;
  mnt->sb_priv = NULL;

  /* Release SPIN_VFS before calling the FS mount callback.
   * The callback may call vfs_vnode_alloc() which also acquires SPIN_VFS —
   * RP2040 hardware spinlocks are NOT re-entrant (same-core re-acquire
   * returns 0 → infinite spin).  The slot is safe: it's not yet active,
   * so no other code path will find or modify it. */
  spin_unlock_irqrestore(SPIN_VFS, saved);

  /* Let the FS driver initialise */
  int err = 0;
  if (ops->mount) err = ops->mount(mnt, dev_data);
  if (err) {
    uint32_t s2 = spin_lock_irqsave(SPIN_VFS);
    mnt->active = 0;
    spin_unlock_irqrestore(SPIN_VFS, s2);
    return err;
  }

  saved = spin_lock_irqsave(SPIN_VFS);
  mnt->active = 1;
  mount_count++;
  spin_unlock_irqrestore(SPIN_VFS, saved);

  return 0;
}

/* ── vfs_umount ──────────────────────────────────────────────────────────── */

int vfs_umount(const char *path) {
  if (!path) return -EINVAL;

  /* Cannot unmount root */
  if (path[0] == '/' && path[1] == '\0') return -EINVAL;

  uint32_t saved = spin_lock_irqsave(SPIN_VFS);

  /* Find the mount entry matching path exactly */
  mount_entry_t *mnt = NULL;
  for (int i = 0; i < VFS_MOUNT_MAX; i++) {
    mount_entry_t *m = &vfs_mount_table[i];
    if (!m->active) continue;
    if (__builtin_strcmp(m->path, path) == 0) {
      mnt = m;
      break;
    }
  }
  if (!mnt) {
    spin_unlock_irqrestore(SPIN_VFS, saved);
    return -ENOENT;
  }

  /* Check no vnodes still reference this mount (scan vnode pool) */
  for (int i = 0; i < VFS_VNODE_MAX; i++) {
    if (vnode_storage[i].refcnt > 0 && vnode_storage[i].mount == mnt &&
        &vnode_storage[i] != mnt->root) {
      spin_unlock_irqrestore(SPIN_VFS, saved);
      return -EBUSY;
    }
  }

  /* Release root vnode and deactivate.
   * Note: vfs_vnode_release() also acquires SPIN_VFS, but we already hold it.
   * Use mod_core.kmem_free() directly to avoid recursive lock. */
  if (mnt->root) {
    if (mnt->root->refcnt > 0) mnt->root->refcnt--;
    if (mnt->root->refcnt == 0) mod_core.kmem_free(&vnode_pool, mnt->root);
  }
  mnt->root = NULL;
  mnt->active = 0;
  mount_count--;

  spin_unlock_irqrestore(SPIN_VFS, saved);

  klogf("VFS: unmounted %s\n", path);

  return 0;
}

/* ── vfs_mount_find ─────────────────────────────────────────────────────────
 */

mount_entry_t *vfs_mount_find(const char *path, const char **remainder) {
  mount_entry_t *best = NULL;
  uint8_t best_len = 0;

  for (int i = 0; i < VFS_MOUNT_MAX; i++) {
    mount_entry_t *m = &vfs_mount_table[i];
    if (!m->active) continue;

    uint8_t mlen = m->path_len;

    /* Root mount "/" matches everything */
    if (mlen == 1 && m->path[0] == '/') {
      if (!best || mlen > best_len) {
        best = m;
        best_len = mlen;
      }
      continue;
    }

    /* Check if the mount path is a prefix of the lookup path.
     * The character after the prefix must be '/' or '\0' to avoid
     * matching "/dev" against "/device". */
    if (__builtin_strncmp(path, m->path, mlen) == 0) {
      char next = path[mlen];
      if (next == '/' || next == '\0') {
        if (mlen > best_len) {
          best = m;
          best_len = mlen;
        }
      }
    }
  }

  if (best && remainder) {
    const char *r = path + best_len;
    /* Skip leading '/' in the remainder */
    while (*r == '/') r++;
    *remainder = r;
  }

  return best;
}

/* ── Convenience mount wrappers ────────────────────────────────────────── */

#include "kernel/vfs/devfs.h"
#include "kernel/vfs/procfs.h"
#include "kernel/vfs/romfs.h"
#include "kernel/vfs/tmpfs.h"
#include "kernel/vfs/ufs.h"
#ifdef PPAP_HAS_BLKDEV
#include "kernel/vfs/driver/blkdev.h"
#include "kernel/vfs/vfat.h"
#endif

#if defined(PPAP_HAS_BLKDEV) || defined(PPAP_HAS_UFS)
int vfs_mount_ufs(const char *path, uint8_t flags, const void *dev_data) {
  return vfs_mount(path, &ufs_ops, flags, dev_data);
}
#else
int vfs_mount_ufs(const char *path, uint8_t flags, const void *dev_data) {
  (void)path;
  (void)flags;
  (void)dev_data;
  return -ENODEV;
}
#endif

int vfs_mount_romfs(const char *path, uint8_t flags, const void *dev_data) {
  return vfs_mount(path, &romfs_ops, flags, dev_data);
}

/* ── String helper for mount_by_fstype ─────────────────────────────────── */

static int fs_str_eq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (*a == '\0' && *b == '\0');
}

#define MS_RDONLY 1u

int vfs_mount_by_fstype(const char *source, const char *target,
                        const char *fstype, long flags) {
  if (!target || !fstype) return -EINVAL;

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
      if (!bd) return -ENODEV;
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
      if (!bd) return -ENODEV;
      dev_data = bd;
    }
  }
#endif
  else {
    return -ENODEV;
  }

  uint8_t mnt_flags = 0;
  if ((uint32_t)flags & MS_RDONLY) mnt_flags |= MNT_RDONLY;

  return vfs_mount(target, ops, mnt_flags, dev_data);
}

/* ── Module definition ─────────────────────────────────────────────────── */

#include "kernel/common/mod/mod_vfs.h"
#include "kernel/vfs/fd.h"

/* Aliases for MOD_IMPL convention: vfs_<name> → <name> */
#define vfs_fd_pool_init fd_pool_init
#define vfs_tty_rx_notify tty_rx_notify

/* fd.c: fd_stdio_init takes pcb_t* (compat wrapper) */
struct pcb;
typedef struct pcb pcb_t;
extern void fd_stdio_init(pcb_t *);
#define vfs_fd_stdio_init fd_stdio_init

/* fd.c pool functions — already named vfs_fd_* */
extern int vfs_fd_open(const char *, int, int);
extern void vfs_fd_release(int);
extern void vfs_fd_acquire(int);
extern int vfs_fd_stdio_desc(int);
extern long vfs_fd_read(int, page_id_t, uint16_t, size_t);
extern long vfs_fd_write(int, page_id_t, uint16_t, size_t);
extern int vfs_fd_ioctl(int, uint32_t, void *);
extern int vfs_fd_poll(int);
extern long vfs_fd_lseek(int, long, int);
extern int vfs_fd_fstat(int, void *);
extern long vfs_fd_getdents(int, page_id_t, uint16_t, size_t);
extern long vfs_fd_getdents64(int, void *, long);
extern int vfs_fd_fstatfs(int, void *);
extern long vfs_fd_fcntl(int, int, long);
extern void *vfs_fd_get_priv(int);

/* pipe.c */
extern int vfs_fd_pipe_create(int *, int *);

/* Combined fstab parse + mount behind the module boundary. */
void vfs_fstab_automount(void) {
  fstab_entry_t fstab[FSTAB_MAX_ENTRIES];
  int n = fstab_parse(fstab, FSTAB_MAX_ENTRIES);
  if (n > 0) {
    klogf("fstab: %lu entries parsed\n", (unsigned long)(uint32_t)n);
    fstab_mount_all(fstab, n);
  } else {
    klogf("fstab: no entries\n");
  }
}

/* Cross-module wrapper: execute ops->read in VFS's code segment. */
long vfs_vnode_read(vnode_t *vn, page_id_t page, uint16_t page_off,
                    uint32_t size, uint32_t off) {
  if (!vn || !vn->mount || !vn->mount->ops || !vn->mount->ops->read)
    return -2; /* ENOENT */
  return vn->mount->ops->read(vn, page, page_off, size, off);
}

int vfs_vnode_stat(vnode_t *vn, void *st) {
  if (!vn || !st || !vn->mount || !vn->mount->ops || !vn->mount->ops->stat)
    return -2; /* ENOENT / unsupported */
  return vn->mount->ops->stat(vn, st);
}

long vfs_vnode_readlink(vnode_t *vn, char *buf, size_t bufsiz) {
  if (!vn || !buf || !vn->mount || !vn->mount->ops || !vn->mount->ops->readlink)
    return -2; /* ENOENT / unsupported */
  return vn->mount->ops->readlink(vn, buf, bufsiz);
}

/* klog alias — defined in klog.c, needed by MOD_IMPL(vfs, klogf) */
extern void vfs_klogf(const char *, ...);

/* Weak default notify — targets override for PLL/TTY/input events */
__attribute__((weak)) void vfs_notify(int event) {
  if (event == VFS_EVENT_MODULE_READY) klog_init_logger();
  if (event == VFS_EVENT_IDLE) tty_poll_input();
}

MOD_DEFINE_BEGIN(vfs)
#define MOD_VFS_ENTRY(name, idx) MOD_IMPL(vfs, name)
#include "kernel/common/mod/mod_vfs.inc"
#undef MOD_VFS_ENTRY
MOD_DEFINE_END()
