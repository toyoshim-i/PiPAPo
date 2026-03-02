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

/* Step 7: allocation self-test (exercised by main_qemu.c) */
void ufs_alloc_selftest(int *out_pass, int *out_fail);

#endif /* PPAP_FS_UFS_H */
