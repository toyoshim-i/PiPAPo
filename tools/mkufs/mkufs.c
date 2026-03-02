/*
 * mkufs.c — Create a formatted UFS image file
 *
 * Host tool that generates an empty UFS filesystem image suitable for
 * use with the PPAP loopback block device.  Optionally populates the
 * image from a host directory tree.
 *
 * Usage:
 *   mkufs [-s SIZE] [-v] [-p DIR] <output_file>
 *
 *   -s SIZE   Image size (e.g., 64K, 1M, 64M).  Default: 64K.
 *   -p DIR    Populate from host directory tree.
 *   -v        Verbose: print layout summary.
 *
 * Build:   cc -O2 -o mkufs mkufs.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── UFS format constants (must match ufs_format.h) ─────────────────── */

#define UFS_MAGIC           0x55465331u
#define UFS_BLOCK_SIZE      4096u
#define UFS_INODE_SIZE        64u
#define UFS_INODES_PER_BLOCK (UFS_BLOCK_SIZE / UFS_INODE_SIZE)
#define UFS_DIRECT_BLOCKS     10
#define UFS_DIRENT_SIZE       32u
#define UFS_DIRENTS_PER_BLOCK (UFS_BLOCK_SIZE / UFS_DIRENT_SIZE)
#define UFS_NAME_MAX          27
#define UFS_ROOT_INO           1

typedef struct {
    uint32_t s_magic;
    uint32_t s_block_size;
    uint32_t s_block_count;
    uint32_t s_inode_count;
    uint32_t s_free_blocks;
    uint32_t s_free_inodes;
    uint32_t s_bmap_block;
    uint32_t s_imap_block;
    uint32_t s_itable_block;
    uint32_t s_data_block;
    uint32_t s_inode_blocks;
    uint8_t  s_pad[84];
} ufs_super_t;

typedef struct {
    uint16_t i_mode;
    uint16_t i_nlink;
    uint16_t i_uid;
    uint16_t i_gid;
    uint32_t i_size;
    uint32_t i_mtime;
    uint32_t i_ctime;
    uint32_t i_direct[UFS_DIRECT_BLOCKS];
    uint32_t i_indirect;
} ufs_inode_t;

typedef struct {
    uint32_t d_ino;
    char     d_name[UFS_NAME_MAX + 1];
} ufs_dirent_t;

/* ── POSIX file mode constants ───────────────────────────────────────── */

#define S_IFMT_  0170000u
#define S_IFDIR_ 0040000u
#define S_IFREG_ 0100000u
#define S_IFLNK_ 0120000u

/* ── Image buffer ────────────────────────────────────────────────────── */

static uint8_t *img;
static uint32_t img_size;
static uint32_t block_count;
static uint32_t inode_count;
static uint32_t inode_blocks;
static uint32_t data_start;

/* Bitmap state */
static uint32_t next_free_block;
static uint32_t next_free_inode;
static uint32_t free_blocks_count;
static uint32_t free_inodes_count;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static uint8_t *block_ptr(uint32_t blk)
{
    return &img[blk * UFS_BLOCK_SIZE];
}

/* ── Bitmap operations ───────────────────────────────────────────────── */

static void bmap_set(uint32_t bmap_block, uint32_t bit)
{
    uint8_t *bmap = block_ptr(bmap_block);
    bmap[bit / 8] |= (1u << (bit % 8));
}

static int bmap_test(uint32_t bmap_block, uint32_t bit)
{
    uint8_t *bmap = block_ptr(bmap_block);
    return (bmap[bit / 8] >> (bit % 8)) & 1;
}

static uint32_t alloc_block(void)
{
    while (next_free_block < block_count) {
        if (!bmap_test(1, next_free_block)) {
            uint32_t b = next_free_block++;
            bmap_set(1, b);
            free_blocks_count--;
            return b;
        }
        next_free_block++;
    }
    fprintf(stderr, "mkufs: out of blocks\n");
    exit(1);
}

static uint32_t alloc_inode(void)
{
    while (next_free_inode < inode_count) {
        if (!bmap_test(2, next_free_inode)) {
            uint32_t i = next_free_inode++;
            bmap_set(2, i);
            free_inodes_count--;
            return i;
        }
        next_free_inode++;
    }
    fprintf(stderr, "mkufs: out of inodes\n");
    exit(1);
}

/* ── Inode read/write ────────────────────────────────────────────────── */

static void write_inode(uint32_t ino, const ufs_inode_t *inode)
{
    uint32_t blk = 3 + ino / UFS_INODES_PER_BLOCK;
    uint32_t off = (ino % UFS_INODES_PER_BLOCK) * UFS_INODE_SIZE;
    memcpy(block_ptr(blk) + off, inode, sizeof(ufs_inode_t));
}

/* ── Directory entry helpers ─────────────────────────────────────────── */

static void add_dirent(ufs_inode_t *dir_inode, uint32_t dir_ino,
                       uint32_t file_ino, const char *name)
{
    /* Find a free slot in existing directory blocks */
    uint32_t nblocks = (dir_inode->i_size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE;
    for (uint32_t b = 0; b < nblocks && b < UFS_DIRECT_BLOCKS; b++) {
        uint32_t pblk = dir_inode->i_direct[b];
        if (pblk == 0) continue;
        ufs_dirent_t *entries = (ufs_dirent_t *)block_ptr(pblk);
        for (uint32_t i = 0; i < UFS_DIRENTS_PER_BLOCK; i++) {
            if (entries[i].d_ino == 0) {
                entries[i].d_ino = file_ino;
                strncpy(entries[i].d_name, name, UFS_NAME_MAX);
                entries[i].d_name[UFS_NAME_MAX] = '\0';
                return;
            }
        }
    }

    /* Need a new directory block */
    if (nblocks >= UFS_DIRECT_BLOCKS) {
        fprintf(stderr, "mkufs: directory too large (> %d blocks)\n",
                UFS_DIRECT_BLOCKS);
        exit(1);
    }
    uint32_t new_blk = alloc_block();
    memset(block_ptr(new_blk), 0, UFS_BLOCK_SIZE);
    dir_inode->i_direct[nblocks] = new_blk;
    dir_inode->i_size = (nblocks + 1) * UFS_BLOCK_SIZE;

    ufs_dirent_t *entries = (ufs_dirent_t *)block_ptr(new_blk);
    entries[0].d_ino = file_ino;
    strncpy(entries[0].d_name, name, UFS_NAME_MAX);
    entries[0].d_name[UFS_NAME_MAX] = '\0';

    /* Update inode on disk */
    write_inode(dir_ino, dir_inode);
}

/* ── File data writing ───────────────────────────────────────────────── */

static void write_file_data(ufs_inode_t *inode, const void *data,
                            uint32_t size)
{
    const uint8_t *src = (const uint8_t *)data;
    uint32_t nblocks = (size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE;
    uint32_t remaining = size;

    for (uint32_t i = 0; i < nblocks; i++) {
        uint32_t blk = alloc_block();
        uint32_t chunk = remaining > UFS_BLOCK_SIZE ? UFS_BLOCK_SIZE : remaining;

        memset(block_ptr(blk), 0, UFS_BLOCK_SIZE);
        memcpy(block_ptr(blk), src, chunk);
        src += chunk;
        remaining -= chunk;

        if (i < UFS_DIRECT_BLOCKS) {
            inode->i_direct[i] = blk;
        } else {
            /* Indirect block */
            if (inode->i_indirect == 0) {
                inode->i_indirect = alloc_block();
                memset(block_ptr(inode->i_indirect), 0, UFS_BLOCK_SIZE);
            }
            uint32_t *ind = (uint32_t *)block_ptr(inode->i_indirect);
            ind[i - UFS_DIRECT_BLOCKS] = blk;
        }
    }
    inode->i_size = size;
}

/* ── Populate from host directory ────────────────────────────────────── */

static void populate_dir(const char *host_path, uint32_t dir_ino,
                         ufs_inode_t *dir_inode);

static void populate_entry(const char *host_path, const char *name,
                           uint32_t parent_ino, ufs_inode_t *parent_inode)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", host_path, name);

    struct stat st;
    if (lstat(path, &st) < 0) {
        perror(path);
        return;
    }

    /* Truncate name if needed */
    char short_name[UFS_NAME_MAX + 1];
    strncpy(short_name, name, UFS_NAME_MAX);
    short_name[UFS_NAME_MAX] = '\0';

    if (S_ISREG(st.st_mode)) {
        uint32_t ino = alloc_inode();
        ufs_inode_t inode;
        memset(&inode, 0, sizeof(inode));
        inode.i_mode = S_IFREG_ | (st.st_mode & 0777);
        inode.i_nlink = 1;
        inode.i_size = 0;

        /* Read file data */
        if (st.st_size > 0) {
            FILE *fp = fopen(path, "rb");
            if (fp) {
                uint8_t *data = malloc(st.st_size);
                if (data) {
                    size_t n = fread(data, 1, st.st_size, fp);
                    write_file_data(&inode, data, (uint32_t)n);
                    free(data);
                }
                fclose(fp);
            }
        }

        write_inode(ino, &inode);
        add_dirent(parent_inode, parent_ino, ino, short_name);
        write_inode(parent_ino, parent_inode);

    } else if (S_ISDIR(st.st_mode)) {
        uint32_t ino = alloc_inode();
        ufs_inode_t inode;
        memset(&inode, 0, sizeof(inode));
        inode.i_mode = S_IFDIR_ | (st.st_mode & 0777);
        inode.i_nlink = 2;

        /* Create directory data block with "." and ".." */
        uint32_t dir_blk = alloc_block();
        memset(block_ptr(dir_blk), 0, UFS_BLOCK_SIZE);
        inode.i_direct[0] = dir_blk;
        inode.i_size = UFS_BLOCK_SIZE;

        ufs_dirent_t *entries = (ufs_dirent_t *)block_ptr(dir_blk);
        entries[0].d_ino = ino;
        strcpy(entries[0].d_name, ".");
        entries[1].d_ino = parent_ino;
        strcpy(entries[1].d_name, "..");

        write_inode(ino, &inode);
        add_dirent(parent_inode, parent_ino, ino, short_name);
        write_inode(parent_ino, parent_inode);

        /* Increment parent nlink for ".." */
        parent_inode->i_nlink++;
        write_inode(parent_ino, parent_inode);

        /* Recurse */
        populate_dir(path, ino, &inode);

    } else if (S_ISLNK(st.st_mode)) {
        char target[256];
        ssize_t len = readlink(path, target, sizeof(target) - 1);
        if (len < 0) { perror(path); return; }
        target[len] = '\0';

        uint32_t ino = alloc_inode();
        ufs_inode_t inode;
        memset(&inode, 0, sizeof(inode));
        inode.i_mode = S_IFLNK_ | 0777;
        inode.i_nlink = 1;
        inode.i_size = (uint32_t)len;

        if ((uint32_t)len <= UFS_DIRECT_BLOCKS * sizeof(uint32_t)) {
            /* Fast symlink: store inline in i_direct */
            memcpy(inode.i_direct, target, len);
        } else {
            /* Regular symlink: store in data block */
            write_file_data(&inode, target, (uint32_t)len);
        }

        write_inode(ino, &inode);
        add_dirent(parent_inode, parent_ino, ino, short_name);
        write_inode(parent_ino, parent_inode);
    }
    /* Skip other file types (devices, sockets, etc.) */
}

static void populate_dir(const char *host_path, uint32_t dir_ino,
                         ufs_inode_t *dir_inode)
{
    DIR *dp = opendir(host_path);
    if (!dp) { perror(host_path); return; }

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        populate_entry(host_path, de->d_name, dir_ino, dir_inode);
    }
    closedir(dp);
}

/* ── Size parsing ────────────────────────────────────────────────────── */

static uint32_t parse_size(const char *s)
{
    char *end;
    unsigned long val = strtoul(s, &end, 0);
    switch (*end) {
    case 'k': case 'K': val *= 1024; break;
    case 'm': case 'M': val *= 1024 * 1024; break;
    case 'g': case 'G': val *= 1024 * 1024 * 1024; break;
    }
    return (uint32_t)val;
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    uint32_t size = 64 * 1024;  /* default 64K */
    int verbose = 0;
    const char *populate_dir_path = NULL;
    const char *output = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            size = parse_size(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            populate_dir_path = argv[++i];
        } else if (argv[i][0] != '-') {
            output = argv[i];
        } else {
            fprintf(stderr, "Usage: %s [-s SIZE] [-v] [-p DIR] <output>\n",
                    argv[0]);
            return 1;
        }
    }

    if (!output) {
        fprintf(stderr, "Usage: %s [-s SIZE] [-v] [-p DIR] <output>\n",
                argv[0]);
        return 1;
    }

    /* Ensure size is block-aligned and reasonable */
    if (size < UFS_BLOCK_SIZE * 8) {
        fprintf(stderr, "mkufs: image size must be at least %u bytes\n",
                UFS_BLOCK_SIZE * 8);
        return 1;
    }
    size &= ~(UFS_BLOCK_SIZE - 1);
    img_size = size;

    /* Allocate image buffer */
    img = calloc(1, img_size);
    if (!img) { perror("calloc"); return 1; }

    /* Compute layout */
    block_count = img_size / UFS_BLOCK_SIZE;
    /* 1 inode per 4 blocks (16 KB), minimum 64 */
    inode_count = block_count / 4;
    if (inode_count < 64) inode_count = 64;
    if (inode_count > UFS_BLOCK_SIZE * 8) inode_count = UFS_BLOCK_SIZE * 8;

    inode_blocks = (inode_count + UFS_INODES_PER_BLOCK - 1) / UFS_INODES_PER_BLOCK;
    data_start = 3 + inode_blocks;  /* super + bmap + imap + itable */

    if (data_start >= block_count) {
        fprintf(stderr, "mkufs: image too small for metadata\n");
        return 1;
    }

    /* Initialize free counters */
    free_blocks_count = block_count;
    free_inodes_count = inode_count;
    next_free_block = 0;
    next_free_inode = 0;

    /* Mark metadata blocks as used in block bitmap */
    for (uint32_t b = 0; b < data_start; b++)
        bmap_set(1, b);
    free_blocks_count -= data_start;
    next_free_block = data_start;

    /* Mark inode 0 as used (reserved) */
    bmap_set(2, 0);
    free_inodes_count--;
    next_free_inode = 1;

    /* Create root directory (inode 1) */
    uint32_t root_ino = alloc_inode();  /* should be 1 */
    ufs_inode_t root_inode;
    memset(&root_inode, 0, sizeof(root_inode));
    root_inode.i_mode = S_IFDIR_ | 0755;
    root_inode.i_nlink = 2;  /* "." and parent (self for root) */

    /* Root directory data block */
    uint32_t root_blk = alloc_block();
    memset(block_ptr(root_blk), 0, UFS_BLOCK_SIZE);
    root_inode.i_direct[0] = root_blk;
    root_inode.i_size = UFS_BLOCK_SIZE;

    ufs_dirent_t *root_entries = (ufs_dirent_t *)block_ptr(root_blk);
    root_entries[0].d_ino = root_ino;
    strcpy(root_entries[0].d_name, ".");
    root_entries[1].d_ino = root_ino;
    strcpy(root_entries[1].d_name, "..");

    write_inode(root_ino, &root_inode);

    /* Populate from host directory if specified */
    if (populate_dir_path) {
        populate_dir(populate_dir_path, root_ino, &root_inode);
    }

    /* Write superblock */
    ufs_super_t *sb = (ufs_super_t *)block_ptr(0);
    sb->s_magic = UFS_MAGIC;
    sb->s_block_size = UFS_BLOCK_SIZE;
    sb->s_block_count = block_count;
    sb->s_inode_count = inode_count;
    sb->s_free_blocks = free_blocks_count;
    sb->s_free_inodes = free_inodes_count;
    sb->s_bmap_block = 1;
    sb->s_imap_block = 2;
    sb->s_itable_block = 3;
    sb->s_data_block = data_start;
    sb->s_inode_blocks = inode_blocks;

    /* Write output file */
    FILE *fp = fopen(output, "wb");
    if (!fp) { perror(output); return 1; }
    if (fwrite(img, 1, img_size, fp) != img_size) {
        perror("fwrite");
        fclose(fp);
        return 1;
    }
    fclose(fp);
    free(img);

    if (verbose) {
        printf("mkufs: created %s\n", output);
        printf("  size:         %u bytes (%u blocks)\n", img_size, block_count);
        printf("  inodes:       %u (%u blocks)\n", inode_count, inode_blocks);
        printf("  data start:   block %u\n", data_start);
        printf("  free blocks:  %u / %u\n", free_blocks_count, block_count);
        printf("  free inodes:  %u / %u\n", free_inodes_count, inode_count);
    } else {
        printf("mkufs: created %s (%u KB, %u blocks, %u inodes)\n",
               output, img_size / 1024, block_count, inode_count);
    }

    return 0;
}
