/*
 * vfs_mod_init.c — Core-side mod_vfs struct (i16 segment split)
 *
 * On i16, the mod_vfs struct lives in the core module and points
 * to the caller-side stubs (vfs_stubs.S). The stubs do the far
 * call to the VFS segment.
 *
 * On 32-bit, mod_vfs is defined in vfs.c and points directly to
 * the real functions. This file is only for i16.
 */

#ifdef __ia16__

#include "vfs/vfs_types.h"
#include "common/mod/module.h"

/* Forward-declare the caller-side stubs from vfs_stubs.S.
 * Signatures must match the mod_vfs_t struct fields. */
void vfs_init(void);
int  vfs_mount(const char *, const vfs_ops_t *, uint8_t, const void *);
int  vfs_umount(const char *);
int  vfs_lookup(const char *, vnode_t **);
int  vfs_lookup_flags(const char *, vnode_t **, int);
int  vfs_lookup_parent(const char *, vnode_t **, char *, int);
int  vfs_path_normalize(const char *, char *, int);
mount_entry_t *vfs_find_mount(const char *, const char **);
int  vfs_mount_ufs(const char *, uint8_t, const void *);
vnode_t *vfs_alloc_vnode(void);
void vfs_ref_vnode(vnode_t *);
void vfs_rel_vnode(vnode_t *);

struct pcb;
typedef struct pcb pcb_t;
void vfs_fd_stdio_init(pcb_t *);

/* Now include mod_vfs.h which declares mod_vfs_t */
#include "common/mod/mod_vfs.h"

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
  MOD_IMPL(vfs, ref_vnode)
  MOD_IMPL(vfs, rel_vnode)
  MOD_IMPL(vfs, fd_stdio_init)
MOD_DEFINE_END()

#endif /* __ia16__ */
