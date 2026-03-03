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

#include "ufs.h"
#include "ufs_format.h"
#include "../vfs/vfs.h"
#include "../blkdev/blkdev.h"
#include "drivers/uart.h"
#include "../errno.h"
#include <stddef.h>

/* ── Constants ────────────────────────────────────────────────────────── */

#define SECTORS_PER_BLOCK  (UFS_BLOCK_SIZE / BLKDEV_SECTOR_SIZE)  /* 8 */
#define INODES_PER_SECTOR  (BLKDEV_SECTOR_SIZE / UFS_INODE_SIZE)  /* 8 */
#define DIRENTS_PER_SECTOR (BLKDEV_SECTOR_SIZE / UFS_DIRENT_SIZE) /* 16 */

/* ── In-memory superblock ────────────────────────────────────────────── */

typedef struct {
    blkdev_t *dev;
    uint32_t  block_count;
    uint32_t  inode_count;
    uint32_t  itable_block;    /* first inode table block */
    uint32_t  data_block;      /* first data block */
    uint32_t  inode_blocks;    /* number of inode table blocks */
    uint32_t  bmap_block;      /* block bitmap block (1) */
    uint32_t  imap_block;      /* inode bitmap block (2) */
    uint32_t  free_blocks;     /* cached s_free_blocks */
    uint32_t  free_inodes;     /* cached s_free_inodes */
} ufs_priv_t;

static ufs_priv_t ufs_priv;

/* ── Sector I/O buffer (single-threaded kernel) ──────────────────────── */

static uint8_t ufs_buf[BLKDEV_SECTOR_SIZE];

/* ── Block I/O helpers ─────────────────────────────────────────────── */

/* Read one sector from the underlying block device into ufs_buf */
static int ufs_read_sector(ufs_priv_t *priv, uint32_t abs_sector)
{
    return priv->dev->read(priv->dev, ufs_buf, abs_sector, 1);
}

/* Read one sector within a UFS block into ufs_buf */
static int ufs_read_blk_sector(ufs_priv_t *priv, uint32_t block,
                                uint32_t sec_within)
{
    return ufs_read_sector(priv, block * SECTORS_PER_BLOCK + sec_within);
}

/* ── Block write helpers ──────────────────────────────────────────────── */

/* Write ufs_buf to one sector on the underlying block device */
static int ufs_write_sector(ufs_priv_t *priv, uint32_t abs_sector)
{
    return priv->dev->write(priv->dev, ufs_buf, abs_sector, 1);
}

/* Write ufs_buf to one sector within a UFS block */
static int ufs_write_blk_sector(ufs_priv_t *priv, uint32_t block,
                                 uint32_t sec_within)
{
    return ufs_write_sector(priv, block * SECTORS_PER_BLOCK + sec_within);
}

/* ── Inode I/O ────────────────────────────────────────────────────────── */

/* Read an on-disk inode.  Reads a single sector of the inode table
 * and copies the 64-byte inode struct.  Clobbers ufs_buf. */
static int ufs_read_inode(ufs_priv_t *priv, uint32_t ino, ufs_inode_t *out)
{
    uint32_t block = priv->itable_block + ino / UFS_INODES_PER_BLOCK;
    uint32_t idx   = ino % UFS_INODES_PER_BLOCK;
    uint32_t sec   = idx / INODES_PER_SECTOR;
    uint32_t off   = (idx % INODES_PER_SECTOR) * UFS_INODE_SIZE;

    int rc = ufs_read_blk_sector(priv, block, sec);
    if (rc < 0) return rc;

    __builtin_memcpy(out, &ufs_buf[off], UFS_INODE_SIZE);
    return 0;
}

/* Write an on-disk inode.  Reads the inode table sector, patches the
 * 64-byte inode struct, and writes the sector back.  Clobbers ufs_buf. */
static int ufs_write_inode(ufs_priv_t *priv, uint32_t ino,
                            const ufs_inode_t *inode)
{
    uint32_t block = priv->itable_block + ino / UFS_INODES_PER_BLOCK;
    uint32_t idx   = ino % UFS_INODES_PER_BLOCK;
    uint32_t sec   = idx / INODES_PER_SECTOR;
    uint32_t off   = (idx % INODES_PER_SECTOR) * UFS_INODE_SIZE;

    int rc = ufs_read_blk_sector(priv, block, sec);
    if (rc < 0) return rc;

    __builtin_memcpy(&ufs_buf[off], inode, UFS_INODE_SIZE);
    return ufs_write_blk_sector(priv, block, sec);
}

/* ── Bitmap helpers ──────────────────────────────────────────────────── */

/* Bits per bitmap sector */
#define BITS_PER_SECTOR (BLKDEV_SECTOR_SIZE * 8)  /* 4096 */

/* Test, set, clear a bit within the current ufs_buf contents.
 * `bit` is relative to the start of the sector (0..4095). */
static int ufs_bmap_test(uint32_t bit)
{
    return (ufs_buf[bit / 8] >> (bit % 8)) & 1;
}

static void ufs_bmap_set(uint32_t bit)
{
    ufs_buf[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static void ufs_bmap_clear(uint32_t bit)
{
    ufs_buf[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

/* ── Block allocation ────────────────────────────────────────────────── */

/* Allocate a free data block.  Returns 0 on success (*out = block number)
 * or -ENOSPC if the filesystem is full.  Clobbers ufs_buf. */
static int ufs_alloc_block(ufs_priv_t *priv, uint32_t *out)
{
    for (uint32_t s = 0; s < SECTORS_PER_BLOCK; s++) {
        int rc = ufs_read_blk_sector(priv, priv->bmap_block, s);
        if (rc < 0) return rc;

        for (uint32_t bit = 0; bit < BITS_PER_SECTOR; bit++) {
            uint32_t blkno = s * BITS_PER_SECTOR + bit;
            if (blkno >= priv->block_count) return -ENOSPC;
            if (ufs_bmap_test(bit)) continue;  /* already allocated */

            ufs_bmap_set(bit);
            rc = ufs_write_blk_sector(priv, priv->bmap_block, s);
            if (rc < 0) return rc;

            priv->free_blocks--;
            *out = blkno;
            return 0;
        }
    }
    return -ENOSPC;
}

/* Free a previously allocated block.  Clobbers ufs_buf. */
static int ufs_free_block(ufs_priv_t *priv, uint32_t blkno)
{
    if (blkno >= priv->block_count) return -EINVAL;

    uint32_t sec = blkno / BITS_PER_SECTOR;
    uint32_t bit = blkno % BITS_PER_SECTOR;

    int rc = ufs_read_blk_sector(priv, priv->bmap_block, sec);
    if (rc < 0) return rc;

    ufs_bmap_clear(bit);
    rc = ufs_write_blk_sector(priv, priv->bmap_block, sec);
    if (rc < 0) return rc;

    priv->free_blocks++;
    return 0;
}

/* ── Inode allocation ────────────────────────────────────────────────── */

/* Allocate a free inode.  Returns 0 on success (*out = inode number)
 * or -ENOSPC.  Clobbers ufs_buf. */
static int ufs_alloc_inode(ufs_priv_t *priv, uint32_t *out)
{
    for (uint32_t s = 0; s < SECTORS_PER_BLOCK; s++) {
        int rc = ufs_read_blk_sector(priv, priv->imap_block, s);
        if (rc < 0) return rc;

        for (uint32_t bit = 0; bit < BITS_PER_SECTOR; bit++) {
            uint32_t ino = s * BITS_PER_SECTOR + bit;
            if (ino >= priv->inode_count) return -ENOSPC;
            if (ufs_bmap_test(bit)) continue;

            ufs_bmap_set(bit);
            rc = ufs_write_blk_sector(priv, priv->imap_block, s);
            if (rc < 0) return rc;

            priv->free_inodes--;
            *out = ino;
            return 0;
        }
    }
    return -ENOSPC;
}

/* Free a previously allocated inode.  Clobbers ufs_buf. */
static int ufs_free_inode(ufs_priv_t *priv, uint32_t ino)
{
    if (ino >= priv->inode_count) return -EINVAL;

    uint32_t sec = ino / BITS_PER_SECTOR;
    uint32_t bit = ino % BITS_PER_SECTOR;

    int rc = ufs_read_blk_sector(priv, priv->imap_block, sec);
    if (rc < 0) return rc;

    ufs_bmap_clear(bit);
    rc = ufs_write_blk_sector(priv, priv->imap_block, sec);
    if (rc < 0) return rc;

    priv->free_inodes++;
    return 0;
}

/* ── Superblock sync ─────────────────────────────────────────────────── */

/* Write cached free counts back to the on-disk superblock.  Clobbers ufs_buf. */
static int ufs_sync_super(ufs_priv_t *priv)
{
    int rc = ufs_read_sector(priv, 0);
    if (rc < 0) return rc;

    ufs_super_t *sb = (ufs_super_t *)ufs_buf;
    sb->s_free_blocks = priv->free_blocks;
    sb->s_free_inodes = priv->free_inodes;

    return ufs_write_sector(priv, 0);
}

/* ── Data block zeroing ──────────────────────────────────────────────── */

/* Zero all 8 sectors of a UFS block.  Clobbers ufs_buf. */
static int ufs_zero_block(ufs_priv_t *priv, uint32_t block)
{
    __builtin_memset(ufs_buf, 0, BLKDEV_SECTOR_SIZE);
    for (uint32_t s = 0; s < SECTORS_PER_BLOCK; s++) {
        int rc = ufs_write_blk_sector(priv, block, s);
        if (rc < 0) return rc;
    }
    return 0;
}

/* ── Allocation self-test (called from main_qemu.c) ──────────────────── */

static void alloc_check(const char *name, int ok, int *pass, int *fail)
{
    uart_puts("TEST: ");
    uart_puts(name);
    if (ok) { uart_puts(" ... PASS\n"); (*pass)++; }
    else    { uart_puts(" ... FAIL\n"); (*fail)++; }
}

void ufs_alloc_selftest(int *out_pass, int *out_fail)
{
    int pass = 0, fail = 0;
    uint32_t orig_fb = ufs_priv.free_blocks;
    uint32_t orig_fi = ufs_priv.free_inodes;

    /* 1. Alloc block → free_blocks decremented */
    uint32_t blk = 0;
    int rc = ufs_alloc_block(&ufs_priv, &blk);
    alloc_check("alloc_block", rc == 0 && blk >= ufs_priv.data_block
                && ufs_priv.free_blocks == orig_fb - 1, &pass, &fail);

    /* 2. Free block → free_blocks restored */
    rc = ufs_free_block(&ufs_priv, blk);
    alloc_check("free_block", rc == 0
                && ufs_priv.free_blocks == orig_fb, &pass, &fail);

    /* 3. Alloc inode → free_inodes decremented */
    uint32_t ino = 0;
    rc = ufs_alloc_inode(&ufs_priv, &ino);
    alloc_check("alloc_inode", rc == 0 && ino >= 2
                && ufs_priv.free_inodes == orig_fi - 1, &pass, &fail);

    /* 4. Free inode → free_inodes restored */
    rc = ufs_free_inode(&ufs_priv, ino);
    alloc_check("free_inode", rc == 0
                && ufs_priv.free_inodes == orig_fi, &pass, &fail);

    /* 5. Write inode + read back → data matches */
    rc = ufs_alloc_inode(&ufs_priv, &ino);
    if (rc == 0) {
        ufs_inode_t test_in;
        __builtin_memset(&test_in, 0, sizeof(test_in));
        test_in.i_mode  = 0100644;
        test_in.i_nlink = 1;
        test_in.i_size  = 12345;
        test_in.i_direct[0] = 42;

        rc = ufs_write_inode(&ufs_priv, ino, &test_in);
        ufs_inode_t test_out;
        int rc2 = ufs_read_inode(&ufs_priv, ino, &test_out);
        alloc_check("write+read inode",
                     rc == 0 && rc2 == 0
                     && test_out.i_mode == 0100644
                     && test_out.i_nlink == 1
                     && test_out.i_size == 12345
                     && test_out.i_direct[0] == 42,
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
    int rc2 = ufs_read_sector(&ufs_priv, 0);
    ufs_super_t *sb = (ufs_super_t *)ufs_buf;
    alloc_check("sync_super",
                 rc == 0 && rc2 == 0
                 && sb->s_free_blocks == ufs_priv.free_blocks
                 && sb->s_free_inodes == ufs_priv.free_inodes,
                 &pass, &fail);

    *out_pass = pass;
    *out_fail = fail;
}

/* ── Block mapping ────────────────────────────────────────────────────── */

/* Resolve a logical file block index to a physical block number.
 * Handles direct (blocks 0-9) and single indirect.
 * May clobber ufs_buf when reading the indirect block. */
static int ufs_block_map(ufs_priv_t *priv, const ufs_inode_t *inode,
                          uint32_t logical, uint32_t *phys_out)
{
    if (logical < UFS_DIRECT_BLOCKS) {
        *phys_out = inode->i_direct[logical];
        return 0;
    }

    uint32_t ind_idx = logical - UFS_DIRECT_BLOCKS;
    if (ind_idx >= UFS_BLOCK_SIZE / sizeof(uint32_t))
        return -EIO;  /* beyond single indirect range */

    if (inode->i_indirect == 0) {
        *phys_out = 0;
        return 0;
    }

    /* Read the relevant sector of the indirect block */
    uint32_t ptrs_per_sec = BLKDEV_SECTOR_SIZE / sizeof(uint32_t);  /* 128 */
    uint32_t sec = ind_idx / ptrs_per_sec;
    uint32_t off = ind_idx % ptrs_per_sec;

    int rc = ufs_read_blk_sector(priv, inode->i_indirect, sec);
    if (rc < 0) return rc;

    uint32_t *ptrs = (uint32_t *)ufs_buf;
    *phys_out = ptrs[off];
    return 0;
}

/* Set a logical block pointer in an inode.
 * For direct blocks (0-9): stores in i_direct[].
 * For indirect blocks (10+): allocates indirect block if needed,
 * then writes the pointer into the correct indirect block sector.
 * The caller must write the inode back after this call.
 * Clobbers ufs_buf. */
static int ufs_block_set(ufs_priv_t *priv, ufs_inode_t *inode,
                          uint32_t logical, uint32_t phys)
{
    if (logical < UFS_DIRECT_BLOCKS) {
        inode->i_direct[logical] = phys;
        return 0;
    }

    uint32_t ind_idx = logical - UFS_DIRECT_BLOCKS;
    if (ind_idx >= UFS_BLOCK_SIZE / sizeof(uint32_t))
        return -ENOSPC;

    /* Allocate indirect block on first use */
    if (inode->i_indirect == 0) {
        uint32_t ind_blk;
        int rc = ufs_alloc_block(priv, &ind_blk);
        if (rc < 0) return rc;
        rc = ufs_zero_block(priv, ind_blk);
        if (rc < 0) return rc;
        inode->i_indirect = ind_blk;
    }

    /* Read the relevant sector, set the pointer, write back */
    uint32_t ptrs_per_sec = BLKDEV_SECTOR_SIZE / sizeof(uint32_t);
    uint32_t sec = ind_idx / ptrs_per_sec;
    uint32_t off = ind_idx % ptrs_per_sec;

    int rc = ufs_read_blk_sector(priv, inode->i_indirect, sec);
    if (rc < 0) return rc;

    uint32_t *ptrs = (uint32_t *)ufs_buf;
    ptrs[off] = phys;
    return ufs_write_blk_sector(priv, inode->i_indirect, sec);
}

/* ── vnode from inode ─────────────────────────────────────────────────── */

static vnode_t *ufs_vnode_from_inode(mount_entry_t *mnt, uint32_t ino,
                                      const ufs_inode_t *inode)
{
    vnode_t *vn = vnode_alloc();
    if (!vn) return (vnode_t *)0;

    vn->ino     = ino;
    vn->size    = inode->i_size;
    vn->mount   = mnt;
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

static int ufs_mount(mount_entry_t *mnt, const void *dev_data)
{
    blkdev_t *dev = (blkdev_t *)dev_data;
    if (!dev) return -EINVAL;

    /* Read superblock (first sector of block 0) */
    int rc = dev->read(dev, ufs_buf, 0, 1);
    if (rc < 0) return rc;

    ufs_super_t *sb = (ufs_super_t *)ufs_buf;
    if (sb->s_magic != UFS_MAGIC) return -EINVAL;
    if (sb->s_block_size != UFS_BLOCK_SIZE) return -EINVAL;

    ufs_priv.dev          = dev;
    ufs_priv.block_count  = sb->s_block_count;
    ufs_priv.inode_count  = sb->s_inode_count;
    ufs_priv.itable_block = sb->s_itable_block;
    ufs_priv.data_block   = sb->s_data_block;
    ufs_priv.inode_blocks = sb->s_inode_blocks;
    ufs_priv.bmap_block   = sb->s_bmap_block;
    ufs_priv.imap_block   = sb->s_imap_block;
    ufs_priv.free_blocks  = sb->s_free_blocks;
    ufs_priv.free_inodes  = sb->s_free_inodes;

    /* Read root inode (inode 1) */
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

static int ufs_lookup(vnode_t *dir, const char *name, vnode_t **result)
{
    ufs_priv_t *priv = (ufs_priv_t *)dir->fs_priv;

    ufs_inode_t dir_inode;
    int rc = ufs_read_inode(priv, dir->ino, &dir_inode);
    if (rc < 0) return rc;

    uint32_t nblocks = (dir_inode.i_size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE;

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t phys;
        rc = ufs_block_map(priv, &dir_inode, b, &phys);
        if (rc < 0) return rc;
        if (phys == 0) continue;

        for (uint32_t s = 0; s < SECTORS_PER_BLOCK; s++) {
            rc = ufs_read_blk_sector(priv, phys, s);
            if (rc < 0) return rc;

            for (uint32_t d = 0; d < DIRENTS_PER_SECTOR; d++) {
                ufs_dirent_t *de =
                    (ufs_dirent_t *)&ufs_buf[d * UFS_DIRENT_SIZE];
                if (de->d_ino == 0) continue;

                /* Compare names */
                const char *a = de->d_name;
                const char *p = name;
                while (*a && *a == *p) { a++; p++; }
                if (*a != '\0' || *p != '\0') continue;

                /* Match — save ino before inode read clobbers ufs_buf */
                uint32_t child_ino = de->d_ino;
                ufs_inode_t child_inode;
                rc = ufs_read_inode(priv, child_ino, &child_inode);
                if (rc < 0) return rc;

                vnode_t *vn = ufs_vnode_from_inode(dir->mount, child_ino,
                                                    &child_inode);
                if (!vn) return -ENOMEM;

                *result = vn;
                return 0;
            }
        }
    }

    return -ENOENT;
}

/* ── ufs_read ─────────────────────────────────────────────────────────── */

static long ufs_read(vnode_t *vn, void *buf, size_t n, uint32_t off)
{
    if (vn->type == VNODE_DIR)
        return -(long)EISDIR;

    ufs_priv_t *priv = (ufs_priv_t *)vn->fs_priv;

    if (off >= vn->size) return 0;
    if (off + n > vn->size) n = vn->size - off;

    ufs_inode_t inode;
    int rc = ufs_read_inode(priv, vn->ino, &inode);
    if (rc < 0) return (long)rc;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t remaining = (uint32_t)n;
    uint32_t pos = off;

    while (remaining > 0) {
        uint32_t logical     = pos / UFS_BLOCK_SIZE;
        uint32_t off_in_blk  = pos % UFS_BLOCK_SIZE;
        uint32_t sec_in_blk  = off_in_blk / BLKDEV_SECTOR_SIZE;
        uint32_t off_in_sec  = off_in_blk % BLKDEV_SECTOR_SIZE;

        uint32_t phys;
        rc = ufs_block_map(priv, &inode, logical, &phys);
        if (rc < 0)
            return (n - remaining > 0) ? (long)(n - remaining) : (long)rc;
        if (phys == 0) break;  /* hole / unallocated block */

        rc = ufs_read_blk_sector(priv, phys, sec_in_blk);
        if (rc < 0)
            return (n - remaining > 0) ? (long)(n - remaining) : (long)rc;

        uint32_t avail = BLKDEV_SECTOR_SIZE - off_in_sec;
        if (avail > remaining) avail = remaining;

        __builtin_memcpy(dst, &ufs_buf[off_in_sec], avail);
        dst += avail;
        pos += avail;
        remaining -= avail;
    }

    return (long)(n - remaining);
}

/* ── ufs_write ─────────────────────────────────────────────────────────── */

static long ufs_write(vnode_t *vn, const void *buf, size_t n, uint32_t off)
{
    if (vn->type == VNODE_DIR)
        return -(long)EISDIR;

    ufs_priv_t *priv = (ufs_priv_t *)vn->fs_priv;

    ufs_inode_t inode;
    int rc = ufs_read_inode(priv, vn->ino, &inode);
    if (rc < 0) return (long)rc;

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t remaining = (uint32_t)n;
    uint32_t pos = off;

    while (remaining > 0) {
        uint32_t logical     = pos / UFS_BLOCK_SIZE;
        uint32_t off_in_blk  = pos % UFS_BLOCK_SIZE;
        uint32_t sec_in_blk  = off_in_blk / BLKDEV_SECTOR_SIZE;
        uint32_t off_in_sec  = off_in_blk % BLKDEV_SECTOR_SIZE;

        /* Ensure the logical block is allocated */
        uint32_t phys;
        rc = ufs_block_map(priv, &inode, logical, &phys);
        if (rc < 0)
            return (n - remaining > 0) ? (long)(n - remaining) : (long)rc;

        if (phys == 0) {
            /* Allocate a new data block */
            rc = ufs_alloc_block(priv, &phys);
            if (rc < 0)
                return (n - remaining > 0) ? (long)(n - remaining) : (long)rc;
            rc = ufs_zero_block(priv, phys);
            if (rc < 0)
                return (n - remaining > 0) ? (long)(n - remaining) : (long)rc;
            rc = ufs_block_set(priv, &inode, logical, phys);
            if (rc < 0)
                return (n - remaining > 0) ? (long)(n - remaining) : (long)rc;
        }

        uint32_t avail = BLKDEV_SECTOR_SIZE - off_in_sec;
        if (avail > remaining) avail = remaining;

        if (off_in_sec != 0 || avail < BLKDEV_SECTOR_SIZE) {
            /* Partial sector: read-modify-write */
            rc = ufs_read_blk_sector(priv, phys, sec_in_blk);
            if (rc < 0)
                return (n - remaining > 0) ? (long)(n - remaining) : (long)rc;
            __builtin_memcpy(&ufs_buf[off_in_sec], src, avail);
        } else {
            /* Full sector: write directly */
            __builtin_memcpy(ufs_buf, src, BLKDEV_SECTOR_SIZE);
        }

        rc = ufs_write_blk_sector(priv, phys, sec_in_blk);
        if (rc < 0)
            return (n - remaining > 0) ? (long)(n - remaining) : (long)rc;

        src += avail;
        pos += avail;
        remaining -= avail;
    }

    /* Update file size if extended */
    uint32_t end = off + (uint32_t)(n - remaining);
    if (end > inode.i_size) {
        inode.i_size = end;
        vn->size = end;
    }

    /* Write inode back and sync superblock */
    rc = ufs_write_inode(priv, vn->ino, &inode);
    if (rc < 0) return (long)rc;

    ufs_sync_super(priv);

    return (long)(n - remaining);
}

/* ── ufs_readdir ──────────────────────────────────────────────────────── */

static int ufs_readdir(vnode_t *dir, struct dirent *entries,
                        size_t max_entries, uint32_t *cookie)
{
    ufs_priv_t *priv = (ufs_priv_t *)dir->fs_priv;

    ufs_inode_t dir_inode;
    int rc = ufs_read_inode(priv, dir->ino, &dir_inode);
    if (rc < 0) return rc;

    uint32_t nblocks = (dir_inode.i_size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE;
    uint32_t entry_idx = 0;
    uint32_t target = *cookie;
    int count = 0;

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t phys;
        rc = ufs_block_map(priv, &dir_inode, b, &phys);
        if (rc < 0) return rc;
        if (phys == 0) continue;

        for (uint32_t s = 0; s < SECTORS_PER_BLOCK; s++) {
            rc = ufs_read_blk_sector(priv, phys, s);
            if (rc < 0) return rc;

            for (uint32_t d = 0; d < DIRENTS_PER_SECTOR; d++) {
                ufs_dirent_t *de =
                    (ufs_dirent_t *)&ufs_buf[d * UFS_DIRENT_SIZE];
                if (de->d_ino == 0) continue;

                /* Skip "." and ".." */
                if (de->d_name[0] == '.') {
                    if (de->d_name[1] == '\0')
                        { entry_idx++; continue; }
                    if (de->d_name[1] == '.' && de->d_name[2] == '\0')
                        { entry_idx++; continue; }
                }

                if (entry_idx >= target) {
                    if ((size_t)count >= max_entries)
                        goto done;

                    /* Save info before inode read clobbers ufs_buf */
                    uint32_t child_ino = de->d_ino;
                    entries[count].d_ino = child_ino;

                    /* Copy name (UFS_NAME_MAX ≤ VFS_NAME_MAX) */
                    int i = 0;
                    while (i < UFS_NAME_MAX && de->d_name[i]) {
                        entries[count].d_name[i] = de->d_name[i];
                        i++;
                    }
                    entries[count].d_name[i] = '\0';

                    /* Read child inode for d_type (clobbers ufs_buf) */
                    ufs_inode_t child;
                    rc = ufs_read_inode(priv, child_ino, &child);
                    if (rc == 0) {
                        if (S_ISDIR(child.i_mode))
                            entries[count].d_type = DT_DIR;
                        else if (S_ISLNK(child.i_mode))
                            entries[count].d_type = DT_LNK;
                        else
                            entries[count].d_type = DT_REG;
                    } else {
                        entries[count].d_type = DT_REG;  /* fallback */
                    }

                    count++;

                    /* Re-read directory sector (was clobbered) */
                    rc = ufs_read_blk_sector(priv, phys, s);
                    if (rc < 0) goto done;
                }
                entry_idx++;
            }
        }
    }

done:
    *cookie = target + (uint32_t)count;
    return count;
}

/* ── ufs_dir_add_entry (helper for create/mkdir) ─────────────────────── */

/* Add a directory entry (name → child_ino) into dir.
 * Searches for a free slot (d_ino == 0) in existing blocks; if none
 * found, extends the directory by one block.  Clobbers ufs_buf. */
static int ufs_dir_add_entry(ufs_priv_t *priv, vnode_t *dir,
                              const char *name, uint32_t child_ino)
{
    ufs_inode_t dir_inode;
    int rc = ufs_read_inode(priv, dir->ino, &dir_inode);
    if (rc < 0) return rc;

    uint32_t nblocks = (dir_inode.i_size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE;

    /* Scan existing directory blocks for a free slot */
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t phys;
        rc = ufs_block_map(priv, &dir_inode, b, &phys);
        if (rc < 0) return rc;
        if (phys == 0) continue;

        for (uint32_t s = 0; s < SECTORS_PER_BLOCK; s++) {
            rc = ufs_read_blk_sector(priv, phys, s);
            if (rc < 0) return rc;

            for (uint32_t d = 0; d < DIRENTS_PER_SECTOR; d++) {
                ufs_dirent_t *de =
                    (ufs_dirent_t *)&ufs_buf[d * UFS_DIRENT_SIZE];
                if (de->d_ino != 0) continue;

                /* Found free slot — fill it in */
                de->d_ino = child_ino;
                int i = 0;
                while (name[i] && i < UFS_NAME_MAX) {
                    de->d_name[i] = name[i];
                    i++;
                }
                while (i <= UFS_NAME_MAX)
                    de->d_name[i++] = '\0';

                return ufs_write_blk_sector(priv, phys, s);
            }
        }
    }

    /* No free slot — extend directory by one block */
    uint32_t new_blk;
    rc = ufs_alloc_block(priv, &new_blk);
    if (rc < 0) return rc;

    rc = ufs_zero_block(priv, new_blk);
    if (rc < 0) return rc;

    /* Write entry into first sector of new block */
    __builtin_memset(ufs_buf, 0, BLKDEV_SECTOR_SIZE);
    ufs_dirent_t *de = (ufs_dirent_t *)ufs_buf;
    de->d_ino = child_ino;
    {
        int i = 0;
        while (name[i] && i < UFS_NAME_MAX) {
            de->d_name[i] = name[i];
            i++;
        }
        while (i <= UFS_NAME_MAX)
            de->d_name[i++] = '\0';
    }
    rc = ufs_write_blk_sector(priv, new_blk, 0);
    if (rc < 0) return rc;

    /* Link new block into directory inode and update size */
    rc = ufs_block_set(priv, &dir_inode, nblocks, new_blk);
    if (rc < 0) return rc;

    dir_inode.i_size += UFS_BLOCK_SIZE;
    rc = ufs_write_inode(priv, dir->ino, &dir_inode);
    if (rc < 0) return rc;

    dir->size = dir_inode.i_size;
    return 0;
}

/* ── ufs_create ──────────────────────────────────────────────────────── */

static int ufs_create(vnode_t *dir, const char *name, uint32_t mode,
                       vnode_t **result)
{
    ufs_priv_t *priv = (ufs_priv_t *)dir->fs_priv;

    /* Allocate a new inode */
    uint32_t new_ino;
    int rc = ufs_alloc_inode(priv, &new_ino);
    if (rc < 0) return rc;

    /* Initialize the inode */
    ufs_inode_t inode;
    __builtin_memset(&inode, 0, sizeof(inode));
    inode.i_mode  = (uint16_t)(S_IFREG | (mode & 0777u));
    inode.i_nlink = 1;

    rc = ufs_write_inode(priv, new_ino, &inode);
    if (rc < 0) {
        ufs_free_inode(priv, new_ino);
        return rc;
    }

    /* Add entry to parent directory */
    rc = ufs_dir_add_entry(priv, dir, name, new_ino);
    if (rc < 0) {
        ufs_free_inode(priv, new_ino);
        return rc;
    }

    ufs_sync_super(priv);

    /* Allocate and return vnode */
    vnode_t *vn = ufs_vnode_from_inode(dir->mount, new_ino, &inode);
    if (!vn) return -ENOMEM;

    *result = vn;
    return 0;
}

/* ── ufs_dir_remove_entry ─────────────────────────────────────────────── */

/* Remove a directory entry by name.  Sets *removed_ino to the inode
 * number of the removed entry.  Clobbers ufs_buf. */
static int ufs_dir_remove_entry(ufs_priv_t *priv, vnode_t *dir,
                                 const char *name, uint32_t *removed_ino)
{
    ufs_inode_t dir_inode;
    int rc = ufs_read_inode(priv, dir->ino, &dir_inode);
    if (rc < 0) return rc;

    uint32_t nblocks = (dir_inode.i_size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE;

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t phys;
        rc = ufs_block_map(priv, &dir_inode, b, &phys);
        if (rc < 0) return rc;
        if (phys == 0) continue;

        for (uint32_t s = 0; s < SECTORS_PER_BLOCK; s++) {
            rc = ufs_read_blk_sector(priv, phys, s);
            if (rc < 0) return rc;

            for (uint32_t d = 0; d < DIRENTS_PER_SECTOR; d++) {
                ufs_dirent_t *de =
                    (ufs_dirent_t *)&ufs_buf[d * UFS_DIRENT_SIZE];
                if (de->d_ino == 0) continue;

                const char *a = de->d_name;
                const char *p = name;
                while (*a && *a == *p) { a++; p++; }
                if (*a != '\0' || *p != '\0') continue;

                /* Match — remove entry */
                *removed_ino = de->d_ino;
                de->d_ino = 0;
                return ufs_write_blk_sector(priv, phys, s);
            }
        }
    }
    return -ENOENT;
}

/* ── ufs_dir_is_empty ────────────────────────────────────────────────── */

/* Check if a directory has no entries beyond "." and "..".
 * Returns 1 if empty, 0 if non-empty, negative on error. */
static int ufs_dir_is_empty(ufs_priv_t *priv, uint32_t dir_ino)
{
    ufs_inode_t dir_inode;
    int rc = ufs_read_inode(priv, dir_ino, &dir_inode);
    if (rc < 0) return rc;

    uint32_t nblocks = (dir_inode.i_size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE;

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t phys;
        rc = ufs_block_map(priv, &dir_inode, b, &phys);
        if (rc < 0) return rc;
        if (phys == 0) continue;

        for (uint32_t s = 0; s < SECTORS_PER_BLOCK; s++) {
            rc = ufs_read_blk_sector(priv, phys, s);
            if (rc < 0) return rc;

            for (uint32_t d = 0; d < DIRENTS_PER_SECTOR; d++) {
                ufs_dirent_t *de =
                    (ufs_dirent_t *)&ufs_buf[d * UFS_DIRENT_SIZE];
                if (de->d_ino == 0) continue;

                /* Skip "." and ".." */
                if (de->d_name[0] == '.') {
                    if (de->d_name[1] == '\0') continue;
                    if (de->d_name[1] == '.' && de->d_name[2] == '\0')
                        continue;
                }
                return 0;  /* non-empty */
            }
        }
    }
    return 1;  /* empty */
}

/* ── ufs_mkdir ───────────────────────────────────────────────────────── */

static int ufs_mkdir(vnode_t *dir, const char *name, uint32_t mode)
{
    ufs_priv_t *priv = (ufs_priv_t *)dir->fs_priv;

    /* Allocate inode for new directory */
    uint32_t new_ino;
    int rc = ufs_alloc_inode(priv, &new_ino);
    if (rc < 0) return rc;

    /* Allocate data block for "." and ".." entries */
    uint32_t data_blk;
    rc = ufs_alloc_block(priv, &data_blk);
    if (rc < 0) {
        ufs_free_inode(priv, new_ino);
        return rc;
    }

    rc = ufs_zero_block(priv, data_blk);
    if (rc < 0) {
        ufs_free_block(priv, data_blk);
        ufs_free_inode(priv, new_ino);
        return rc;
    }

    /* Write "." and ".." entries into first sector of new block */
    __builtin_memset(ufs_buf, 0, BLKDEV_SECTOR_SIZE);
    ufs_dirent_t *dot = (ufs_dirent_t *)&ufs_buf[0];
    dot->d_ino = new_ino;
    dot->d_name[0] = '.';
    dot->d_name[1] = '\0';

    ufs_dirent_t *dotdot = (ufs_dirent_t *)&ufs_buf[UFS_DIRENT_SIZE];
    dotdot->d_ino = dir->ino;
    dotdot->d_name[0] = '.';
    dotdot->d_name[1] = '.';
    dotdot->d_name[2] = '\0';

    rc = ufs_write_blk_sector(priv, data_blk, 0);
    if (rc < 0) {
        ufs_free_block(priv, data_blk);
        ufs_free_inode(priv, new_ino);
        return rc;
    }

    /* Initialize and write the new directory inode */
    ufs_inode_t inode;
    __builtin_memset(&inode, 0, sizeof(inode));
    inode.i_mode      = (uint16_t)(S_IFDIR | (mode & 0777u));
    inode.i_nlink     = 2;  /* "." from self + entry from parent */
    inode.i_size      = UFS_BLOCK_SIZE;
    inode.i_direct[0] = data_blk;

    rc = ufs_write_inode(priv, new_ino, &inode);
    if (rc < 0) return rc;

    /* Add entry in parent directory */
    rc = ufs_dir_add_entry(priv, dir, name, new_ino);
    if (rc < 0) return rc;

    /* Increment parent's nlink (for ".." backlink) */
    ufs_inode_t parent_inode;
    rc = ufs_read_inode(priv, dir->ino, &parent_inode);
    if (rc < 0) return rc;
    parent_inode.i_nlink++;
    rc = ufs_write_inode(priv, dir->ino, &parent_inode);
    if (rc < 0) return rc;

    ufs_sync_super(priv);
    return 0;
}

/* ── ufs_truncate ────────────────────────────────────────────────────── */

static int ufs_truncate(vnode_t *vn, uint32_t length)
{
    if (vn->type == VNODE_DIR)
        return -EISDIR;

    ufs_priv_t *priv = (ufs_priv_t *)vn->fs_priv;

    ufs_inode_t inode;
    int rc = ufs_read_inode(priv, vn->ino, &inode);
    if (rc < 0) return rc;

    if (length >= inode.i_size) {
        /* Extend (or no change) — just update size */
        inode.i_size = length;
    } else if (length == 0) {
        /* Truncate to zero — free all blocks */
        for (int i = 0; i < UFS_DIRECT_BLOCKS; i++) {
            if (inode.i_direct[i] != 0) {
                ufs_free_block(priv, inode.i_direct[i]);
                inode.i_direct[i] = 0;
            }
        }

        if (inode.i_indirect != 0) {
            /* Free blocks pointed to by the indirect block.
             * Process one sector at a time; save pointers locally
             * since ufs_free_block clobbers ufs_buf. */
            for (uint32_t s = 0; s < SECTORS_PER_BLOCK; s++) {
                rc = ufs_read_blk_sector(priv, inode.i_indirect, s);
                if (rc < 0) break;

                /* Save pointers from this sector */
                uint32_t saved[BLKDEV_SECTOR_SIZE / sizeof(uint32_t)];
                __builtin_memcpy(saved, ufs_buf, BLKDEV_SECTOR_SIZE);

                for (uint32_t j = 0;
                     j < BLKDEV_SECTOR_SIZE / sizeof(uint32_t); j++) {
                    if (saved[j] != 0)
                        ufs_free_block(priv, saved[j]);
                }
            }
            ufs_free_block(priv, inode.i_indirect);
            inode.i_indirect = 0;
        }
        inode.i_size = 0;
    } else {
        /* General shrink: free blocks beyond the new length */
        uint32_t keep_blocks = (length + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE;

        /* Free direct blocks beyond keep_blocks */
        for (uint32_t i = keep_blocks; i < UFS_DIRECT_BLOCKS; i++) {
            if (inode.i_direct[i] != 0) {
                ufs_free_block(priv, inode.i_direct[i]);
                inode.i_direct[i] = 0;
            }
        }

        /* Free indirect entries beyond keep_blocks */
        if (inode.i_indirect != 0 && keep_blocks <= UFS_DIRECT_BLOCKS) {
            /* All indirect blocks should be freed */
            for (uint32_t s = 0; s < SECTORS_PER_BLOCK; s++) {
                rc = ufs_read_blk_sector(priv, inode.i_indirect, s);
                if (rc < 0) break;
                uint32_t saved[BLKDEV_SECTOR_SIZE / sizeof(uint32_t)];
                __builtin_memcpy(saved, ufs_buf, BLKDEV_SECTOR_SIZE);
                for (uint32_t j = 0;
                     j < BLKDEV_SECTOR_SIZE / sizeof(uint32_t); j++) {
                    if (saved[j] != 0)
                        ufs_free_block(priv, saved[j]);
                }
            }
            ufs_free_block(priv, inode.i_indirect);
            inode.i_indirect = 0;
        } else if (inode.i_indirect != 0 && keep_blocks > UFS_DIRECT_BLOCKS) {
            /* Free only indirect entries beyond keep_blocks */
            uint32_t first_free_ind = keep_blocks - UFS_DIRECT_BLOCKS;
            uint32_t ptrs_per_sec = BLKDEV_SECTOR_SIZE / sizeof(uint32_t);
            for (uint32_t s = first_free_ind / ptrs_per_sec;
                 s < SECTORS_PER_BLOCK; s++) {
                rc = ufs_read_blk_sector(priv, inode.i_indirect, s);
                if (rc < 0) break;
                uint32_t saved[BLKDEV_SECTOR_SIZE / sizeof(uint32_t)];
                __builtin_memcpy(saved, ufs_buf, BLKDEV_SECTOR_SIZE);
                uint32_t start = (s == first_free_ind / ptrs_per_sec)
                               ? first_free_ind % ptrs_per_sec : 0;
                for (uint32_t j = start; j < ptrs_per_sec; j++) {
                    if (saved[j] != 0)
                        ufs_free_block(priv, saved[j]);
                }
            }
        }
        inode.i_size = length;
    }

    vn->size = inode.i_size;
    rc = ufs_write_inode(priv, vn->ino, &inode);
    if (rc < 0) return rc;

    ufs_sync_super(priv);
    return 0;
}

/* ── ufs_unlink ──────────────────────────────────────────────────────── */

static int ufs_unlink(vnode_t *dir, const char *name)
{
    ufs_priv_t *priv = (ufs_priv_t *)dir->fs_priv;

    /* Remove directory entry and get the child inode number */
    uint32_t child_ino;
    int rc = ufs_dir_remove_entry(priv, dir, name, &child_ino);
    if (rc < 0) return rc;

    /* Read child inode */
    ufs_inode_t child;
    rc = ufs_read_inode(priv, child_ino, &child);
    if (rc < 0) return rc;

    /* If directory: check it's empty, decrement parent nlink */
    if (S_ISDIR(child.i_mode)) {
        int empty = ufs_dir_is_empty(priv, child_ino);
        if (empty <= 0) {
            /* Not empty or error — re-add the entry (best effort) */
            ufs_dir_add_entry(priv, dir, name, child_ino);
            return (empty == 0) ? -ENOTEMPTY : empty;
        }

        /* Decrement parent's nlink (removing ".." backlink) */
        ufs_inode_t parent;
        rc = ufs_read_inode(priv, dir->ino, &parent);
        if (rc == 0 && parent.i_nlink > 0) {
            parent.i_nlink--;
            ufs_write_inode(priv, dir->ino, &parent);
        }
    }

    /* Decrement link count */
    if (child.i_nlink > 0)
        child.i_nlink--;

    if (child.i_nlink == 0) {
        /* Free all data blocks */
        for (int i = 0; i < UFS_DIRECT_BLOCKS; i++) {
            if (child.i_direct[i] != 0) {
                ufs_free_block(priv, child.i_direct[i]);
                child.i_direct[i] = 0;
            }
        }

        if (child.i_indirect != 0) {
            for (uint32_t s = 0; s < SECTORS_PER_BLOCK; s++) {
                rc = ufs_read_blk_sector(priv, child.i_indirect, s);
                if (rc < 0) break;
                uint32_t saved[BLKDEV_SECTOR_SIZE / sizeof(uint32_t)];
                __builtin_memcpy(saved, ufs_buf, BLKDEV_SECTOR_SIZE);
                for (uint32_t j = 0;
                     j < BLKDEV_SECTOR_SIZE / sizeof(uint32_t); j++) {
                    if (saved[j] != 0)
                        ufs_free_block(priv, saved[j]);
                }
            }
            ufs_free_block(priv, child.i_indirect);
            child.i_indirect = 0;
        }

        child.i_size = 0;
        ufs_write_inode(priv, child_ino, &child);
        ufs_free_inode(priv, child_ino);
    } else {
        ufs_write_inode(priv, child_ino, &child);
    }

    ufs_sync_super(priv);
    return 0;
}

/* ── ufs_stat ─────────────────────────────────────────────────────────── */

static int ufs_stat(vnode_t *vn, struct stat *st)
{
    ufs_priv_t *priv = (ufs_priv_t *)vn->fs_priv;

    ufs_inode_t inode;
    int rc = ufs_read_inode(priv, vn->ino, &inode);
    if (rc < 0) return rc;

    st->st_ino   = vn->ino;
    st->st_mode  = inode.i_mode;
    st->st_nlink = inode.i_nlink;
    st->st_size  = inode.i_size;
    return 0;
}

/* ── ufs_readlink ─────────────────────────────────────────────────────── */

static long ufs_readlink(vnode_t *vn, char *buf, size_t bufsiz)
{
    if (vn->type != VNODE_SYMLINK)
        return -(long)EINVAL;

    ufs_priv_t *priv = (ufs_priv_t *)vn->fs_priv;

    ufs_inode_t inode;
    int rc = ufs_read_inode(priv, vn->ino, &inode);
    if (rc < 0) return (long)rc;

    uint32_t len = inode.i_size;
    if (len > bufsiz) len = (uint32_t)bufsiz;

    if (inode.i_size <= UFS_FAST_SYMLINK_MAX) {
        /* Fast symlink: data stored inline in i_direct[] */
        __builtin_memcpy(buf, inode.i_direct, len);
        return (long)len;
    }

    /* Regular symlink: data in first data block */
    if (inode.i_direct[0] == 0) return -EIO;

    rc = ufs_read_blk_sector(priv, inode.i_direct[0], 0);
    if (rc < 0) return (long)rc;

    if (len > BLKDEV_SECTOR_SIZE) len = BLKDEV_SECTOR_SIZE;
    __builtin_memcpy(buf, ufs_buf, len);
    return (long)len;
}

/* ── ufs_statfs ──────────────────────────────────────────────────────── */

static int ufs_statfs(mount_entry_t *mnt, struct kernel_statfs *buf)
{
    ufs_priv_t *priv = (ufs_priv_t *)mnt->sb_priv;
    if (!priv)
        return -EINVAL;

    __builtin_memset(buf, 0, sizeof(*buf));

    buf->f_type    = UFS_MAGIC;
    buf->f_bsize   = UFS_BLOCK_SIZE;
    buf->f_frsize  = UFS_BLOCK_SIZE;
    buf->f_blocks  = priv->block_count;
    buf->f_bfree   = priv->free_blocks;
    buf->f_bavail  = priv->free_blocks;
    buf->f_files   = priv->inode_count;
    buf->f_ffree   = priv->free_inodes;
    buf->f_namelen = VFS_NAME_MAX;
    return 0;
}

/* ── Operations table ─────────────────────────────────────────────────── */

const vfs_ops_t ufs_ops = {
    .mount    = ufs_mount,
    .lookup   = ufs_lookup,
    .read     = ufs_read,
    .write    = ufs_write,
    .readdir  = ufs_readdir,
    .stat     = ufs_stat,
    .readlink = ufs_readlink,
    .create   = ufs_create,
    .mkdir    = ufs_mkdir,
    .unlink   = ufs_unlink,
    .truncate = ufs_truncate,
    .statfs   = ufs_statfs,
};
