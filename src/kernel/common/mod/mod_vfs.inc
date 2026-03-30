/*
 * mod_vfs_funcs.inc — VFS module function index
 *
 * Single source of truth for function names and their indices.
 * Included by C (vfs_mod_init.c, mod_vfs.h) and assembly
 * (vfs_stubs.S, vfs_entries.S, vfs_header.S) to prevent mismatches.
 *
 * Define MOD_VFS_ENTRY(name, idx) before including this file.
 *
 * When adding a new function:
 *   1. Add MOD_FUNC(...) in mod_vfs.h (with types and comments)
 *   2. Add MOD_VFS_ENTRY(name, idx) here (increment idx)
 *   3. Update MOD_VFS_FUNC_COUNT below
 *   4. Add implementation + MOD_IMPL in vfs.c or fd.c
 *   The _Static_assert will catch any mismatch.
 */

MOD_VFS_ENTRY(init,            0)
MOD_VFS_ENTRY(mount,           1)
MOD_VFS_ENTRY(umount,          2)
MOD_VFS_ENTRY(lookup,          3)
MOD_VFS_ENTRY(lookup_flags,    4)
MOD_VFS_ENTRY(lookup_parent,   5)
MOD_VFS_ENTRY(path_normalize,  6)
MOD_VFS_ENTRY(mount_find,      7)
MOD_VFS_ENTRY(mount_ufs,       8)
MOD_VFS_ENTRY(vnode_alloc,     9)
MOD_VFS_ENTRY(vnode_acquire,  10)
MOD_VFS_ENTRY(vnode_release,  11)
MOD_VFS_ENTRY(fd_stdio_init,  12)
MOD_VFS_ENTRY(vnode_read,      13)
MOD_VFS_ENTRY(fd_pool_init, 14)
MOD_VFS_ENTRY(tty_rx_notify,  15)
MOD_VFS_ENTRY(fstab_automount, 16)
MOD_VFS_ENTRY(fd_open,        17)
MOD_VFS_ENTRY(fd_release,     18)
MOD_VFS_ENTRY(fd_acquire,     19)
MOD_VFS_ENTRY(fd_pipe_create, 20)
MOD_VFS_ENTRY(fd_stdio_desc,  21)
MOD_VFS_ENTRY(fd_read,        22)
MOD_VFS_ENTRY(fd_write,       23)
MOD_VFS_ENTRY(fd_ioctl,       24)
MOD_VFS_ENTRY(fd_poll,        25)
MOD_VFS_ENTRY(fd_lseek,       26)
MOD_VFS_ENTRY(fd_fstat,       27)
MOD_VFS_ENTRY(fd_getdents,    28)
MOD_VFS_ENTRY(fd_getdents64,  29)
MOD_VFS_ENTRY(fd_fstatfs,     30)
MOD_VFS_ENTRY(fd_fcntl,       31)
MOD_VFS_ENTRY(fd_get_priv,    32)

#ifndef MOD_VFS_FUNC_COUNT
#define MOD_VFS_FUNC_COUNT 33
#endif
