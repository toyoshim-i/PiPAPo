/*
 * tmpfs.h — RAM-backed temporary filesystem
 *
 * tmpfs provides a volatile, read-write filesystem mounted at /tmp.
 * All contents are stored in SRAM and lost on reboot.  Total data is
 * bounded by TMPFS_DATA_MAX (config.h) to prevent unbounded SRAM use.
 *
 * Call vfs_mount("/tmp", &tmpfs_ops, 0, NULL) from kmain() after vfs_init().
 */

#ifndef PPAP_FS_TMPFS_H
#define PPAP_FS_TMPFS_H

#include "../vfs/vfs.h"

extern const vfs_ops_t tmpfs_ops;

#endif /* PPAP_FS_TMPFS_H */
