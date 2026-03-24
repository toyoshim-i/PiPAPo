/*
 * mod_vfs.h — VFS kernel module interface
 *
 * Defines the module boundary for the Virtual File System layer.
 * This is the API surface through which the rest of the kernel
 * (syscalls, exec, main.c) accesses VFS functionality.
 *
 * The VFS layer manages mount points, path resolution, and vnode
 * lifecycle.  It does NOT implement filesystem-specific logic — that
 * lives in fs/ drivers which register via vfs_mount() and operate
 * through the vfs_ops_t vtable.
 *
 * Usage:
 *   Callers:          #include "mod/mod_vfs.h"
 *   Implementation:   #define MOD_IMPLEMENTATION
 *                     #include "mod/mod_vfs.h"
 *
 * Implementation: src/kernel/vfs/vfs.c, src/kernel/vfs/namei.c
 */

/* Forward declarations (first include only) */
#ifndef PPAP_KERNEL_MOD_MOD_VFS_H
#define PPAP_KERNEL_MOD_MOD_VFS_H

struct vfs_ops;
typedef struct vfs_ops vfs_ops_t;
struct vnode;
typedef struct vnode vnode_t;
struct mount_entry;
typedef struct mount_entry mount_entry_t;

#endif /* PPAP_KERNEL_MOD_MOD_VFS_H */

#include "module.h"

MOD_DECLARE_BEGIN(vfs)

  /* ── VFS operations ──────────────────────────────────────────────────── */

  /*
   * init — Initialise the VFS layer.
   *
   * Sets up the mount table and vnode pool (kmem slab allocator).
   * Must be called once from kmain() before any filesystem mounts.
   */
  MOD_FUNC(vfs, void, init, void)

  /*
   * mount — Mount a filesystem at a given path.
   *
   *   path     Mount point (e.g. "/", "/dev", "/proc").
   *   ops      Filesystem driver vtable (romfs_ops, tmpfs_ops, etc.).
   *   flags    Mount flags (MNT_RDONLY, etc.).
   *   dev_data Opaque data passed to ops->mount() (e.g. romfs image
   *            pointer, block device, NULL for pseudo-filesystems).
   *
   * Returns 0 on success, negative errno on failure.
   */
  MOD_FUNC(vfs, int, mount, const char *, const vfs_ops_t *,
                             uint8_t, const void *)

  /*
   * umount — Unmount a filesystem.
   *
   *   path  Mount point to detach.
   *
   * Returns 0 on success, -ENOENT if not mounted, -EBUSY if in use.
   */
  MOD_FUNC(vfs, int, umount, const char *)

  /*
   * lookup — Resolve a pathname to a vnode.
   *
   *   path    Absolute or relative pathname (relative to current->cwd).
   *   result  Output: vnode pointer with incremented refcount.
   *           Caller must call vfs_rel_vnode() when done.
   *
   * Returns 0 on success, negative errno (-ENOENT, -ENOTDIR, etc.).
   */
  MOD_FUNC(vfs, int, lookup, const char *, vnode_t **)

  /*
   * lookup_flags — Resolve a pathname with flags.
   *
   * Same as lookup() but with additional flags:
   *   LOOKUP_NOFOLLOW  Do not follow terminal symlinks.
   *   LOOKUP_CREATE    Return parent dir if final component missing.
   */
  MOD_FUNC(vfs, int, lookup_flags, const char *, vnode_t **, int)

  /*
   * lookup_parent — Resolve the parent directory of a pathname.
   *
   *   path       Pathname to resolve.
   *   parent     Output: vnode of the parent directory.
   *   namebuf    Output: final path component (filename).
   *   namebuf_size  Size of namebuf.
   *
   * Used by sys_open(O_CREAT), sys_mkdir, sys_unlink, sys_rename.
   * Returns 0 on success, negative errno on failure.
   */
  MOD_FUNC(vfs, int, lookup_parent, const char *, vnode_t **,
                                     char *, int)

  /*
   * path_normalize — Normalize a pathname (resolve . and ..).
   *
   *   path   Input pathname.
   *   buf    Output buffer for normalized path.
   *   bufsiz Size of output buffer.
   *
   * Returns 0 on success, -ENAMETOOLONG if result doesn't fit.
   */
  MOD_FUNC(vfs, int, path_normalize, const char *, char *, int)

  /*
   * find_mount — Find the mount entry for a given path.
   *
   *   path       Absolute pathname to look up.
   *   remainder  Output: portion of path below the mount point.
   *
   * Returns the mount_entry_t with the longest matching prefix,
   * or NULL if no mount covers the path.
   */
  MOD_FUNC(vfs, mount_entry_t *, find_mount, const char *, const char **)

  /* ── Vnode lifecycle ─────────────────────────────────────────────────── */

  /*
   * alloc_vnode — Allocate a vnode from the pool.
   *
   * Returns a zeroed vnode with refcount 1, or NULL if the pool
   * is exhausted.  The caller must set vn->ops, vn->type, etc.
   * before making the vnode visible to other subsystems.
   *
   * Called by filesystem drivers during mount and lookup to create
   * in-memory representations of files, directories, and devices.
   */
  MOD_FUNC(vfs, vnode_t *, alloc_vnode, void)

  /*
   * ref_vnode — Increment a vnode's reference count.
   *
   *   vn  Vnode to reference.
   *
   * Called when a new file descriptor or directory entry points to
   * an existing vnode (e.g. dup, fork fd inheritance, hardlink).
   */
  MOD_FUNC(vfs, void, ref_vnode, vnode_t *)

  /*
   * rel_vnode — Release a vnode (decrement refcount, free if zero).
   *
   *   vn  Vnode to release.  Safe to call with NULL (no-op).
   *
   * When the refcount reaches zero, the vnode is returned to the
   * pool.  Called by sys_close, do_execve cleanup, process exit,
   * and any code that obtained a vnode via vfs_lookup.
   */
  MOD_FUNC(vfs, void, rel_vnode, vnode_t *)

MOD_DECLARE_END(vfs)

/*
 * When MOD_IMPLEMENTATION is defined, re-include this file in
 * implementation mode to generate the struct initializer from
 * the same MOD_FUNC list above.
 */
#ifdef MOD_IMPLEMENTATION
#undef MOD_IMPLEMENTATION
#define _MOD_IMPL_PHASE
#include "mod_vfs.h"
#endif
