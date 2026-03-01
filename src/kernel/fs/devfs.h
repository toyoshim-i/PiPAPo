/*
 * devfs.h — Device pseudo-filesystem interface
 *
 * Provides devfs_ops — a vfs_ops_t for mounting a RAM-resident device
 * filesystem at /dev.  All entries are created at mount time from a
 * static table; there is no on-disk backing.
 */

#ifndef PPAP_FS_DEVFS_H
#define PPAP_FS_DEVFS_H

#include "../vfs/vfs.h"

/* FS operations table — pass to vfs_mount() as the ops parameter */
extern const vfs_ops_t devfs_ops;

#endif /* PPAP_FS_DEVFS_H */
