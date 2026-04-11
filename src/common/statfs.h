/*
 * statfs.h --- Filesystem statistics structure
 *
 * Shared between kernel and user space.
 * Layout matches struct kernel_statfs in kernel/common/vfs/vfs_types.h.
 */

#ifndef PPAP_COMMON_STATFS_H
#define PPAP_COMMON_STATFS_H

#include <stdint.h>

struct statfs {
  uint32_t f_type;     /* filesystem magic number                       */
  uint32_t f_bsize;    /* optimal transfer block size                   */
  uint64_t f_blocks;   /* total data blocks in filesystem               */
  uint64_t f_bfree;    /* free blocks in filesystem                     */
  uint64_t f_bavail;   /* free blocks available to non-root             */
  uint64_t f_files;    /* total file nodes in filesystem                */
  uint64_t f_ffree;    /* free file nodes in filesystem                 */
  uint32_t f_fsid[2];  /* filesystem ID                                 */
  uint32_t f_namelen;  /* maximum filename length                       */
  uint32_t f_frsize;   /* fragment size (same as f_bsize for us)        */
  uint32_t f_flags;    /* mount flags (ST_RDONLY, etc.)                 */
  uint32_t f_spare[4]; /* padding to match Linux layout                 */
};

#endif /* PPAP_COMMON_STATFS_H */
