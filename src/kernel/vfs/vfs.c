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
 * The mount table is a simple fixed-size array.  vfs_find_mount() does a
 * longest-prefix match so that "/dev/ttyS0" resolves to the "/dev" mount
 * rather than "/".
 */

#include "vfs.h"

#include <stddef.h>

#include "../common/errno.h"
#include "../common/mod/mod_core.h"
#include "../fd/file.h"   /* file_pool_init */
#include "../fd/tty.h"    /* tty_rx_notify */
#include "../fs/fstab.h"  /* fstab_parse, fstab_mount_all */
#include "../mm/kmem.h" /* kmem_pool_t type — functions via mod_core */
#include "../common/spinlock.h" /* SPIN_VFS */

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
  /* Zero the mount table (BSS guarantees this, but be explicit) */
  for (int i = 0; i < VFS_MOUNT_MAX; i++) vfs_mount_table[i].active = 0;
  mount_count = 0;

  /* Initialise the vnode slab pool */
  mod_core.kmem_pool_init(&vnode_pool, vnode_storage, sizeof(vnode_t), VFS_VNODE_MAX);

  mod_core.klogf("VFS: initialised (%u vnodes, %u mount slots)\n",
        (unsigned)VFS_VNODE_MAX, (unsigned)VFS_MOUNT_MAX);
}

/* ── vfs_alloc_vnode / vfs_acquire_vnode / vfs_release_vnode ─────────────────────────────────────
 */

vnode_t *vfs_alloc_vnode(void) {
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

void vfs_acquire_vnode(vnode_t *vn) {
  if (!vn) return;
  uint32_t saved = spin_lock_irqsave(SPIN_VFS);
  vn->refcnt++;
  spin_unlock_irqrestore(SPIN_VFS, saved);
}

void vfs_release_vnode(vnode_t *vn) {
  if (!vn) return;
  uint32_t saved = spin_lock_irqsave(SPIN_VFS);
  if (vn->refcnt > 0) vn->refcnt--;
  if (vn->refcnt == 0) mod_core.kmem_free(&vnode_pool, vn);
  spin_unlock_irqrestore(SPIN_VFS, saved);
}

uint32_t vnode_free_count(void) { return mod_core.kmem_free_count(&vnode_pool); }

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
   * The callback may call vfs_alloc_vnode() which also acquires SPIN_VFS —
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
   * Note: vfs_release_vnode() also acquires SPIN_VFS, but we already hold it.
   * Use mod_core.kmem_free() directly to avoid recursive lock. */
  if (mnt->root) {
    if (mnt->root->refcnt > 0) mnt->root->refcnt--;
    if (mnt->root->refcnt == 0) mod_core.kmem_free(&vnode_pool, mnt->root);
  }
  mnt->root = NULL;
  mnt->active = 0;
  mount_count--;

  spin_unlock_irqrestore(SPIN_VFS, saved);

  mod_core.klogf("VFS: unmounted %s\n", path);

  return 0;
}

/* ── vfs_find_mount ─────────────────────────────────────────────────────────
 */

mount_entry_t *vfs_find_mount(const char *path, const char **remainder) {
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

#include "../fs/ufs.h"

int vfs_mount_ufs(const char *path, uint8_t flags, const void *dev_data)
{
  return vfs_mount(path, &ufs_ops, flags, dev_data);
}

/* ── Module definition ─────────────────────────────────────────────────── */

#include "../common/mod/mod_vfs.h"
#include "../fd/fd.h"

/* Aliases for MOD_IMPL convention: vfs_<name> → <name> */
#define vfs_file_pool_init file_pool_init
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
extern long vfs_fd_read(int, char *, size_t);
extern long vfs_fd_write(int, const char *, size_t);
extern int vfs_fd_ioctl(int, uint32_t, void *);
extern int vfs_fd_poll(int);
extern long vfs_fd_lseek(int, long, int);
extern int vfs_fd_fstat(int, void *);
extern long vfs_fd_getdents(int, void *, size_t);
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
    mod_core.klogf("fstab: %lu entries parsed\n",
                   (unsigned long)(uint32_t)n);
    fstab_mount_all(fstab, n);
  } else {
    mod_core.klog("fstab: no entries\n");
  }
}

/* Cross-module wrapper: execute ops->read in VFS's code segment. */
long vfs_file_read(vnode_t *vn, void *buf, uint32_t size, uint32_t off) {
  if (!vn || !vn->mount || !vn->mount->ops || !vn->mount->ops->read)
    return -2; /* ENOENT */
  return vn->mount->ops->read(vn, buf, size, off);
}

MOD_DEFINE_BEGIN(vfs)
  MOD_IMPL(vfs, init)
  MOD_IMPL(vfs, mount)
  MOD_IMPL(vfs, umount)
  MOD_IMPL(vfs, lookup)
  MOD_IMPL(vfs, lookup_flags)
  MOD_IMPL(vfs, lookup_parent)
  MOD_IMPL(vfs, path_normalize)
  MOD_IMPL(vfs, find_mount)
  MOD_IMPL(vfs, mount_ufs)
  MOD_IMPL(vfs, alloc_vnode)
  MOD_IMPL(vfs, acquire_vnode)
  MOD_IMPL(vfs, release_vnode)
  MOD_IMPL(vfs, fd_stdio_init)
  MOD_IMPL(vfs, file_read)
  MOD_IMPL(vfs, file_pool_init)
  MOD_IMPL(vfs, tty_rx_notify)
  MOD_IMPL(vfs, fstab_automount)
  MOD_IMPL(vfs, fd_open)
  MOD_IMPL(vfs, fd_release)
  MOD_IMPL(vfs, fd_acquire)
  MOD_IMPL(vfs, fd_pipe_create)
  MOD_IMPL(vfs, fd_stdio_desc)
  MOD_IMPL(vfs, fd_read)
  MOD_IMPL(vfs, fd_write)
  MOD_IMPL(vfs, fd_ioctl)
  MOD_IMPL(vfs, fd_poll)
  MOD_IMPL(vfs, fd_lseek)
  MOD_IMPL(vfs, fd_fstat)
  MOD_IMPL(vfs, fd_getdents)
  MOD_IMPL(vfs, fd_getdents64)
  MOD_IMPL(vfs, fd_fstatfs)
  MOD_IMPL(vfs, fd_fcntl)
  MOD_IMPL(vfs, fd_get_priv)
MOD_DEFINE_END()
