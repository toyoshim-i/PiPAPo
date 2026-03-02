/*
 * ufs_format.h — UFS on-disk data structures
 *
 * Simplified 4.4BSD-style Unix File System format for RP2040.
 * All fields are little-endian (ARM native).  Block size is 4 KB
 * (matching PAGE_SIZE and SD card erase granularity).
 *
 * Disk layout:
 *   Block 0:        Superblock
 *   Block 1:        Block bitmap (covers up to 32768 blocks = 128 MB)
 *   Block 2:        Inode bitmap (covers up to 32768 inodes)
 *   Blocks 3..N:    Inode table (N-2 blocks × 64 inodes/block)
 *   Blocks N+1..:   Data blocks
 *
 * Maximum file sizes:
 *   Direct only (10 blocks):            40 KB
 *   Direct + single indirect:           ~4 MB (10 + 1024 blocks)
 */

#ifndef PPAP_FS_UFS_FORMAT_H
#define PPAP_FS_UFS_FORMAT_H

#include <stdint.h>

/* ── Constants ────────────────────────────────────────────────────────────── */

#define UFS_MAGIC           0x55465331u  /* "UFS1" in little-endian       */
#define UFS_BLOCK_SIZE      4096u        /* bytes per block               */
#define UFS_INODE_SIZE        64u        /* bytes per on-disk inode       */
#define UFS_INODES_PER_BLOCK (UFS_BLOCK_SIZE / UFS_INODE_SIZE)  /* 64    */
#define UFS_DIRECT_BLOCKS     10         /* direct block pointers         */
#define UFS_DIRENT_SIZE       32u        /* bytes per directory entry     */
#define UFS_DIRENTS_PER_BLOCK (UFS_BLOCK_SIZE / UFS_DIRENT_SIZE) /* 128  */
#define UFS_NAME_MAX          27         /* max filename (27 + NUL = 28)  */
#define UFS_ROOT_INO           1         /* inode 0 is reserved/unused    */

/* Number of block pointers in a single indirect block */
#define UFS_PTRS_PER_BLOCK  (UFS_BLOCK_SIZE / sizeof(uint32_t))  /* 1024 */

/* ── Superblock (128 bytes at offset 0 within block 0) ───────────────── */

typedef struct {
    uint32_t s_magic;          /* UFS_MAGIC                              */
    uint32_t s_block_size;     /* always UFS_BLOCK_SIZE (4096)           */
    uint32_t s_block_count;    /* total blocks in filesystem             */
    uint32_t s_inode_count;    /* total inodes provisioned               */
    uint32_t s_free_blocks;    /* current free block count               */
    uint32_t s_free_inodes;    /* current free inode count               */
    uint32_t s_bmap_block;     /* block number of block bitmap (1)       */
    uint32_t s_imap_block;     /* block number of inode bitmap (2)       */
    uint32_t s_itable_block;   /* first block of inode table (3)         */
    uint32_t s_data_block;     /* first data block                       */
    uint32_t s_inode_blocks;   /* number of blocks in inode table        */
    uint8_t  s_pad[84];        /* pad to 128 bytes total                 */
} ufs_super_t;

/* ── Inode (64 bytes) ─────────────────────────────────────────────────── */

typedef struct {
    uint16_t i_mode;           /* file type + permissions (S_IFREG|0644) */
    uint16_t i_nlink;          /* hard link count                        */
    uint16_t i_uid;            /* owner uid (always 0 for now)           */
    uint16_t i_gid;            /* group gid (always 0 for now)           */
    uint32_t i_size;           /* file size in bytes                     */
    uint32_t i_mtime;          /* last modification time (UNIX epoch)    */
    uint32_t i_ctime;          /* status change time                     */
    uint32_t i_direct[UFS_DIRECT_BLOCKS]; /* direct block pointers (10)  */
    uint32_t i_indirect;       /* single-indirect block pointer          */
} ufs_inode_t;

/* Verify sizes at compile time */
_Static_assert(sizeof(ufs_inode_t) == UFS_INODE_SIZE,
               "ufs_inode_t must be 64 bytes");
_Static_assert(sizeof(ufs_super_t) == 128,
               "ufs_super_t must be 128 bytes");

/* ── Directory entry (32 bytes) ───────────────────────────────────────── */

typedef struct {
    uint32_t d_ino;                      /* inode number (0 = unused)     */
    char     d_name[UFS_NAME_MAX + 1];   /* filename (NUL-terminated, 28B)*/
} ufs_dirent_t;

_Static_assert(sizeof(ufs_dirent_t) == UFS_DIRENT_SIZE,
               "ufs_dirent_t must be 32 bytes");

/* ── Symbolic link inline storage ─────────────────────────────────────── */

/* Symlinks with targets ≤ UFS_FAST_SYMLINK_MAX bytes are stored inline
 * in the i_direct[] array (fast symlinks).  Longer targets use a data
 * block. */
#define UFS_FAST_SYMLINK_MAX  (UFS_DIRECT_BLOCKS * sizeof(uint32_t))  /* 40 */

#endif /* PPAP_FS_UFS_FORMAT_H */
