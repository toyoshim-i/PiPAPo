/*
 * romfs.h — Read-only romfs driver interface
 *
 * Provides romfs_ops — a vfs_ops_t for mounting a flash-resident romfs
 * image as a read-only filesystem.  The image base address is passed as
 * dev_data to vfs_mount().
 */

#ifndef PPAP_FS_ROMFS_H
#define PPAP_FS_ROMFS_H

#include "../vfs/vfs.h"

/* FS operations table — pass to vfs_mount() as the ops parameter */
extern const vfs_ops_t romfs_ops;

#endif /* PPAP_FS_ROMFS_H */
