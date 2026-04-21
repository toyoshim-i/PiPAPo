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
MOD_VFS_ENTRY(klogf,          20)
MOD_VFS_ENTRY(lookup,         21)
MOD_VFS_ENTRY(lookup_flags,   22)
MOD_VFS_ENTRY(lookup_parent,  23)
MOD_VFS_ENTRY(mount,          24)
MOD_VFS_ENTRY(mount_by_fstype, 25)
MOD_VFS_ENTRY(mount_find,     26)
MOD_VFS_ENTRY(mount_romfs,    27)
MOD_VFS_ENTRY(mount_ufs,      28)
MOD_VFS_ENTRY(notify,         29)
MOD_VFS_ENTRY(path_mkdir,     30)
MOD_VFS_ENTRY(path_normalize, 31)
MOD_VFS_ENTRY(path_rename,    32)
MOD_VFS_ENTRY(path_statfs,    33)
MOD_VFS_ENTRY(path_unlink,    34)
MOD_VFS_ENTRY(path_utimes,    35)
MOD_VFS_ENTRY(tty_rx_notify,  36)
MOD_VFS_ENTRY(umount,         37)
MOD_VFS_ENTRY(vnode_acquire,  38)
MOD_VFS_ENTRY(vnode_alloc,    39)
MOD_VFS_ENTRY(vnode_read,     40)
MOD_VFS_ENTRY(vnode_readlink, 41)
MOD_VFS_ENTRY(vnode_release,  42)
MOD_VFS_ENTRY(vnode_stat,     43)

#ifndef MOD_VFS_FUNC_COUNT
#define MOD_VFS_FUNC_COUNT 44
#endif
