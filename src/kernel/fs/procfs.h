/*
 * procfs.h — Process information pseudo-filesystem interface
 *
 * Provides procfs_ops — a vfs_ops_t for mounting a RAM-resident process
 * information filesystem at /proc.  All entries are generated dynamically
 * on each read.
 */

#ifndef PPAP_FS_PROCFS_H
#define PPAP_FS_PROCFS_H

#include "../vfs/vfs.h"

/* FS operations table — pass to vfs_mount() as the ops parameter */
extern const vfs_ops_t procfs_ops;

#endif /* PPAP_FS_PROCFS_H */
