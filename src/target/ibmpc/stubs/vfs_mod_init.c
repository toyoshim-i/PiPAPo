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
void vfs_acquire_vnode(vnode_t *);
void vfs_release_vnode(vnode_t *);

struct pcb;
typedef struct pcb pcb_t;
void vfs_fd_stdio_init(pcb_t *);
long vfs_file_read(vnode_t *, void *, uint32_t, uint32_t);
void vfs_file_pool_init(void);
void vfs_tty_rx_notify(int);
void vfs_fstab_automount(void);

/* Step 3: fd pool operations (fd.c / pipe.c) */
int  vfs_fd_open(const char *, int, int);
void vfs_fd_release(int);
void vfs_fd_acquire(int);
int  vfs_fd_pipe_create(int *, int *);
int  vfs_fd_stdio_desc(int);
long vfs_fd_read(int, char *, size_t);
long vfs_fd_write(int, const char *, size_t);
int  vfs_fd_ioctl(int, uint32_t, void *);
int  vfs_fd_poll(int);
long vfs_fd_lseek(int, long, int);
int  vfs_fd_fstat(int, void *);
long vfs_fd_getdents(int, void *, size_t);
long vfs_fd_getdents64(int, void *, long);
int  vfs_fd_fstatfs(int, void *);
long vfs_fd_fcntl(int, int, long);
void *vfs_fd_get_priv(int);

/* Now include mod_vfs.h which declares mod_vfs_t */
#include "common/mod/mod_vfs.h"

MOD_DEFINE_BEGIN(vfs)
#define MOD_VFS_ENTRY(name, idx)  MOD_IMPL(vfs, name)
#include "common/mod/mod_vfs.inc"
#undef MOD_VFS_ENTRY
MOD_DEFINE_END()

#endif /* __ia16__ */
