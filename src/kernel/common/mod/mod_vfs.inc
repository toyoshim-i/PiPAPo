/*
 * mod_vfs.inc — VFS module function index (alphabetical order)
 *
 * Single source of truth for function names and their indices.
 * Included by C and assembly to prevent mismatches.
 *
 * Define MOD_VFS_ENTRY(name, idx) before including this file.
 * Entries are sorted alphabetically.  The index determines the
 * struct field position, far-pointer table slot, and entry-point
 * table offset.
 */

MOD_VFS_ENTRY(fd_acquire,       0)
MOD_VFS_ENTRY(fd_fcntl,         1)
MOD_VFS_ENTRY(fd_fstat,         2)
MOD_VFS_ENTRY(fd_fstatfs,       3)
MOD_VFS_ENTRY(fd_getdents,      4)
MOD_VFS_ENTRY(fd_getdents64,    5)
MOD_VFS_ENTRY(fd_get_priv,      6)
MOD_VFS_ENTRY(fd_ioctl,         7)
MOD_VFS_ENTRY(fd_lseek,         8)
MOD_VFS_ENTRY(fd_open,          9)
MOD_VFS_ENTRY(fd_pipe_create,  10)
MOD_VFS_ENTRY(fd_poll,         11)
MOD_VFS_ENTRY(fd_pool_init,   12)
MOD_VFS_ENTRY(fd_read,        13)
MOD_VFS_ENTRY(fd_release,     14)
MOD_VFS_ENTRY(fd_stdio_desc,  15)
MOD_VFS_ENTRY(fd_stdio_init,  16)
MOD_VFS_ENTRY(fd_write,       17)
MOD_VFS_ENTRY(fstab_automount, 18)
MOD_VFS_ENTRY(init,           19)
MOD_VFS_ENTRY(lookup,         20)
MOD_VFS_ENTRY(lookup_flags,   21)
MOD_VFS_ENTRY(lookup_parent,  22)
MOD_VFS_ENTRY(mount,          23)
MOD_VFS_ENTRY(mount_by_fstype, 24)
MOD_VFS_ENTRY(mount_find,     25)
MOD_VFS_ENTRY(mount_romfs,    26)
MOD_VFS_ENTRY(mount_ufs,      27)
MOD_VFS_ENTRY(path_normalize, 28)
MOD_VFS_ENTRY(tty_rx_notify,  29)
MOD_VFS_ENTRY(umount,         30)
MOD_VFS_ENTRY(vnode_acquire,  31)
MOD_VFS_ENTRY(vnode_alloc,    32)
MOD_VFS_ENTRY(vnode_read,     33)
MOD_VFS_ENTRY(vnode_readlink, 34)
MOD_VFS_ENTRY(vnode_release,  35)
MOD_VFS_ENTRY(vnode_stat,     36)

#ifndef MOD_VFS_FUNC_COUNT
#define MOD_VFS_FUNC_COUNT 37
#endif
