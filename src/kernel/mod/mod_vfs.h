/*
 * mod_vfs.h — VFS kernel module interface
 *
 * Defines the module boundary for the Virtual File System layer.
 * This is the API surface through which the rest of the kernel
 * (syscalls, exec, main.c) accesses VFS functionality.
 *
 * The VFS layer manages mount points and path resolution.  It does
 * NOT implement filesystem-specific logic — that lives in fs/ drivers
 * drivers which register via vfs_mount() and operate through the
 * vfs_ops_t vtable.
 *
 * On 32-bit platforms, these are fields in a function-pointer struct
 * (mod_vfs), enabling future far-call dispatch on i16.
 * On i16, these are plain extern declarations (direct calls).
 *
 * Callers:
 *   32-bit: mod_vfs.init(), mod_vfs.lookup(path, &vn), etc.
 *   i16:    vfs_init(), vfs_lookup(path, &vn), etc.
 *
 * Implementation: src/kernel/vfs/vfs.c, src/kernel/vfs/namei.c
 */

#ifndef PPAP_KERNEL_MOD_MOD_VFS_H
#define PPAP_KERNEL_MOD_MOD_VFS_H

#include "module.h"

/* Forward declarations to avoid pulling in the full vfs.h */
struct vfs_ops;
typedef struct vfs_ops vfs_ops_t;
struct vnode;
typedef struct vnode vnode_t;
struct mount_entry;
typedef struct mount_entry mount_entry_t;

MOD_DECLARE_BEGIN(vfs)

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
   *           Caller must call vnode_put() when done.
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

MOD_DECLARE_END(vfs)

#endif /* PPAP_KERNEL_MOD_MOD_VFS_H */
