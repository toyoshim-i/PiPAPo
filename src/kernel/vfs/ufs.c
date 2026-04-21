/*
 * ufs.c — UFS filesystem driver
 *
 * Implements vfs_ops_t for mounting UFS filesystem images via the block
 * device layer.  The driver reads and writes the on-disk superblock,
 * inode table, bitmaps, and data blocks through sector-level blkdev I/O.
 *
 * Step 6: read-only mount/lookup/read/readdir/stat/readlink
 * Step 7: block/inode allocation, inode write-back, superblock sync
 * Step 8: write, create, truncate
 * Step 9: mkdir, unlink (directory ops + link count management)
 *
 * Operations:
 *   mount    — verify magic, parse superblock, allocate root vnode
 *   lookup   — walk directory entries to find child by name
 *   read     — follow direct + single-indirect block pointers
 *   readdir  — iterate directory entries (skips "." and "..")
 *   stat     — return inode metadata
 *   readlink — fast symlink (inline) or data block symlink
 */

#include "kernel/vfs/ufs.h"

#include <stddef.h>

#include "common/errno.h"
#include "kernel/common/mem_region_kbuf.h"
#include "kernel/common/mod/mod_core.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/common/spinlock.h" /* SPIN_FS */
#include "kernel/vfs/driver/blkdev.h"
#include "kernel/vfs/klog.h"
#include "kernel/vfs/ufs_format.h"
#include "kernel/vfs/vfs.h"

/* ── Constants ────────────────────────────────────────────────────────── */

#define SECTORS_PER_BLOCK (UFS_BLOCK_SIZE / BLKDEV_SECTOR_SIZE) /* 8 */

/* ── In-memory filesystem state ─────────────────────────────────────── */

typedef struct {
  blkdev_t *dev;
  uint32_t itable_sector; /* abs sector of inode table start (fs_iblkno) */
  uint32_t data_frag;     /* abs fragment of data area start (fs_dblkno) */
  uint32_t dsize_frags;   /* total data fragments                        */
  uint32_t inode_count;   /* total provisioned inodes                    */
  uint32_t free_blocks;   /* free full-block count (8 frags each)        */
  uint32_t free_inodes;   /* free inode count                            */
  uint32_t cg_freeoff;    /* byte offset of block free bitmap in CG      */
  uint32_t cg_iusedoff;   /* byte offset of inode used bitmap in CG      */
  page_id_t scratch_page; /* out-of-segment page for large temporaries   */
} ufs_priv_t;

static ufs_priv_t ufs_priv;

/* ── Sector I/O buffer ────────────────────────────────────────────────── */
/* Protected by SPIN_FS for dual-core safety — acquired at VFS entry points. */

/* Aligned so it never straddles a page boundary — the blkdev
 * single-page contract requires that off + 512 <= PAGE_SIZE for the
 * (page, off) pair derived via mem_region_kbuf_to_page. */
static uint8_t ufs_buf[BLKDEV_SECTOR_SIZE]
    __attribute__((aligned(BLKDEV_SECTOR_SIZE)));

static uint32_t ufs_get_u32(const uint8_t *buf, uint32_t off) {
  uint32_t v;
  __builtin_memcpy(&v, &buf[off], sizeof(v));
  return v;
}

static void ufs_put_u32(uint8_t *buf, uint32_t off, uint32_t v) {
  __builtin_memcpy(&buf[off], &v, sizeof(v));
}

/* ── Block I/O helpers ─────────────────────────────────────────────── */

/* Read one sector from the underlying block device into ufs_buf */
static int ufs_read_sector(ufs_priv_t *priv, uint32_t abs_sector) {
  page_id_t page;
  uint16_t off;
  mem_region_kbuf_to_page(ufs_buf, &page, &off);
  return priv->dev->read(priv->dev, page, off, abs_sector, 1);
}

/* ── Block write helpers ──────────────────────────────────────────────── */

/* Write ufs_buf to one sector on the underlying block device */
static int ufs_write_sector(ufs_priv_t *priv, uint32_t abs_sector) {
  page_id_t page;
  uint16_t off;
  mem_region_kbuf_to_page(ufs_buf, &page, &off);
  return priv->dev->write(priv->dev, page, off, abs_sector, 1);
}

static int ufs_free_block(ufs_priv_t *priv, uint32_t frag);

/* ── Indirect block pointer freeing (via scratch page) ───────────────── */

/* Free all non-zero block pointers in ufs_buf.  Copies ufs_buf to the
 * out-of-segment scratch page first because ufs_free_block clobbers
 * ufs_buf.  Saves 512 bytes of kernel stack per call site compared to
 * the former `uint32_t saved[128]` stack array. */
static void ufs_free_indirect_ptrs(ufs_priv_t *priv, uint32_t start,
                                   uint32_t count) {
  mod_core.mem_region_page_write(priv->scratch_page, 0, ufs_buf,
                                 BLKDEV_SECTOR_SIZE);
  for (uint32_t j = start; j < count; j++) {
    uint32_t ptr;
    mod_core.mem_region_page_read(priv->scratch_page,
                                  (uint16_t)(j * sizeof(uint32_t)), &ptr,
                                  sizeof(ptr));
    if (ptr != 0) ufs_free_block(priv, ptr);
  }
}

/* Free the double-indirect tree rooted at ib2_frag, keeping only the first
 * `keep` logical data blocks reachable through it.  If keep == 0 the outer
 * block itself is also returned to the allocator (caller must zero
 * inode->i_ib[1]).  The outer sector is cached at scratch_page[2048..] so it
 * doesn't collide with ufs_free_indirect_ptrs which uses the [0..511] region.
 */
#define UFS_SCRATCH_OUTER_OFF 2048u
static void ufs_free_double_indirect(ufs_priv_t *priv, uint32_t ib2_frag,
                                     uint32_t keep) {
  uint32_t ptrs_per_block = UFS_BLOCK_SIZE / sizeof(uint32_t);   /* 1024 */
  uint32_t ptrs_per_sec = BLKDEV_SECTOR_SIZE / sizeof(uint32_t); /* 128 */
  for (uint32_t s = 0; s < UFS_FRAGS_PER_BLK; s++) {
    int rc = ufs_read_sector(priv, ib2_frag + s);
    if (rc < 0) return;
    mod_core.mem_region_page_write(priv->scratch_page, UFS_SCRATCH_OUTER_OFF,
                                   ufs_buf, BLKDEV_SECTOR_SIZE);
    int sector_dirty = 0;
    for (uint32_t i = 0; i < ptrs_per_sec; i++) {
      uint32_t outer_idx = s * ptrs_per_sec + i;
      uint32_t block_start = outer_idx * ptrs_per_block;
      uint32_t inner_frag;
      mod_core.mem_region_page_read(
          priv->scratch_page,
          (uint16_t)(UFS_SCRATCH_OUTER_OFF + i * sizeof(uint32_t)), &inner_frag,
          sizeof(inner_frag));
      if (inner_frag == 0) continue;

      if (block_start >= keep) {
        /* Free the inner indirect block and everything under it. */
        for (uint32_t is = 0; is < UFS_FRAGS_PER_BLK; is++) {
          rc = ufs_read_sector(priv, inner_frag + is);
          if (rc < 0) break;
          ufs_free_indirect_ptrs(priv, 0, ptrs_per_sec);
        }
        ufs_free_block(priv, inner_frag);
        /* Clear the outer slot so a later extend doesn't reuse a stale
         * pointer to a recycled block. */
        uint32_t zero = 0;
        mod_core.mem_region_page_write(
            priv->scratch_page,
            (uint16_t)(UFS_SCRATCH_OUTER_OFF + i * sizeof(uint32_t)), &zero,
            sizeof(zero));
        sector_dirty = 1;
      } else if (block_start + ptrs_per_block > keep) {
        /* Partial free inside this inner indirect block. */
        uint32_t keep_in_inner = keep - block_start;
        uint32_t first_sec = keep_in_inner / ptrs_per_sec;
        for (uint32_t is = first_sec; is < UFS_FRAGS_PER_BLK; is++) {
          rc = ufs_read_sector(priv, inner_frag + is);
          if (rc < 0) break;
          uint32_t start_in_sec =
              (is == first_sec) ? keep_in_inner % ptrs_per_sec : 0;
          ufs_free_indirect_ptrs(priv, start_in_sec, ptrs_per_sec);
        }
        /* Inner block still in use; leave outer pointer intact. */
      }
      /* else: block_start + ptrs_per_block <= keep → keep fully. */
    }
    if (sector_dirty) {
      mod_core.mem_region_page_read(priv->scratch_page, UFS_SCRATCH_OUTER_OFF,
                                    ufs_buf, BLKDEV_SECTOR_SIZE);
      rc = ufs_write_sector(priv, ib2_frag + s);
      if (rc < 0) return;
    }
  }
  if (keep == 0) {
    ufs_free_block(priv, ib2_frag);
  }
}

/* ── Inode I/O ────────────────────────────────────────────────────────── */

/* Read an on-disk inode.  UFS1 inode = 128 bytes; 4 per 512-byte sector.
 * Sector = itable_sector + ino/4; offset = (ino%4)*128.  Clobbers ufs_buf. */
static int ufs_read_inode(ufs_priv_t *priv, uint32_t ino, ufs_inode_t *out) {
  uint32_t sec = priv->itable_sector + ino / UFS_INODES_PER_SECTOR;
  uint32_t off = (ino % UFS_INODES_PER_SECTOR) * UFS_INODE_SIZE;
  int rc = ufs_read_sector(priv, sec);
  if (rc < 0) return rc;
  __builtin_memcpy(out, &ufs_buf[off], UFS_INODE_SIZE);
  return 0;
}

/* Write an on-disk inode.  RMW the sector.  Clobbers ufs_buf. */
static int ufs_write_inode(ufs_priv_t *priv, uint32_t ino,
                           const ufs_inode_t *inode) {
  uint32_t sec = priv->itable_sector + ino / UFS_INODES_PER_SECTOR;
  uint32_t off = (ino % UFS_INODES_PER_SECTOR) * UFS_INODE_SIZE;
  int rc = ufs_read_sector(priv, sec);
  if (rc < 0) return rc;
  __builtin_memcpy(&ufs_buf[off], inode, UFS_INODE_SIZE);
  return ufs_write_sector(priv, sec);
}

/* ── CG bitmap helpers ───────────────────────────────────────────────── */

/* Load the CG sector that contains byte_off into ufs_buf;
 * return abs sector and byte offset within it.  Clobbers ufs_buf. */
static int ufs_cg_byte_read(ufs_priv_t *priv, uint32_t byte_off,
                            uint32_t *cg_sec_out, uint32_t *sec_off_out) {
  *cg_sec_out = UFS_CG_SECTOR + byte_off / BLKDEV_SECTOR_SIZE;
  *sec_off_out = byte_off % BLKDEV_SECTOR_SIZE;
  return ufs_read_sector(priv, *cg_sec_out);
}

/* ── Block allocation (CG free bitmap, 1 byte = 8 frags = 1 block) ─────── */

/* Allocate a free data block.  Returns 0 on success; *out_frag = starting
 * fragment of the allocated block.  Each byte in the free bitmap represents
 * 8 consecutive fragments = 1 full block (0xFF = all frags free).         */
static int ufs_alloc_block(ufs_priv_t *priv, uint32_t *out_frag) {
  uint32_t nblks = priv->dsize_frags / UFS_FRAGS_PER_BLK;
  for (uint32_t n = 0; n < nblks; n++) {
    uint32_t byte_off = priv->cg_freeoff + n;
    uint32_t cg_sec, sec_off;
    int rc = ufs_cg_byte_read(priv, byte_off, &cg_sec, &sec_off);
    if (rc < 0) return rc;
    if (ufs_buf[sec_off] != 0xFF) continue; /* not all 8 frags free */
    ufs_buf[sec_off] = 0x00;                /* mark all 8 frags allocated */
    rc = ufs_write_sector(priv, cg_sec);
    if (rc < 0) return rc;
    priv->free_blocks--;
    *out_frag = priv->data_frag + n * UFS_FRAGS_PER_BLK;
    return 0;
  }
  return -ENOSPC;
}

/* Free a data block.  frag = starting fragment of the block. */
static int ufs_free_block(ufs_priv_t *priv, uint32_t frag) {
  if (frag < priv->data_frag) return -EINVAL;
  uint32_t n = (frag - priv->data_frag) / UFS_FRAGS_PER_BLK;
  uint32_t byte_off = priv->cg_freeoff + n;
  uint32_t cg_sec, sec_off;
  int rc = ufs_cg_byte_read(priv, byte_off, &cg_sec, &sec_off);
  if (rc < 0) return rc;
  ufs_buf[sec_off] = 0xFF;
  rc = ufs_write_sector(priv, cg_sec);
  if (rc < 0) return rc;
  priv->free_blocks++;
  return 0;
}

/* ── Inode allocation (CG iused bitmap, 1 bit per inode) ──────────────── */

/* Allocate a free inode.  Returns 0 on success; *out = inode number. */
static int ufs_alloc_inode(ufs_priv_t *priv, uint32_t *out) {
  /* Start from ROOT_INO+1 to skip well-known inodes */
  for (uint32_t ino = UFS_ROOT_INO + 1u; ino < priv->inode_count; ino++) {
    uint32_t byte_off = priv->cg_iusedoff + ino / 8u;
    uint32_t cg_sec, sec_off;
    int rc = ufs_cg_byte_read(priv, byte_off, &cg_sec, &sec_off);
    if (rc < 0) return rc;
    uint8_t bit = (uint8_t)(1u << (ino % 8u));
    if (ufs_buf[sec_off] & bit) continue; /* in use */
    ufs_buf[sec_off] |= bit;
    rc = ufs_write_sector(priv, cg_sec);
    if (rc < 0) return rc;
    priv->free_inodes--;
    *out = ino;
    return 0;
  }
  return -ENOSPC;
}

/* Free an inode.  Clobbers ufs_buf. */
static int ufs_free_inode(ufs_priv_t *priv, uint32_t ino) {
  if (ino >= priv->inode_count) return -EINVAL;
  uint32_t byte_off = priv->cg_iusedoff + ino / 8u;
  uint32_t cg_sec, sec_off;
  int rc = ufs_cg_byte_read(priv, byte_off, &cg_sec, &sec_off);
  if (rc < 0) return rc;
  ufs_buf[sec_off] &= (uint8_t) ~(1u << (ino % 8u));
  rc = ufs_write_sector(priv, cg_sec);
  if (rc < 0) return rc;
  priv->free_inodes++;
  return 0;
}

/* ── Superblock sync ─────────────────────────────────────────────────── */

/* Write cached free counts back to the on-disk SB and CG.  Clobbers ufs_buf. */
static int ufs_sync_super(ufs_priv_t *priv) {
  /* SB free counts: sector UFS_SB_SECTOR, byte offsets 196 and 200 */
  int rc = ufs_read_sector(priv, UFS_SB_SECTOR);
  if (rc < 0) return rc;
  ufs_put_u32(ufs_buf, UFS_FS_CSTOTAL_NBFREE_OFF, priv->free_blocks);
  ufs_put_u32(ufs_buf, UFS_FS_CSTOTAL_NIFREE_OFF, priv->free_inodes);
  rc = ufs_write_sector(priv, UFS_SB_SECTOR);
  if (rc < 0) return rc;
  /* CG summary counts: sector UFS_CG_SECTOR */
  rc = ufs_read_sector(priv, UFS_CG_SECTOR);
  if (rc < 0) return rc;
  ufs_put_u32(ufs_buf, UFS_CG_CS_NBFREE_OFF, priv->free_blocks);
  ufs_put_u32(ufs_buf, UFS_CG_CS_NIFREE_OFF, priv->free_inodes);
  return ufs_write_sector(priv, UFS_CG_SECTOR);
}

/* ── Data block zeroing ──────────────────────────────────────────────── */

/* Zero all 8 sectors of a UFS block.  frag = starting fragment.  Clobbers
 * ufs_buf. */
static int ufs_zero_block(ufs_priv_t *priv, uint32_t frag) {
  __builtin_memset(ufs_buf, 0, BLKDEV_SECTOR_SIZE);
  for (uint32_t s = 0; s < UFS_FRAGS_PER_BLK; s++) {
    int rc = ufs_write_sector(priv, frag + s);
    if (rc < 0) return rc;
  }
  return 0;
}

/* ── Allocation self-test (called from main_qemu.c) ──────────────────── */

static void alloc_check(const char *name, int ok, int *pass, int *fail) {
  if (ok) {
    klogf("TEST: %s ... PASS\n", name);
    (*pass)++;
  } else {
    klogf("TEST: %s ... FAIL\n", name);
    (*fail)++;
  }
}

void ufs_alloc_selftest(int *out_pass, int *out_fail) {
  int pass = 0, fail = 0;
  uint32_t orig_fb = ufs_priv.free_blocks;
  uint32_t orig_fi = ufs_priv.free_inodes;

  /* 1. Alloc block → free_blocks decremented */
  uint32_t frag = 0;
  int rc = ufs_alloc_block(&ufs_priv, &frag);
  alloc_check("alloc_block",
              rc == 0 && frag >= ufs_priv.data_frag &&
                  ufs_priv.free_blocks == orig_fb - 1,
              &pass, &fail);

  /* 2. Free block → free_blocks restored */
  rc = ufs_free_block(&ufs_priv, frag);
  alloc_check("free_block", rc == 0 && ufs_priv.free_blocks == orig_fb, &pass,
              &fail);

  /* 3. Alloc inode → free_inodes decremented */
  uint32_t ino = 0;
  rc = ufs_alloc_inode(&ufs_priv, &ino);
  alloc_check("alloc_inode",
              rc == 0 && ino >= 2 && ufs_priv.free_inodes == orig_fi - 1, &pass,
              &fail);

  /* 4. Free inode → free_inodes restored */
  rc = ufs_free_inode(&ufs_priv, ino);
  alloc_check("free_inode", rc == 0 && ufs_priv.free_inodes == orig_fi, &pass,
              &fail);

  /* 5. Write inode + read back → data matches */
  rc = ufs_alloc_inode(&ufs_priv, &ino);
  if (rc == 0) {
    ufs_inode_t test_in;
    __builtin_memset(&test_in, 0, sizeof(test_in));
    test_in.i_mode = 0100644;
    test_in.i_nlink = 1;
    test_in.i_size = 12345;
    test_in.i_direct[0] = 42;

    rc = ufs_write_inode(&ufs_priv, ino, &test_in);
    ufs_inode_t test_out;
    int rc2 = ufs_read_inode(&ufs_priv, ino, &test_out);
    alloc_check("write+read inode",
                rc == 0 && rc2 == 0 && test_out.i_mode == 0100644 &&
                    test_out.i_nlink == 1 && test_out.i_size == 12345 &&
                    test_out.i_direct[0] == 42,
                &pass, &fail);

    /* Clean up test inode */
    __builtin_memset(&test_in, 0, sizeof(test_in));
    ufs_write_inode(&ufs_priv, ino, &test_in);
    ufs_free_inode(&ufs_priv, ino);
  } else {
    alloc_check("write+read inode (alloc failed)", 0, &pass, &fail);
  }

  /* 6. Sync super → re-read → free counts match */
  rc = ufs_sync_super(&ufs_priv);
  int rc2 = ufs_read_sector(&ufs_priv, UFS_SB_SECTOR);
  alloc_check("sync_super",
              rc == 0 && rc2 == 0 &&
                  ufs_get_u32(ufs_buf, UFS_FS_CSTOTAL_NBFREE_OFF) ==
                      ufs_priv.free_blocks &&
                  ufs_get_u32(ufs_buf, UFS_FS_CSTOTAL_NIFREE_OFF) ==
                      ufs_priv.free_inodes,
              &pass, &fail);

  *out_pass = pass;
  *out_fail = fail;
}

/* ── Block mapping ────────────────────────────────────────────────────── */

/* Resolve a logical file block index to a physical fragment number.
 * Handles direct (0..11), single-indirect via i_ib[0], and double-indirect
 * via i_ib[1].  Clobbers ufs_buf when reading an indirect block. */
static int ufs_block_map(ufs_priv_t *priv, const ufs_inode_t *inode,
                         uint32_t logical, uint32_t *phys_out) {
  if (logical < (uint32_t)UFS_DIRECT_BLOCKS) {
    *phys_out = inode->i_direct[logical];
    return 0;
  }

  uint32_t ptrs_per_block = UFS_BLOCK_SIZE / sizeof(uint32_t);   /* 1024 */
  uint32_t ptrs_per_sec = BLKDEV_SECTOR_SIZE / sizeof(uint32_t); /* 128 */
  uint32_t ind_idx = logical - (uint32_t)UFS_DIRECT_BLOCKS;

  if (ind_idx < ptrs_per_block) {
    /* Single-indirect */
    if (inode->i_ib[0] == 0) {
      *phys_out = 0;
      return 0;
    }
    int rc = ufs_read_sector(priv, inode->i_ib[0] + ind_idx / ptrs_per_sec);
    if (rc < 0) return rc;
    *phys_out = ((uint32_t *)ufs_buf)[ind_idx % ptrs_per_sec];
    return 0;
  }

  /* Double-indirect */
  uint32_t di_idx = ind_idx - ptrs_per_block;
  if (di_idx >= ptrs_per_block * ptrs_per_block)
    return -EIO; /* beyond double-indirect range */

  if (inode->i_ib[1] == 0) {
    *phys_out = 0;
    return 0;
  }

  uint32_t outer_idx = di_idx / ptrs_per_block;
  uint32_t inner_idx = di_idx % ptrs_per_block;

  int rc = ufs_read_sector(priv, inode->i_ib[1] + outer_idx / ptrs_per_sec);
  if (rc < 0) return rc;
  uint32_t inner_frag = ((uint32_t *)ufs_buf)[outer_idx % ptrs_per_sec];
  if (inner_frag == 0) {
    *phys_out = 0;
    return 0;
  }

  rc = ufs_read_sector(priv, inner_frag + inner_idx / ptrs_per_sec);
  if (rc < 0) return rc;
  *phys_out = ((uint32_t *)ufs_buf)[inner_idx % ptrs_per_sec];
  return 0;
}

/* Set a logical block pointer in an inode (stores fragment number).
 * Direct (0..11) → i_direct[]; single-indirect via i_ib[0]; double-indirect
 * via i_ib[1]; allocated on demand.  Caller writes the inode back.
 * Clobbers ufs_buf. */
static int ufs_block_set(ufs_priv_t *priv, ufs_inode_t *inode, uint32_t logical,
                         uint32_t phys) {
  if (logical < (uint32_t)UFS_DIRECT_BLOCKS) {
    inode->i_direct[logical] = phys;
    return 0;
  }

  uint32_t ptrs_per_block = UFS_BLOCK_SIZE / sizeof(uint32_t);
  uint32_t ptrs_per_sec = BLKDEV_SECTOR_SIZE / sizeof(uint32_t);
  uint32_t ind_idx = logical - (uint32_t)UFS_DIRECT_BLOCKS;

  if (ind_idx < ptrs_per_block) {
    /* Single-indirect */
    if (inode->i_ib[0] == 0) {
      uint32_t ind_frag;
      int rc = ufs_alloc_block(priv, &ind_frag);
      if (rc < 0) return rc;
      rc = ufs_zero_block(priv, ind_frag);
      if (rc < 0) return rc;
      inode->i_ib[0] = ind_frag;
    }
    uint32_t sec = ind_idx / ptrs_per_sec;
    uint32_t off = ind_idx % ptrs_per_sec;
    int rc = ufs_read_sector(priv, inode->i_ib[0] + sec);
    if (rc < 0) return rc;
    ((uint32_t *)ufs_buf)[off] = phys;
    return ufs_write_sector(priv, inode->i_ib[0] + sec);
  }

  /* Double-indirect */
  uint32_t di_idx = ind_idx - ptrs_per_block;
  if (di_idx >= ptrs_per_block * ptrs_per_block) return -ENOSPC;

  if (inode->i_ib[1] == 0) {
    uint32_t ib2_frag;
    int rc = ufs_alloc_block(priv, &ib2_frag);
    if (rc < 0) return rc;
    rc = ufs_zero_block(priv, ib2_frag);
    if (rc < 0) return rc;
    inode->i_ib[1] = ib2_frag;
  }

  uint32_t outer_idx = di_idx / ptrs_per_block;
  uint32_t inner_idx = di_idx % ptrs_per_block;
  uint32_t outer_sec = outer_idx / ptrs_per_sec;
  uint32_t outer_off = outer_idx % ptrs_per_sec;

  int rc = ufs_read_sector(priv, inode->i_ib[1] + outer_sec);
  if (rc < 0) return rc;
  uint32_t inner_frag = ((uint32_t *)ufs_buf)[outer_off];
  if (inner_frag == 0) {
    rc = ufs_alloc_block(priv, &inner_frag);
    if (rc < 0) return rc;
    rc = ufs_zero_block(priv, inner_frag);
    if (rc < 0) return rc;
    /* ufs_alloc_block/ufs_zero_block clobber ufs_buf — re-read outer sector */
    rc = ufs_read_sector(priv, inode->i_ib[1] + outer_sec);
    if (rc < 0) return rc;
    ((uint32_t *)ufs_buf)[outer_off] = inner_frag;
    rc = ufs_write_sector(priv, inode->i_ib[1] + outer_sec);
    if (rc < 0) return rc;
  }

  uint32_t inner_sec = inner_idx / ptrs_per_sec;
  uint32_t inner_off = inner_idx % ptrs_per_sec;
  rc = ufs_read_sector(priv, inner_frag + inner_sec);
  if (rc < 0) return rc;
  ((uint32_t *)ufs_buf)[inner_off] = phys;
  return ufs_write_sector(priv, inner_frag + inner_sec);
}

/* ── vnode from inode ─────────────────────────────────────────────────── */

static vnode_t *ufs_vnode_from_inode(mount_entry_t *mnt, uint32_t ino,
                                     const ufs_inode_t *inode) {
  vnode_t *vn = mod_vfs.vnode_alloc();
  if (!vn) return (vnode_t *)0;

  vn->ino = ino;
  vn->size = (uint32_t)inode->i_size;
  vn->mount = mnt;
  vn->fs_priv = mnt->sb_priv;

  if (S_ISDIR(inode->i_mode)) {
    vn->type = VNODE_DIR;
    vn->mode = inode->i_mode;
  } else if (S_ISLNK(inode->i_mode)) {
    vn->type = VNODE_SYMLINK;
    vn->mode = inode->i_mode;
  } else {
    vn->type = VNODE_FILE;
    vn->mode = inode->i_mode;
  }

  return vn;
}

/* ── ufs_mount ────────────────────────────────────────────────────────── */

static int ufs_mount(mount_entry_t *mnt, const void *dev_data) {
  blkdev_t *dev = blkdev_find((const char *)dev_data);
  if (!dev) return -EINVAL;

  ufs_priv.dev = dev;

  /* Allocate a scratch page for large temporaries (indirect block
   * pointer arrays).  Lives outside the DS=0 segment on i16. */
  {
    proc_image_segment_t scratch_seg;
    if (mod_core.mem_region_alloc(&scratch_seg, PPAP_MEM_RAM_DATA, PAGE_SIZE,
                                  0) == 0)
      ufs_priv.scratch_page = scratch_seg.base_page;
    else
      ufs_priv.scratch_page = PAGE_ID_INVALID;
  }

  /* Read SB sector (sector 16) and extract layout parameters */
  int rc = ufs_read_sector(&ufs_priv, UFS_SB_SECTOR);
  if (rc < 0) return rc;

  uint32_t bsize = ufs_get_u32(ufs_buf, UFS_FS_BSIZE_OFF);
  if (bsize != UFS_BLOCK_SIZE) return -EINVAL;

  ufs_priv.itable_sector = ufs_get_u32(ufs_buf, UFS_FS_IBLKNO_OFF);
  ufs_priv.data_frag = ufs_get_u32(ufs_buf, UFS_FS_DBLKNO_OFF);
  ufs_priv.dsize_frags = ufs_get_u32(ufs_buf, UFS_FS_DSIZE_OFF);
  ufs_priv.inode_count = ufs_get_u32(ufs_buf, UFS_FS_IPG_OFF);
  ufs_priv.free_blocks = ufs_get_u32(ufs_buf, UFS_FS_CSTOTAL_NBFREE_OFF);
  ufs_priv.free_inodes = ufs_get_u32(ufs_buf, UFS_FS_CSTOTAL_NIFREE_OFF);

  /* Read sector containing the FS magic and verify */
  rc = ufs_read_sector(&ufs_priv,
                       UFS_SB_SECTOR + UFS_FS_MAGIC_OFF / BLKDEV_SECTOR_SIZE);
  if (rc < 0) return rc;
  uint32_t magic = ufs_get_u32(ufs_buf, UFS_FS_MAGIC_OFF % BLKDEV_SECTOR_SIZE);
  if (magic != UFS_MAGIC) return -EINVAL;

  /* Read CG descriptor (sector 32) for bitmap byte offsets */
  rc = ufs_read_sector(&ufs_priv, UFS_CG_SECTOR);
  if (rc < 0) return rc;
  ufs_priv.cg_iusedoff = ufs_get_u32(ufs_buf, UFS_CG_IUSEDOFF_OFF);
  ufs_priv.cg_freeoff = ufs_get_u32(ufs_buf, UFS_CG_FREEOFF_OFF);

  /* Read root inode */
  ufs_inode_t root_inode;
  rc = ufs_read_inode(&ufs_priv, UFS_ROOT_INO, &root_inode);
  if (rc < 0) return rc;

  mnt->sb_priv = &ufs_priv;

  vnode_t *root = ufs_vnode_from_inode(mnt, UFS_ROOT_INO, &root_inode);
  if (!root) return -ENOMEM;

  mnt->root = root;
  return 0;
}

/* ── ufs_lookup ───────────────────────────────────────────────────────── */

static int ufs_lookup(vnode_t *dir, const char *name, vnode_t **result) {
  ufs_priv_t *priv = (ufs_priv_t *)dir->fs_priv;

  ufs_inode_t *dir_inode = vfs_scratch_alloc();
  if (!dir_inode) return -ENOMEM;
  int rc = ufs_read_inode(priv, dir->ino, dir_inode);
  if (rc < 0) {
    vfs_scratch_free(dir_inode);
    return rc;
  }

  /* Compute target name length */
  uint32_t namlen = 0;
  while (name[namlen]) namlen++;
  if (namlen > UFS_NAME_MAX) {
    vfs_scratch_free(dir_inode);
    return -ENOENT;
  }

  uint32_t nblocks =
      ((uint32_t)dir_inode->i_size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE;

  for (uint32_t b = 0; b < nblocks; b++) {
    uint32_t phys;
    rc = ufs_block_map(priv, dir_inode, b, &phys);
    if (rc < 0) {
      vfs_scratch_free(dir_inode);
      return rc;
    }
    if (phys == 0) continue;

    for (uint32_t s = 0; s < UFS_FRAGS_PER_BLK; s++) {
      rc = ufs_read_sector(priv, phys + s);
      if (rc < 0) {
        vfs_scratch_free(dir_inode);
        return rc;
      }

      uint32_t off = 0;
      while (off + sizeof(ufs_dirent_t) <= BLKDEV_SECTOR_SIZE) {
        ufs_dirent_t *de = (ufs_dirent_t *)&ufs_buf[off];
        if (de->d_reclen == 0) break; /* corrupt sector */
        if (de->d_ino != 0 && de->d_namlen == (uint8_t)namlen) {
          const char *dn = (const char *)&ufs_buf[off + sizeof(ufs_dirent_t)];
          uint32_t i;
          for (i = 0; i < namlen && dn[i] == name[i]; i++) {
          }
          if (i == namlen) {
            /* Match — reuse dir_inode buffer for child inode
             * (dir_inode no longer needed for block_map) */
            uint32_t child_ino = de->d_ino;
            rc = ufs_read_inode(priv, child_ino, dir_inode);
            if (rc < 0) {
              vfs_scratch_free(dir_inode);
              return rc;
            }

            vnode_t *vn =
                ufs_vnode_from_inode(dir->mount, child_ino, dir_inode);
            vfs_scratch_free(dir_inode);
            if (!vn) return -ENOMEM;

            *result = vn;
            return 0;
          }
        }
        off += de->d_reclen;
      }
    }
  }

  vfs_scratch_free(dir_inode);
  return -ENOENT;
}

/* ── ufs_read ─────────────────────────────────────────────────────────── */

static long ufs_read(vnode_t *vn, page_id_t page, uint16_t page_off, size_t n,
                     uint32_t off) {
  if (vn->type == VNODE_DIR) return -(long)EISDIR;

  ufs_priv_t *priv = (ufs_priv_t *)vn->fs_priv;

  if (off >= vn->size) return 0;
  if (off + n > vn->size) n = vn->size - off;

  ufs_inode_t *inode = vfs_scratch_alloc();
  if (!inode) return -(long)ENOMEM;
  int rc = ufs_read_inode(priv, vn->ino, inode);
  if (rc < 0) {
    vfs_scratch_free(inode);
    return (long)rc;
  }

  uint32_t remaining = (uint32_t)n;
  uint32_t pos = off;

  while (remaining > 0) {
    uint32_t logical = pos / UFS_BLOCK_SIZE;
    uint32_t off_in_blk = pos % UFS_BLOCK_SIZE;
    uint32_t sec_in_blk = off_in_blk / BLKDEV_SECTOR_SIZE;
    uint32_t off_in_sec = off_in_blk % BLKDEV_SECTOR_SIZE;

    uint32_t phys;
    rc = ufs_block_map(priv, inode, logical, &phys);
    if (rc < 0) {
      vfs_scratch_free(inode);
      return (n - remaining > 0) ? (long)(n - remaining) : (long)rc;
    }
    if (phys == 0) break; /* hole / unallocated block */

    rc = ufs_read_sector(priv, phys + sec_in_blk);
    if (rc < 0) {
      vfs_scratch_free(inode);
      return (n - remaining > 0) ? (long)(n - remaining) : (long)rc;
    }

    uint32_t avail = BLKDEV_SECTOR_SIZE - off_in_sec;
    if (avail > remaining) avail = remaining;

    mod_core.mem_region_page_write(page, page_off, &ufs_buf[off_in_sec],
                                   (uint16_t)avail);
    page_off += (uint16_t)avail;
    pos += avail;
    remaining -= avail;
  }

  vfs_scratch_free(inode);
  return (long)(n - remaining);
}

/* ── ufs_write ─────────────────────────────────────────────────────────── */

static long ufs_write(vnode_t *vn, page_id_t page, uint16_t page_off, size_t n,
                      uint32_t off) {
  if (vn->type == VNODE_DIR) return -(long)EISDIR;

  ufs_priv_t *priv = (ufs_priv_t *)vn->fs_priv;

  ufs_inode_t *inode = vfs_scratch_alloc();
  if (!inode) return -(long)ENOMEM;
  int rc = ufs_read_inode(priv, vn->ino, inode);
  if (rc < 0) {
    vfs_scratch_free(inode);
    return (long)rc;
  }

  uint32_t remaining = (uint32_t)n;
  uint32_t pos = off;
  long ret;

  while (remaining > 0) {
    uint32_t logical = pos / UFS_BLOCK_SIZE;
    uint32_t off_in_blk = pos % UFS_BLOCK_SIZE;
    uint32_t sec_in_blk = off_in_blk / BLKDEV_SECTOR_SIZE;
    uint32_t off_in_sec = off_in_blk % BLKDEV_SECTOR_SIZE;

    /* Ensure the logical block is allocated */
    uint32_t phys;
    rc = ufs_block_map(priv, inode, logical, &phys);
    if (rc < 0) {
      ret = (n - remaining > 0) ? (long)(n - remaining) : (long)rc;
      goto out;
    }

    if (phys == 0) {
      rc = ufs_alloc_block(priv, &phys);
      if (rc < 0) {
        ret = (n - remaining > 0) ? (long)(n - remaining) : (long)rc;
        goto out;
      }
      rc = ufs_zero_block(priv, phys);
      if (rc < 0) {
        ret = (n - remaining > 0) ? (long)(n - remaining) : (long)rc;
        goto out;
      }
      rc = ufs_block_set(priv, inode, logical, phys);
      if (rc < 0) {
        ret = (n - remaining > 0) ? (long)(n - remaining) : (long)rc;
        goto out;
      }
    }

    uint32_t avail = BLKDEV_SECTOR_SIZE - off_in_sec;
    if (avail > remaining) avail = remaining;

    if (off_in_sec != 0 || avail < BLKDEV_SECTOR_SIZE) {
      rc = ufs_read_sector(priv, phys + sec_in_blk);
      if (rc < 0) {
        ret = (n - remaining > 0) ? (long)(n - remaining) : (long)rc;
        goto out;
      }
      mod_core.mem_region_page_read(page, page_off, &ufs_buf[off_in_sec],
                                    (uint16_t)avail);
    } else {
      mod_core.mem_region_page_read(page, page_off, ufs_buf,
                                    BLKDEV_SECTOR_SIZE);
    }

    rc = ufs_write_sector(priv, phys + sec_in_blk);
    if (rc < 0) {
      ret = (n - remaining > 0) ? (long)(n - remaining) : (long)rc;
      goto out;
    }

    page_off += (uint16_t)avail;
    pos += avail;
    remaining -= avail;
  }

  /* Update file size if extended */
  uint32_t end = off + (uint32_t)(n - remaining);
  if ((uint64_t)end > inode->i_size) {
    inode->i_size = end;
    vn->size = end;
  }

  rc = ufs_write_inode(priv, vn->ino, inode);
  if (rc < 0) {
    vfs_scratch_free(inode);
    return (long)rc;
  }

  ufs_sync_super(priv);
  vfs_scratch_free(inode);
  return (long)(n - remaining);

out:
  vfs_scratch_free(inode);
  return ret;
}

/* ── ufs_readdir ──────────────────────────────────────────────────────── */

static int ufs_readdir(vnode_t *dir, struct dirent *entries, size_t max_entries,
                       uint32_t *cookie) {
  ufs_priv_t *priv = (ufs_priv_t *)dir->fs_priv;

  ufs_inode_t *dir_inode = vfs_scratch_alloc();
  if (!dir_inode) return -ENOMEM;
  int rc = ufs_read_inode(priv, dir->ino, dir_inode);
  if (rc < 0) {
    vfs_scratch_free(dir_inode);
    return rc;
  }

  uint32_t nblocks =
      ((uint32_t)dir_inode->i_size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE;
  uint32_t entry_idx = 0;
  uint32_t target = *cookie;
  int count = 0;

  for (uint32_t b = 0; b < nblocks; b++) {
    uint32_t phys;
    rc = ufs_block_map(priv, dir_inode, b, &phys);
    if (rc < 0) {
      vfs_scratch_free(dir_inode);
      return rc;
    }
    if (phys == 0) continue;

    for (uint32_t s = 0; s < UFS_FRAGS_PER_BLK; s++) {
      rc = ufs_read_sector(priv, phys + s);
      if (rc < 0) {
        vfs_scratch_free(dir_inode);
        return rc;
      }

      uint32_t off = 0;
      while (off + sizeof(ufs_dirent_t) <= BLKDEV_SECTOR_SIZE) {
        ufs_dirent_t *de = (ufs_dirent_t *)&ufs_buf[off];
        if (de->d_reclen == 0) break; /* corrupt */
        if (de->d_ino == 0) {
          off += de->d_reclen;
          continue;
        }

        const char *dn = (const char *)&ufs_buf[off + sizeof(ufs_dirent_t)];

        /* Skip "." and ".." */
        if (de->d_namlen == 1 && dn[0] == '.') {
          entry_idx++;
          off += de->d_reclen;
          continue;
        }
        if (de->d_namlen == 2 && dn[0] == '.' && dn[1] == '.') {
          entry_idx++;
          off += de->d_reclen;
          continue;
        }

        if (entry_idx >= target) {
          if ((size_t)count >= max_entries) goto done;

          entries[count].d_ino = de->d_ino;

          /* Copy name bounded by VFS_NAME_MAX */
          uint8_t nl = de->d_namlen;
          if (nl > VFS_NAME_MAX) nl = VFS_NAME_MAX;
          uint8_t ni;
          for (ni = 0; ni < nl; ni++) entries[count].d_name[ni] = dn[ni];
          entries[count].d_name[ni] = '\0';

          /* Use d_type directly when set */
          switch (de->d_type) {
            case UFS_DT_DIR:
              entries[count].d_type = DT_DIR;
              break;
            case UFS_DT_LNK:
              entries[count].d_type = DT_LNK;
              break;
            case UFS_DT_REG:
              entries[count].d_type = DT_REG;
              break;
            default: {
              /* Fallback: read child inode (clobbers ufs_buf) */
              uint32_t child_ino = de->d_ino;
              ufs_inode_t *child = vfs_scratch_alloc();
              if (child && ufs_read_inode(priv, child_ino, child) == 0) {
                if (S_ISDIR(child->i_mode))
                  entries[count].d_type = DT_DIR;
                else if (S_ISLNK(child->i_mode))
                  entries[count].d_type = DT_LNK;
                else
                  entries[count].d_type = DT_REG;
              } else {
                entries[count].d_type = DT_REG;
              }
              vfs_scratch_free(child);
              /* Re-read directory sector (was clobbered) */
              rc = ufs_read_sector(priv, phys + s);
              if (rc < 0) goto done;
              de = (ufs_dirent_t *)&ufs_buf[off];
              dn = (const char *)&ufs_buf[off + sizeof(ufs_dirent_t)];
            }
          }

          count++;
        }
        entry_idx++;
        off += de->d_reclen;
      }
    }
  }

done:
  vfs_scratch_free(dir_inode);
  *cookie = entry_idx;
  return count;
}

/* ── ufs_dir_add_entry (helper for create/mkdir) ─────────────────────── */

/* Add a directory entry (name → child_ino) into dir.
 * Searches for a free slot (d_ino == 0) in existing blocks; if none
 * found, extends the directory by one block.  Clobbers ufs_buf. */
static int ufs_dir_add_entry(ufs_priv_t *priv, vnode_t *dir, const char *name,
                             uint32_t child_ino) {
  ufs_inode_t *dir_inode = vfs_scratch_alloc();
  if (!dir_inode) return -ENOMEM;
  int rc = ufs_read_inode(priv, dir->ino, dir_inode);
  if (rc < 0) {
    vfs_scratch_free(dir_inode);
    return rc;
  }

  /* Compute name length and minimum record size for the new entry */
  uint8_t namlen = 0;
  while (name[namlen] && namlen < UFS_NAME_MAX) namlen++;
  uint16_t new_min = UFS_DIRENT_MINREC(namlen);

  uint32_t nblocks =
      ((uint32_t)dir_inode->i_size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE;

  /* Scan existing sectors for a deleted or splittable slot */
  for (uint32_t b = 0; b < nblocks; b++) {
    uint32_t phys;
    rc = ufs_block_map(priv, dir_inode, b, &phys);
    if (rc < 0) {
      vfs_scratch_free(dir_inode);
      return rc;
    }
    if (phys == 0) continue;

    for (uint32_t s = 0; s < UFS_FRAGS_PER_BLK; s++) {
      rc = ufs_read_sector(priv, phys + s);
      if (rc < 0) {
        vfs_scratch_free(dir_inode);
        return rc;
      }

      uint32_t off = 0;
      while (off + sizeof(ufs_dirent_t) <= BLKDEV_SECTOR_SIZE) {
        ufs_dirent_t *de = (ufs_dirent_t *)&ufs_buf[off];
        if (de->d_reclen == 0) break;

        /* Deleted slot large enough to hold the new entry */
        if (de->d_ino == 0 && de->d_reclen >= new_min) {
          de->d_ino = child_ino;
          de->d_type = UFS_DT_UNKNOWN;
          de->d_namlen = namlen;
          char *dn = (char *)&ufs_buf[off + sizeof(ufs_dirent_t)];
          for (uint8_t i = 0; i < namlen; i++) dn[i] = name[i];
          vfs_scratch_free(dir_inode);
          return ufs_write_sector(priv, phys + s);
        }

        /* Live entry with enough slack to split */
        if (de->d_ino != 0) {
          uint16_t min_here = UFS_DIRENT_MINREC(de->d_namlen);
          uint16_t slack = de->d_reclen - min_here;
          if (slack >= new_min) {
            uint16_t old_rec = de->d_reclen;
            de->d_reclen = min_here;
            ufs_dirent_t *ne = (ufs_dirent_t *)&ufs_buf[off + min_here];
            ne->d_ino = child_ino;
            ne->d_reclen = old_rec - min_here;
            ne->d_type = UFS_DT_UNKNOWN;
            ne->d_namlen = namlen;
            char *dn = (char *)&ufs_buf[off + min_here + sizeof(ufs_dirent_t)];
            for (uint8_t i = 0; i < namlen; i++) dn[i] = name[i];
            vfs_scratch_free(dir_inode);
            return ufs_write_sector(priv, phys + s);
          }
        }

        off += de->d_reclen;
      }
    }
  }

  /* No room — extend directory by one block */
  uint32_t new_frag;
  rc = ufs_alloc_block(priv, &new_frag);
  if (rc < 0) {
    vfs_scratch_free(dir_inode);
    return rc;
  }

  rc = ufs_zero_block(priv, new_frag);
  if (rc < 0) {
    vfs_scratch_free(dir_inode);
    return rc;
  }

  /* Sector 0: new entry with reclen=DIRBLK_SIZE (fills chunk) */
  __builtin_memset(ufs_buf, 0, BLKDEV_SECTOR_SIZE);
  ufs_dirent_t *de = (ufs_dirent_t *)ufs_buf;
  de->d_ino = child_ino;
  de->d_reclen = UFS_DIRBLK_SIZE;
  de->d_type = UFS_DT_UNKNOWN;
  de->d_namlen = namlen;
  {
    char *dn = (char *)&ufs_buf[sizeof(ufs_dirent_t)];
    for (uint8_t i = 0; i < namlen; i++) dn[i] = name[i];
  }
  rc = ufs_write_sector(priv, new_frag);
  if (rc < 0) {
    vfs_scratch_free(dir_inode);
    return rc;
  }

  /* Sectors 1-7: placeholder chunks (ino=0, reclen=512) */
  __builtin_memset(ufs_buf, 0, BLKDEV_SECTOR_SIZE);
  ufs_dirent_t *ph = (ufs_dirent_t *)ufs_buf;
  ph->d_ino = 0;
  ph->d_reclen = UFS_DIRBLK_SIZE;
  for (uint32_t s = 1; s < UFS_FRAGS_PER_BLK; s++) {
    rc = ufs_write_sector(priv, new_frag + s);
    if (rc < 0) {
      vfs_scratch_free(dir_inode);
      return rc;
    }
  }

  /* Link new block into directory inode and update size */
  rc = ufs_block_set(priv, dir_inode, nblocks, new_frag);
  if (rc < 0) {
    vfs_scratch_free(dir_inode);
    return rc;
  }

  dir_inode->i_size += UFS_BLOCK_SIZE;
  rc = ufs_write_inode(priv, dir->ino, dir_inode);
  dir->size = (uint32_t)dir_inode->i_size;
  vfs_scratch_free(dir_inode);
  if (rc < 0) return rc;

  return 0;
}

/* ── ufs_create ──────────────────────────────────────────────────────── */

static int ufs_create(vnode_t *dir, const char *name, uint32_t mode,
                      vnode_t **result) {
  ufs_priv_t *priv = (ufs_priv_t *)dir->fs_priv;

  /* Allocate a new inode */
  uint32_t new_ino;
  int rc = ufs_alloc_inode(priv, &new_ino);
  if (rc < 0) return rc;

  /* Initialize the inode */
  ufs_inode_t *inode = vfs_scratch_alloc();
  if (!inode) {
    ufs_free_inode(priv, new_ino);
    return -ENOMEM;
  }
  __builtin_memset(inode, 0, sizeof(*inode));
  inode->i_mode = (uint16_t)(S_IFREG | (mode & 0777u));
  inode->i_nlink = 1;

  rc = ufs_write_inode(priv, new_ino, inode);
  if (rc < 0) {
    vfs_scratch_free(inode);
    ufs_free_inode(priv, new_ino);
    return rc;
  }

  /* Add entry to parent directory */
  rc = ufs_dir_add_entry(priv, dir, name, new_ino);
  if (rc < 0) {
    vfs_scratch_free(inode);
    ufs_free_inode(priv, new_ino);
    return rc;
  }

  ufs_sync_super(priv);

  /* Allocate and return vnode */
  vnode_t *vn = ufs_vnode_from_inode(dir->mount, new_ino, inode);
  vfs_scratch_free(inode);
  if (!vn) return -ENOMEM;

  *result = vn;
  return 0;
}

/* ── ufs_dir_remove_entry ─────────────────────────────────────────────── */

/* Remove a directory entry by name.  Sets *removed_ino to the inode
 * number of the removed entry.  Clobbers ufs_buf. */
static int ufs_dir_remove_entry(ufs_priv_t *priv, vnode_t *dir,
                                const char *name, uint32_t *removed_ino) {
  ufs_inode_t *dir_inode = vfs_scratch_alloc();
  if (!dir_inode) return -ENOMEM;
  int rc = ufs_read_inode(priv, dir->ino, dir_inode);
  if (rc < 0) {
    vfs_scratch_free(dir_inode);
    return rc;
  }

  uint8_t namlen = 0;
  while (name[namlen] && namlen < UFS_NAME_MAX) namlen++;

  uint32_t nblocks =
      ((uint32_t)dir_inode->i_size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE;

  for (uint32_t b = 0; b < nblocks; b++) {
    uint32_t phys;
    rc = ufs_block_map(priv, dir_inode, b, &phys);
    if (rc < 0) {
      vfs_scratch_free(dir_inode);
      return rc;
    }
    if (phys == 0) continue;

    for (uint32_t s = 0; s < UFS_FRAGS_PER_BLK; s++) {
      rc = ufs_read_sector(priv, phys + s);
      if (rc < 0) {
        vfs_scratch_free(dir_inode);
        return rc;
      }

      uint32_t off = 0;
      while (off + sizeof(ufs_dirent_t) <= BLKDEV_SECTOR_SIZE) {
        ufs_dirent_t *de = (ufs_dirent_t *)&ufs_buf[off];
        if (de->d_reclen == 0) break;
        if (de->d_ino != 0 && de->d_namlen == namlen) {
          const char *dn = (const char *)&ufs_buf[off + sizeof(ufs_dirent_t)];
          uint8_t i;
          for (i = 0; i < namlen && dn[i] == name[i]; i++) {
          }
          if (i == namlen) {
            *removed_ino = de->d_ino;
            de->d_ino = 0;
            vfs_scratch_free(dir_inode);
            return ufs_write_sector(priv, phys + s);
          }
        }
        off += de->d_reclen;
      }
    }
  }
  vfs_scratch_free(dir_inode);
  return -ENOENT;
}

/* ── ufs_dir_is_empty ────────────────────────────────────────────────── */

/* Check if a directory has no entries beyond "." and "..".
 * Returns 1 if empty, 0 if non-empty, negative on error. */
static int ufs_dir_is_empty(ufs_priv_t *priv, uint32_t dir_ino) {
  ufs_inode_t *dir_inode = vfs_scratch_alloc();
  if (!dir_inode) return -ENOMEM;
  int rc = ufs_read_inode(priv, dir_ino, dir_inode);
  if (rc < 0) {
    vfs_scratch_free(dir_inode);
    return rc;
  }

  uint32_t nblocks =
      ((uint32_t)dir_inode->i_size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE;

  for (uint32_t b = 0; b < nblocks; b++) {
    uint32_t phys;
    rc = ufs_block_map(priv, dir_inode, b, &phys);
    if (rc < 0) {
      vfs_scratch_free(dir_inode);
      return rc;
    }
    if (phys == 0) continue;

    for (uint32_t s = 0; s < UFS_FRAGS_PER_BLK; s++) {
      rc = ufs_read_sector(priv, phys + s);
      if (rc < 0) {
        vfs_scratch_free(dir_inode);
        return rc;
      }

      uint32_t off = 0;
      while (off + sizeof(ufs_dirent_t) <= BLKDEV_SECTOR_SIZE) {
        ufs_dirent_t *de = (ufs_dirent_t *)&ufs_buf[off];
        if (de->d_reclen == 0) break;
        if (de->d_ino == 0) {
          off += de->d_reclen;
          continue;
        }

        const char *dn = (const char *)&ufs_buf[off + sizeof(ufs_dirent_t)];
        /* Skip "." */
        if (de->d_namlen == 1 && dn[0] == '.') {
          off += de->d_reclen;
          continue;
        }
        /* Skip ".." */
        if (de->d_namlen == 2 && dn[0] == '.' && dn[1] == '.') {
          off += de->d_reclen;
          continue;
        }
        vfs_scratch_free(dir_inode);
        return 0; /* non-empty */
      }
    }
  }
  vfs_scratch_free(dir_inode);
  return 1; /* empty */
}

/* ── ufs_mkdir ───────────────────────────────────────────────────────── */

static int ufs_mkdir(vnode_t *dir, const char *name, uint32_t mode) {
  ufs_priv_t *priv = (ufs_priv_t *)dir->fs_priv;

  /* Allocate inode for new directory */
  uint32_t new_ino;
  int rc = ufs_alloc_inode(priv, &new_ino);
  if (rc < 0) return rc;

  /* Allocate data block */
  uint32_t data_frag;
  rc = ufs_alloc_block(priv, &data_frag);
  if (rc < 0) {
    ufs_free_inode(priv, new_ino);
    return rc;
  }

  rc = ufs_zero_block(priv, data_frag);
  if (rc < 0) {
    ufs_free_block(priv, data_frag);
    ufs_free_inode(priv, new_ino);
    return rc;
  }

  /* Sector 0: "." (reclen=12, namlen=1) + ".." (reclen=500, namlen=2)
   * The two together fill exactly 512 bytes (one DIRBLK chunk). */
  __builtin_memset(ufs_buf, 0, BLKDEV_SECTOR_SIZE);
  ufs_dirent_t *dot = (ufs_dirent_t *)ufs_buf;
  dot->d_ino = new_ino;
  dot->d_reclen = UFS_DIRENT_MINREC(1); /* 12 */
  dot->d_type = UFS_DT_DIR;
  dot->d_namlen = 1;
  ufs_buf[sizeof(ufs_dirent_t)] = '.';

  uint16_t dot_rec = dot->d_reclen;
  ufs_dirent_t *dotdot = (ufs_dirent_t *)&ufs_buf[dot_rec];
  dotdot->d_ino = dir->ino;
  dotdot->d_reclen = (uint16_t)(UFS_DIRBLK_SIZE - dot_rec);
  dotdot->d_type = UFS_DT_DIR;
  dotdot->d_namlen = 2;
  ufs_buf[dot_rec + sizeof(ufs_dirent_t)] = '.';
  ufs_buf[dot_rec + sizeof(ufs_dirent_t) + 1] = '.';

  rc = ufs_write_sector(priv, data_frag);
  if (rc < 0) {
    ufs_free_block(priv, data_frag);
    ufs_free_inode(priv, new_ino);
    return rc;
  }

  /* Sectors 1-7: placeholder chunks (ino=0, reclen=512) */
  __builtin_memset(ufs_buf, 0, BLKDEV_SECTOR_SIZE);
  ufs_dirent_t *ph = (ufs_dirent_t *)ufs_buf;
  ph->d_ino = 0;
  ph->d_reclen = UFS_DIRBLK_SIZE;
  for (uint32_t s = 1; s < UFS_FRAGS_PER_BLK; s++) {
    rc = ufs_write_sector(priv, data_frag + s);
    if (rc < 0) {
      ufs_free_block(priv, data_frag);
      ufs_free_inode(priv, new_ino);
      return rc;
    }
  }

  /* Initialize and write the new directory inode.  Reuse a single
   * scratch buffer for both the new inode and the parent update. */
  ufs_inode_t *inode = vfs_scratch_alloc();
  if (!inode) {
    ufs_free_block(priv, data_frag);
    ufs_free_inode(priv, new_ino);
    return -ENOMEM;
  }
  __builtin_memset(inode, 0, sizeof(*inode));
  inode->i_mode = (uint16_t)(S_IFDIR | (mode & 0777u));
  inode->i_nlink = 2; /* "." from self + entry from parent */
  inode->i_size = UFS_BLOCK_SIZE;
  inode->i_direct[0] = data_frag;

  rc = ufs_write_inode(priv, new_ino, inode);
  if (rc < 0) {
    vfs_scratch_free(inode);
    return rc;
  }

  /* Add entry in parent directory */
  rc = ufs_dir_add_entry(priv, dir, name, new_ino);
  if (rc < 0) {
    vfs_scratch_free(inode);
    return rc;
  }

  /* Increment parent's nlink (for ".." backlink).
   * Reuse the same scratch buffer — new inode data no longer needed. */
  rc = ufs_read_inode(priv, dir->ino, inode);
  if (rc < 0) {
    vfs_scratch_free(inode);
    return rc;
  }
  inode->i_nlink++;
  rc = ufs_write_inode(priv, dir->ino, inode);
  vfs_scratch_free(inode);
  if (rc < 0) return rc;

  ufs_sync_super(priv);
  return 0;
}

/* ── ufs_truncate ────────────────────────────────────────────────────── */

static int ufs_truncate(vnode_t *vn, uint32_t length) {
  if (vn->type == VNODE_DIR) return -EISDIR;

  ufs_priv_t *priv = (ufs_priv_t *)vn->fs_priv;

  ufs_inode_t *inode = vfs_scratch_alloc();
  if (!inode) return -ENOMEM;
  int rc = ufs_read_inode(priv, vn->ino, inode);
  if (rc < 0) {
    vfs_scratch_free(inode);
    return rc;
  }

  if ((uint64_t)length >= inode->i_size) {
    /* Extend (or no change) — just update size */
    inode->i_size = length;
  } else if (length == 0) {
    /* Truncate to zero — free all blocks */
    for (int i = 0; i < UFS_DIRECT_BLOCKS; i++) {
      if (inode->i_direct[i] != 0) {
        ufs_free_block(priv, inode->i_direct[i]);
        inode->i_direct[i] = 0;
      }
    }

    if (inode->i_ib[0] != 0) {
      for (uint32_t s = 0; s < UFS_FRAGS_PER_BLK; s++) {
        rc = ufs_read_sector(priv, inode->i_ib[0] + s);
        if (rc < 0) break;
        ufs_free_indirect_ptrs(priv, 0, BLKDEV_SECTOR_SIZE / sizeof(uint32_t));
      }
      ufs_free_block(priv, inode->i_ib[0]);
      inode->i_ib[0] = 0;
    }
    if (inode->i_ib[1] != 0) {
      ufs_free_double_indirect(priv, inode->i_ib[1], 0);
      inode->i_ib[1] = 0;
    }
    inode->i_size = 0;
  } else {
    /* General shrink: free blocks beyond the new length */
    uint32_t keep_blocks = (length + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE;
    uint32_t ptrs_per_block = UFS_BLOCK_SIZE / sizeof(uint32_t);
    uint32_t sind_threshold = UFS_DIRECT_BLOCKS + ptrs_per_block;

    for (uint32_t i = keep_blocks; i < UFS_DIRECT_BLOCKS; i++) {
      if (inode->i_direct[i] != 0) {
        ufs_free_block(priv, inode->i_direct[i]);
        inode->i_direct[i] = 0;
      }
    }

    if (inode->i_ib[0] != 0 && keep_blocks <= UFS_DIRECT_BLOCKS) {
      for (uint32_t s = 0; s < UFS_FRAGS_PER_BLK; s++) {
        rc = ufs_read_sector(priv, inode->i_ib[0] + s);
        if (rc < 0) break;
        ufs_free_indirect_ptrs(priv, 0, BLKDEV_SECTOR_SIZE / sizeof(uint32_t));
      }
      ufs_free_block(priv, inode->i_ib[0]);
      inode->i_ib[0] = 0;
    } else if (inode->i_ib[0] != 0 && keep_blocks > UFS_DIRECT_BLOCKS &&
               keep_blocks < sind_threshold) {
      uint32_t first_free_ind = keep_blocks - UFS_DIRECT_BLOCKS;
      uint32_t ptrs_per_sec = BLKDEV_SECTOR_SIZE / sizeof(uint32_t);
      for (uint32_t s = first_free_ind / ptrs_per_sec; s < UFS_FRAGS_PER_BLK;
           s++) {
        rc = ufs_read_sector(priv, inode->i_ib[0] + s);
        if (rc < 0) break;
        uint32_t start = (s == first_free_ind / ptrs_per_sec)
                             ? first_free_ind % ptrs_per_sec
                             : 0;
        ufs_free_indirect_ptrs(priv, start, ptrs_per_sec);
      }
    }

    if (inode->i_ib[1] != 0) {
      if (keep_blocks <= sind_threshold) {
        ufs_free_double_indirect(priv, inode->i_ib[1], 0);
        inode->i_ib[1] = 0;
      } else {
        ufs_free_double_indirect(priv, inode->i_ib[1],
                                 keep_blocks - sind_threshold);
      }
    }
    inode->i_size = length;
  }

  vn->size = (uint32_t)inode->i_size;
  rc = ufs_write_inode(priv, vn->ino, inode);
  vfs_scratch_free(inode);
  if (rc < 0) return rc;

  ufs_sync_super(priv);
  return 0;
}

/* ── ufs_unlink ──────────────────────────────────────────────────────── */

static int ufs_unlink(vnode_t *dir, const char *name) {
  ufs_priv_t *priv = (ufs_priv_t *)dir->fs_priv;

  /* Remove directory entry and get the child inode number */
  uint32_t child_ino = 0;
  int rc = ufs_dir_remove_entry(priv, dir, name, &child_ino);
  if (rc < 0) return rc;

  /* Read child inode — use single scratch buffer, reused for parent */
  ufs_inode_t *child = vfs_scratch_alloc();
  if (!child) return -ENOMEM;
  rc = ufs_read_inode(priv, child_ino, child);
  if (rc < 0) {
    vfs_scratch_free(child);
    return rc;
  }

  /* If directory: check it's empty, decrement parent nlink */
  if (S_ISDIR(child->i_mode)) {
    int empty = ufs_dir_is_empty(priv, child_ino);
    if (empty <= 0) {
      ufs_dir_add_entry(priv, dir, name, child_ino);
      vfs_scratch_free(child);
      return (empty == 0) ? -ENOTEMPTY : empty;
    }

    /* Decrement parent's nlink.  child is still needed below, so
     * allocate a second scratch for the parent inode. */
    ufs_inode_t *parent = vfs_scratch_alloc();
    if (parent) {
      rc = ufs_read_inode(priv, dir->ino, parent);
      if (rc == 0 && parent->i_nlink > 0) {
        parent->i_nlink--;
        ufs_write_inode(priv, dir->ino, parent);
      }
      vfs_scratch_free(parent);
    }
  }

  if (child->i_nlink > 0) child->i_nlink--;

  if (child->i_nlink == 0) {
    for (int i = 0; i < UFS_DIRECT_BLOCKS; i++) {
      if (child->i_direct[i] != 0) {
        ufs_free_block(priv, child->i_direct[i]);
        child->i_direct[i] = 0;
      }
    }

    if (child->i_ib[0] != 0) {
      for (uint32_t s = 0; s < UFS_FRAGS_PER_BLK; s++) {
        rc = ufs_read_sector(priv, child->i_ib[0] + s);
        if (rc < 0) break;
        ufs_free_indirect_ptrs(priv, 0, BLKDEV_SECTOR_SIZE / sizeof(uint32_t));
      }
      ufs_free_block(priv, child->i_ib[0]);
      child->i_ib[0] = 0;
    }
    if (child->i_ib[1] != 0) {
      ufs_free_double_indirect(priv, child->i_ib[1], 0);
      child->i_ib[1] = 0;
    }

    child->i_size = 0;
    ufs_write_inode(priv, child_ino, child);
    ufs_free_inode(priv, child_ino);
  } else {
    ufs_write_inode(priv, child_ino, child);
  }

  vfs_scratch_free(child);
  ufs_sync_super(priv);
  return 0;
}

/* ── ufs_stat ─────────────────────────────────────────────────────────── */

static int ufs_stat(vnode_t *vn, struct stat *st) {
  ufs_priv_t *priv = (ufs_priv_t *)vn->fs_priv;

  ufs_inode_t *inode = vfs_scratch_alloc();
  if (!inode) return -ENOMEM;
  int rc = ufs_read_inode(priv, vn->ino, inode);
  if (rc < 0) {
    vfs_scratch_free(inode);
    return rc;
  }

  st->st_ino = vn->ino;
  st->st_mode = inode->i_mode;
  st->st_nlink = inode->i_nlink;
  st->st_size = (uint32_t)inode->i_size;
  st->st_mtime = inode->i_mtime;
  st->st_ctime = inode->i_ctime;
  st->st_atime = inode->i_atime;
  vfs_scratch_free(inode);
  return 0;
}

/* ── ufs_readlink ─────────────────────────────────────────────────────── */

static long ufs_readlink(vnode_t *vn, char *buf, size_t bufsiz) {
  if (vn->type != VNODE_SYMLINK) return -(long)EINVAL;

  ufs_priv_t *priv = (ufs_priv_t *)vn->fs_priv;

  ufs_inode_t *inode = vfs_scratch_alloc();
  if (!inode) return -(long)ENOMEM;
  int rc = ufs_read_inode(priv, vn->ino, inode);
  if (rc < 0) {
    vfs_scratch_free(inode);
    return (long)rc;
  }

  uint32_t len = (uint32_t)inode->i_size;
  if (len > (uint32_t)bufsiz) len = (uint32_t)bufsiz;

  if (inode->i_size <= (uint64_t)UFS_FAST_SYMLINK_MAX) {
    if (len > UFS_FAST_SYMLINK_MAX) len = UFS_FAST_SYMLINK_MAX;
    __builtin_memcpy(buf, inode->i_direct, len);
    vfs_scratch_free(inode);
    return (long)len;
  }

  if (inode->i_direct[0] == 0) {
    vfs_scratch_free(inode);
    return -EIO;
  }

  rc = ufs_read_sector(priv, inode->i_direct[0]);
  vfs_scratch_free(inode);
  if (rc < 0) return (long)rc;

  if (len > BLKDEV_SECTOR_SIZE) len = BLKDEV_SECTOR_SIZE;
  __builtin_memcpy(buf, ufs_buf, len);
  return (long)len;
}

/* ── ufs_statfs ──────────────────────────────────────────────────────── */

static int ufs_statfs(mount_entry_t *mnt, struct kernel_statfs *buf) {
  ufs_priv_t *priv = (ufs_priv_t *)mnt->sb_priv;
  if (!priv) return -EINVAL;

  __builtin_memset(buf, 0, sizeof(*buf));

  buf->f_type = UFS_MAGIC;
  buf->f_bsize = UFS_BLOCK_SIZE;
  buf->f_frsize = UFS_BLOCK_SIZE;
  buf->f_blocks = priv->dsize_frags / UFS_FRAGS_PER_BLK;
  buf->f_bfree = priv->free_blocks;
  buf->f_bavail = priv->free_blocks;
  buf->f_files = priv->inode_count;
  buf->f_ffree = priv->free_inodes;
  buf->f_namelen = VFS_NAME_MAX;
  return 0;
}

/* ── SPIN_FS wrappers ─────────────────────────────────────────────────── */
/*
 * Each VFS entry point acquires SPIN_FS to serialize access to ufs_buf.
 * ufs_statfs uses only cached values, but wrapped for consistency.
 */

static int ufs_mount_locked(mount_entry_t *mnt, const void *dev_data) {
  spin_lock(SPIN_FS);
  int r = ufs_mount(mnt, dev_data);
  spin_unlock(SPIN_FS);
  return r;
}

static int ufs_lookup_locked(vnode_t *dir, const char *name, vnode_t **result) {
  spin_lock(SPIN_FS);
  int r = ufs_lookup(dir, name, result);
  spin_unlock(SPIN_FS);
  return r;
}

static long ufs_read_locked(vnode_t *vn, page_id_t page, uint16_t page_off,
                            size_t n, uint32_t off) {
  spin_lock(SPIN_FS);
  long r = ufs_read(vn, page, page_off, n, off);
  spin_unlock(SPIN_FS);
  return r;
}

static long ufs_write_locked(vnode_t *vn, page_id_t page, uint16_t page_off,
                             size_t n, uint32_t off) {
  spin_lock(SPIN_FS);
  long r = ufs_write(vn, page, page_off, n, off);
  spin_unlock(SPIN_FS);
  return r;
}

static int ufs_readdir_locked(vnode_t *dir, struct dirent *entries,
                              size_t max_entries, uint32_t *cookie) {
  spin_lock(SPIN_FS);
  int r = ufs_readdir(dir, entries, max_entries, cookie);
  spin_unlock(SPIN_FS);
  return r;
}

static int ufs_stat_locked(vnode_t *vn, struct stat *st) {
  spin_lock(SPIN_FS);
  int r = ufs_stat(vn, st);
  spin_unlock(SPIN_FS);
  return r;
}

static long ufs_readlink_locked(vnode_t *vn, char *buf, size_t bufsiz) {
  spin_lock(SPIN_FS);
  long r = ufs_readlink(vn, buf, bufsiz);
  spin_unlock(SPIN_FS);
  return r;
}

static int ufs_create_locked(vnode_t *dir, const char *name, uint32_t mode,
                             vnode_t **result) {
  spin_lock(SPIN_FS);
  int r = ufs_create(dir, name, mode, result);
  spin_unlock(SPIN_FS);
  return r;
}

static int ufs_mkdir_locked(vnode_t *dir, const char *name, uint32_t mode) {
  spin_lock(SPIN_FS);
  int r = ufs_mkdir(dir, name, mode);
  spin_unlock(SPIN_FS);
  return r;
}

static int ufs_unlink_locked(vnode_t *dir, const char *name) {
  spin_lock(SPIN_FS);
  int r = ufs_unlink(dir, name);
  spin_unlock(SPIN_FS);
  return r;
}

static int ufs_truncate_locked(vnode_t *vn, uint32_t length) {
  spin_lock(SPIN_FS);
  int r = ufs_truncate(vn, length);
  spin_unlock(SPIN_FS);
  return r;
}

static int ufs_statfs_locked(mount_entry_t *mnt, struct kernel_statfs *buf) {
  spin_lock(SPIN_FS);
  int r = ufs_statfs(mnt, buf);
  spin_unlock(SPIN_FS);
  return r;
}

/* ── Operations table ─────────────────────────────────────────────────── */

const vfs_ops_t ufs_ops = {
    .mount = ufs_mount_locked,
    .lookup = ufs_lookup_locked,
    .read = ufs_read_locked,
    .write = ufs_write_locked,
    .readdir = ufs_readdir_locked,
    .stat = ufs_stat_locked,
    .readlink = ufs_readlink_locked,
    .create = ufs_create_locked,
    .mkdir = ufs_mkdir_locked,
    .unlink = ufs_unlink_locked,
    .truncate = ufs_truncate_locked,
    .statfs = ufs_statfs_locked,
};
