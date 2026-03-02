/*
 * vfat.h — FAT32 (VFAT) filesystem driver
 *
 * Provides vfs_ops_t implementation for mounting and operating on FAT32
 * partitions.  Supports both 8.3 short filenames and VFAT long filenames.
 *
 * Mount: pass a blkdev_t pointer as dev_data to vfs_mount().
 */

#ifndef PPAP_FS_VFAT_H
#define PPAP_FS_VFAT_H

#include "../vfs/vfs.h"

extern const vfs_ops_t vfat_ops;

#endif /* PPAP_FS_VFAT_H */
