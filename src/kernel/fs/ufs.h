/*
 * ufs.h — UFS filesystem driver (public interface)
 *
 * Provides the vfs_ops_t table for mounting UFS filesystem images
 * through the block device layer.
 */

#ifndef PPAP_FS_UFS_H
#define PPAP_FS_UFS_H

#include "../vfs/vfs.h"

extern const vfs_ops_t ufs_ops;

#endif /* PPAP_FS_UFS_H */
