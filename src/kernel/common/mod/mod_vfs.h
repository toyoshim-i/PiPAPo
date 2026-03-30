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
 *   Callers:          #include "common/mod/mod_vfs.h"
 *   Implementation:   #define MOD_IMPLEMENTATION
 *                     #include "common/mod/mod_vfs.h"
 *
 * Implementation: src/kernel/vfs/vfs.c, src/kernel/vfs/namei.c
 */

#ifndef PPAP_KERNEL_MOD_MOD_VFS_H
#define PPAP_KERNEL_MOD_MOD_VFS_H

/* Full VFS type definitions */
#include "../../vfs/vfs_types.h"

#include "module.h"

/* Forward declaration for fd_stdio_init parameter */
struct pcb;
typedef struct pcb pcb_t;

MOD_DECLARE_BEGIN(vfs)

  /* Fields are sorted alphabetically to match mod_vfs.inc indices. */

  MOD_FUNC(vfs, void, fd_acquire, int)
  MOD_FUNC(vfs, long, fd_fcntl, int, int, long)
  MOD_FUNC(vfs, int, fd_fstat, int, void *)
  MOD_FUNC(vfs, int, fd_fstatfs, int, void *)
  MOD_FUNC(vfs, long, fd_getdents, int, void *, size_t)
  MOD_FUNC(vfs, long, fd_getdents64, int, void *, long)
  MOD_FUNC(vfs, void *, fd_get_priv, int)
  MOD_FUNC(vfs, int, fd_ioctl, int, uint32_t, void *)
  MOD_FUNC(vfs, long, fd_lseek, int, long, int)
  MOD_FUNC(vfs, int, fd_open, const char *, int, int)
  MOD_FUNC(vfs, int, fd_pipe_create, int *, int *)
  MOD_FUNC(vfs, int, fd_poll, int)
  MOD_FUNC(vfs, void, fd_pool_init, void)
  MOD_FUNC(vfs, long, fd_read, int, char *, size_t)
  MOD_FUNC(vfs, void, fd_release, int)
  MOD_FUNC(vfs, int, fd_stdio_desc, int)
  MOD_FUNC(vfs, void, fd_stdio_init, pcb_t *)
  MOD_FUNC(vfs, long, fd_write, int, const char *, size_t)
  MOD_FUNC(vfs, void, fstab_automount, void)
  MOD_FUNC(vfs, void, init, void)
  MOD_FUNC(vfs, int, lookup, const char *, vnode_t **)
  MOD_FUNC(vfs, int, lookup_flags, const char *, vnode_t **, int)
  MOD_FUNC(vfs, int, lookup_parent, const char *, vnode_t **,
                                     char *, int)
  MOD_FUNC(vfs, int, mount, const char *, const vfs_ops_t *,
                             uint8_t, const void *)
  MOD_FUNC(vfs, int, mount_by_fstype, const char *, const char *,
                                       const char *, long)
  MOD_FUNC(vfs, mount_entry_t *, mount_find, const char *, const char **)
  MOD_FUNC(vfs, int, mount_romfs, const char *, uint8_t, const void *)
  MOD_FUNC(vfs, int, mount_ufs, const char *, uint8_t, const void *)
  MOD_FUNC(vfs, int, path_normalize, const char *, char *, int)
  MOD_FUNC(vfs, void, tty_rx_notify, int)
  MOD_FUNC(vfs, int, umount, const char *)
  MOD_FUNC(vfs, void, vnode_acquire, vnode_t *)
  MOD_FUNC(vfs, vnode_t *, vnode_alloc, void)
  MOD_FUNC(vfs, long, vnode_read, vnode_t *, void *, uint32_t, uint32_t)
  MOD_FUNC(vfs, void, vnode_release, vnode_t *)

MOD_DECLARE_END(vfs)

/* MOD_VFS_FUNC_COUNT is defined in mod_vfs.inc — the single source
 * of truth shared by both C and assembly stubs.  The _Static_assert
 * below catches mismatches between this struct and the .inc file.
 * To add a new function: update mod_vfs.h (types) AND mod_vfs.inc
 * (name + index).  Assembly stubs auto-generate from the .inc file. */
#define MOD_VFS_ENTRY(name, idx) /* count only */
#include "mod_vfs.inc"
#undef MOD_VFS_ENTRY
_Static_assert(sizeof(mod_vfs_t) == MOD_VFS_FUNC_COUNT * sizeof(void (*)(void)),
               "mod_vfs_t size mismatch — update MOD_VFS_FUNC_COUNT in "
               "mod_vfs.inc");

#endif /* PPAP_KERNEL_MOD_MOD_VFS_H */
