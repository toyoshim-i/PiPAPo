/*
 * vfs.h — VFS module-internal header
 *
 * PRIVATE to src/kernel/vfs/.  Only vfs.c and namei.c should include this.
 * External callers use mod/mod_vfs.h instead.
 *
 * Provides VFS types (via vfs_types.h) plus internal function declarations.
 */

#ifndef PPAP_KERNEL_VFS_VFS_H
#define PPAP_KERNEL_VFS_VFS_H

#include "vfs_types.h"

/* Lookup flags */
#define VFS_LOOKUP_NOFOLLOW 0x01

/* Internal function declarations — external callers use mod_vfs struct */
void vfs_init(void);
int vfs_mount(const char *path, const vfs_ops_t *ops, uint8_t flags,
              const void *dev_data);
int vfs_umount(const char *path);
vnode_t *vfs_vnode_alloc(void);
void vfs_vnode_acquire(vnode_t *vn);
void vfs_vnode_release(vnode_t *vn);
int vfs_lookup(const char *path, vnode_t **result);
int vfs_lookup_flags(const char *path, vnode_t **result, int flags);
int vfs_lookup_parent(const char *path, vnode_t **parent, char *namebuf,
                      int namebuf_size);
int vfs_path_normalize(const char *path, char *buf, int bufsiz);
mount_entry_t *vfs_mount_find(const char *path, const char **remainder);
uint32_t vnode_free_count(void);

#endif /* PPAP_KERNEL_VFS_VFS_H */
