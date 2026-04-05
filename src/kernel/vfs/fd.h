/*
 * fd.h — File descriptor pool declarations
 *
 * The system-wide file descriptor pool is managed by fd.c (VFS module).
 * Core-side code accesses it through mod_vfs exports (fd_open, fd_read,
 * fd_release, etc.) using opaque descriptor IDs.
 *
 * This header provides VFS-internal declarations for pipe.c and tty.c.
 */

#ifndef PPAP_KERNEL_FD_FD_H
#define PPAP_KERNEL_FD_FD_H

#include <stdint.h>

#include "kernel/vfs/file.h"

/*
 * fd_pool_alloc_pipe — Allocate a pool entry for a pipe end.
 *
 * Used by pipe.c to create pipe read/write file objects.
 * Returns descriptor ID, or negative errno if pool is exhausted.
 */
int fd_pool_alloc_pipe(const struct file_ops *ops, void *priv,
                       uint32_t flags);

#endif /* PPAP_KERNEL_FD_FD_H */
