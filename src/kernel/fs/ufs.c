/*
 * ufs.c — UFS filesystem driver
 *
 * Implements vfs_ops_t for mounting UFS filesystem images via the block
 * device layer.  The driver reads and writes the on-disk superblock,
 * inode table, bitmaps, and data blocks through sector-level blkdev I/O.
 *
 * Step 6: read-only mount/lookup/read/readdir/stat/readlink
 * Step 7: block/inode allocation, inode write-back, superblock sync
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

/* ── Operations table ─────────────────────────────────────────────────── */

const vfs_ops_t ufs_ops = {
    .mount    = ufs_mount,
    .lookup   = ufs_lookup,
    .read     = ufs_read,
    .write    = NULL,      /* read-only (Phase 5 Step 8) */
    .readdir  = ufs_readdir,
    .stat     = ufs_stat,
    .readlink = ufs_readlink,
    .create   = NULL,      /* read-only */
    .mkdir    = NULL,      /* read-only */
    .unlink   = NULL,      /* read-only */
    .truncate = NULL,      /* read-only */
};
