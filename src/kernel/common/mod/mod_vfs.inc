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

MOD_VFS_ENTRY(fd_acquire,        0)
MOD_VFS_ENTRY(fd_fcntl,          1)
MOD_VFS_ENTRY(fd_fstat,          2)
MOD_VFS_ENTRY(fd_fstatfs,        3)
MOD_VFS_ENTRY(fd_getdents,       4)
MOD_VFS_ENTRY(fd_getdents64,     5)
MOD_VFS_ENTRY(fd_get_priv,       6)
MOD_VFS_ENTRY(fd_ioctl,          7)
MOD_VFS_ENTRY(fd_lseek,          8)
MOD_VFS_ENTRY(fd_open,           9)
MOD_VFS_ENTRY(fd_pipe_create,   10)
MOD_VFS_ENTRY(fd_poll,          11)
MOD_VFS_ENTRY(fd_pool_init,     12)
MOD_VFS_ENTRY(fd_read,          13)
MOD_VFS_ENTRY(fd_release,       14)
MOD_VFS_ENTRY(fd_stdio_desc,    15)
MOD_VFS_ENTRY(fd_stdio_init,    16)
MOD_VFS_ENTRY(fd_write,         17)
MOD_VFS_ENTRY(fstab_automount,  18)
MOD_VFS_ENTRY(init,             19)
MOD_VFS_ENTRY(klogf,            20)
MOD_VFS_ENTRY(kvlogf,           21)
MOD_VFS_ENTRY(lookup,           22)
MOD_VFS_ENTRY(lookup_flags,     23)
MOD_VFS_ENTRY(lookup_parent,    24)
MOD_VFS_ENTRY(mount,            25)
MOD_VFS_ENTRY(mount_by_fstype,  26)
MOD_VFS_ENTRY(mount_find,       27)
MOD_VFS_ENTRY(mount_romfs,      28)
MOD_VFS_ENTRY(mount_ufs,        29)
MOD_VFS_ENTRY(notify,           30)
MOD_VFS_ENTRY(path_chmod,       31)
MOD_VFS_ENTRY(path_link,        32)
MOD_VFS_ENTRY(path_mkdir,       33)
MOD_VFS_ENTRY(path_normalize,   34)
MOD_VFS_ENTRY(path_rename,      35)
MOD_VFS_ENTRY(path_statfs,      36)
MOD_VFS_ENTRY(path_unlink,      37)
MOD_VFS_ENTRY(path_utimes,      38)
MOD_VFS_ENTRY(tty_rx_notify,    39)
MOD_VFS_ENTRY(umount,           40)
MOD_VFS_ENTRY(vnode_acquire,    41)
MOD_VFS_ENTRY(vnode_alloc,      42)
MOD_VFS_ENTRY(vnode_read,       43)
MOD_VFS_ENTRY(vnode_readlink,   44)
MOD_VFS_ENTRY(vnode_release,    45)
MOD_VFS_ENTRY(vnode_stat,       46)

#ifndef MOD_VFS_FUNC_COUNT
#define MOD_VFS_FUNC_COUNT 47
#endif
