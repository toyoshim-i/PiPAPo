/*
 * vfs.h — VFS module-internal header
 *
 * PRIVATE to src/kernel/vfs/.  Only vfs.c and namei.c may include this.
 * External callers use mod/mod_vfs.h instead.
 *
 * Provides VFS types (via vfs_types.h) plus internal function declarations.
 */

#ifndef PPAP_KERNEL_VFS_VFS_H
#define PPAP_KERNEL_VFS_VFS_H

/* All VFS types (vnode_t, vfs_ops_t, mount_entry_t, etc.) */
#include "vfs_types.h"

/* ── VFS internal function declarations ───────────────────────────────────
 *
 * These are the real function prototypes.  External callers access them
 * via the mod_vfs struct (see mod/mod_vfs.h).
 */

/* Lookup flags for vfs_lookup_flags() */
#define VFS_LOOKUP_NOFOLLOW 0x01 /* don't follow final symlink */

void vfs_init(void);
int vfs_mount(const char *path, const vfs_ops_t *ops, uint8_t flags,
              const void *dev_data);
int vfs_umount(const char *path);
vnode_t *vfs_alloc_vnode(void);
void vfs_ref_vnode(vnode_t *vn);
void vfs_rel_vnode(vnode_t *vn);
int vfs_lookup(const char *path, vnode_t **result);
int vfs_lookup_flags(const char *path, vnode_t **result, int flags);
int vfs_lookup_parent(const char *path, vnode_t **parent, char *namebuf,
                      int namebuf_size);
int vfs_path_normalize(const char *path, char *buf, int bufsiz);
mount_entry_t *vfs_find_mount(const char *path, const char **remainder);
uint32_t vnode_free_count(void);

/* Read-only access to the mount table (for procfs /proc/mounts) */
extern mount_entry_t vfs_mount_table[VFS_MOUNT_MAX];

#endif /* PPAP_KERNEL_VFS_VFS_H */
