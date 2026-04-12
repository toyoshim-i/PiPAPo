/*
 * ufs_format.h — UFS on-disk data structures (4.4BSD FFS1 layout)
 *
 * Compatible with Linux mount -t ufs -o ufstype=44bsd.
 * All on-disk fields are always little-endian (regardless of CPU
 * endianness) for cross-architecture portability — UFS images on
 * FAT32 SD cards can be shared between ARM and m68k targets.
 * Block size is 4 KB; fragment size is 512 B (8 frags/block).
 *
 * Disk layout (single cylinder group, one UFS partition):
 *   Bytes 0–8191:      Boot block area (zeros)
 *   Bytes 8192–9727:   Superblock (1536 bytes, BSD FFS1 format)
 *   Bytes 16384–20479: Cylinder group 0 descriptor + bitmaps
 *   Fragment 40+:      Inode table (128 bytes/inode, 4 inodes/sector)
 *   Fragment 48+:      Data blocks (4 KB / 8 fragments each)
 */

#ifndef PPAP_KERNEL_VFS_UFS_FORMAT_H
#define PPAP_KERNEL_VFS_UFS_FORMAT_H

#include <stdint.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

#define UFS_MAGIC 0x00011954u /* BSD FFS1 superblock magic         */
#define UFS_BLOCK_SIZE 4096u  /* bytes per block                   */
#define UFS_FRAG_SIZE 512u    /* bytes per fragment (= sector)     */
#define UFS_FRAGS_PER_BLK 8u  /* fragments per block               */

#define UFS_INODE_SIZE 128u      /* bytes per on-disk inode           */
#define UFS_INODES_PER_SECTOR 4u /* 512 / 128                         */
#define UFS_DIRECT_BLOCKS 12     /* direct block pointers per inode   */
#define UFS_NAME_MAX 255         /* max filename length (excl. NUL)   */
#define UFS_ROOT_INO 2u          /* inode number of root directory    */

/* Fast symlink: target stored inline in i_direct[] (12 × 4 = 48 bytes) */
#define UFS_FAST_SYMLINK_MAX (UFS_DIRECT_BLOCKS * sizeof(uint32_t)) /* 48 */

/* Absolute sector numbers for fixed-location structures */
#define UFS_SB_SECTOR 16u /* superblock at byte 8192 = sector 16      */
#define UFS_CG_SECTOR 32u /* cylinder group at byte 16384 = sector 32 */

/*
 * Superblock byte offsets.  The full SB is 1536 bytes at absolute byte 8192.
 * Fields at offset O are in the sector at (UFS_SB_SECTOR + O/512),
 * at byte (O % 512) within that sector.
 */
#define UFS_FS_IBLKNO_OFF 16u          /* inode-table start fragment       */
#define UFS_FS_DBLKNO_OFF 20u          /* data-area start fragment         */
#define UFS_FS_DSIZE_OFF 40u           /* data fragment count              */
#define UFS_FS_BSIZE_OFF 48u           /* block size (must = UFS_BLOCK_SIZE) */
#define UFS_FS_IPG_OFF 184u            /* inodes per CG                    */
#define UFS_FS_CSTOTAL_NBFREE_OFF 196u /* total free blocks                */
#define UFS_FS_CSTOTAL_NIFREE_OFF 200u /* total free inodes                */
/* Magic at SB+1372 = abs sector UFS_SB_SECTOR+2, byte 1372%512 = 348 */
#define UFS_FS_MAGIC_OFF 1372u

/*
 * Cylinder-group byte offsets.  CG block is at absolute sector UFS_CG_SECTOR.
 */
#define UFS_CG_CS_NBFREE_OFF 28u /* free block count in this CG          */
#define UFS_CG_CS_NIFREE_OFF 32u /* free inode count in this CG          */
#define UFS_CG_IUSEDOFF_OFF 92u  /* uint32: byte offset of iused bitmap  */
#define UFS_CG_FREEOFF_OFF 96u   /* uint32: byte offset of free bitmap   */

/* ── On-disk inode (128 bytes, UFS1 layout) ─────────────────────────────── */

typedef struct {
  uint16_t i_mode;           /* file type + permissions               */
  uint16_t i_nlink;          /* hard link count                       */
  uint16_t i_uid16, i_gid16; /* uid/gid (low 16 bits)                 */
  uint64_t i_size;           /* file size in bytes                    */
  uint32_t i_atime, i_atimensec;
  uint32_t i_mtime, i_mtimensec;
  uint32_t i_ctime, i_ctimensec;
  uint32_t i_direct[UFS_DIRECT_BLOCKS]; /* direct block ptrs (frags)     */
  uint32_t i_ib[3]; /* indirect block ptrs (frags)           */
  uint32_t i_flags;
  uint32_t i_blocks; /* 512-byte units used                   */
  uint32_t i_gen;
  uint32_t i_uid, i_gid;
  uint64_t i_spare; /* reserved; zero                        */
} ufs_inode_t;

_Static_assert(sizeof(ufs_inode_t) == UFS_INODE_SIZE,
               "ufs_inode_t must be 128 bytes");

/* ── Variable-length directory entry ────────────────────────────────────── */

/*
 * Entries never cross a UFS_DIRBLK_SIZE (512-byte) chunk boundary.
 * The name immediately follows at offset 8; NUL-terminated and padded
 * to a 4-byte boundary by d_reclen.
 */
typedef struct {
  uint32_t d_ino;    /* inode number (0 = deleted/unused slot)  */
  uint16_t d_reclen; /* record length (multiple of 4)           */
  uint8_t d_type;    /* file type (UFS_DT_*)                    */
  uint8_t d_namlen;  /* name length (excl. NUL)                 */
  /* char d_name[] follows — NUL-terminated, padded to 4 bytes  */
} ufs_dirent_t;

/* Minimum record length for a name of `nl` bytes (includes NUL + 4-align) */
#define UFS_DIRENT_MINREC(nl) \
  ((uint16_t)(((uint16_t)8u + (uint16_t)(nl) + 1u + 3u) & ~(uint16_t)3u))

/* Directory chunk unit — entries must not cross this boundary */
#define UFS_DIRBLK_SIZE UFS_FRAG_SIZE /* 512 bytes */

/* d_type values */
#define UFS_DT_UNKNOWN 0u
#define UFS_DT_DIR 4u
#define UFS_DT_REG 8u
#define UFS_DT_LNK 10u

#endif /* PPAP_KERNEL_VFS_UFS_FORMAT_H */
