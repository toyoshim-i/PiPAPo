/*
 * mkufs.c — Create a formatted UFS image file
 *
 * Host tool that generates an empty UFS filesystem image suitable for
 * use with the PPAP loopback block device.  Optionally populates the
 * image from a host directory tree.
 *
 * Usage:
 *   mkufs [-s SIZE] [-i INODES] [-f FORMAT] [-v] [-p DIR] <output_file>
 *
 *   -s SIZE   Image size (e.g., 64K, 1M, 64M).  Default: 64K.
 *   -i INODES Override inode count (default: block_count/4, min 64).
 *   -f FORMAT Filesystem format: legacy | 44bsd. Default: legacy.
 *   -B        Write all multi-byte fields in big-endian (for M68K targets).
 *   -p DIR    Populate from host directory tree.
 *   -v        Verbose: print layout summary.
 *
 * Build:   cc -O2 -o mkufs mkufs.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
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

#define UFS44_MAGIC           0x00011954u
#define UFS44_BOOT_BYTES      8192u
#define UFS44_SB_OFFSET       8192u
#define UFS44_SB_SIZE         1536u
#define UFS44_CG_OFFSET       16384u
#define UFS44_POSTBLFMT       1u
#define UFS44_FSOK            0x7c269d38u
#define UFS44_FSCLEAN         0x01u
#define UFS44_OPTTIME         0u

#define UFS44_FS_SBLKNO_OFF       8u
#define UFS44_FS_CBLKNO_OFF      12u
#define UFS44_FS_IBLKNO_OFF      16u
#define UFS44_FS_DBLKNO_OFF      20u
#define UFS44_FS_TIME_OFF        32u
#define UFS44_FS_SIZE_OFF        36u
#define UFS44_FS_DSIZE_OFF       40u
#define UFS44_FS_NCG_OFF         44u
#define UFS44_FS_BSIZE_OFF       48u
#define UFS44_FS_FSIZE_OFF       52u
#define UFS44_FS_FRAG_OFF        56u
#define UFS44_FS_MINFREE_OFF     60u
#define UFS44_FS_ROTDELAY_OFF    64u
#define UFS44_FS_RPS_OFF         68u
#define UFS44_FS_BMASK_OFF       72u
#define UFS44_FS_FMASK_OFF       76u
#define UFS44_FS_BSHIFT_OFF      80u
#define UFS44_FS_FSHIFT_OFF      84u
#define UFS44_FS_MAXCONTIG_OFF   88u
#define UFS44_FS_MAXBPG_OFF      92u
#define UFS44_FS_FRAGSHIFT_OFF   96u
#define UFS44_FS_FSBTODB_OFF    100u
#define UFS44_FS_SBSIZE_OFF     104u
#define UFS44_FS_CSMASK_OFF     108u
#define UFS44_FS_CSSHIFT_OFF    112u
#define UFS44_FS_NINDIR_OFF     116u
#define UFS44_FS_INOPB_OFF      120u
#define UFS44_FS_NSPF_OFF       124u
#define UFS44_FS_OPTIM_OFF      128u
#define UFS44_FS_INTERLEAVE_OFF 136u
#define UFS44_FS_TRACKSKEW_OFF  140u
#define UFS44_FS_ID0_OFF        144u
#define UFS44_FS_ID1_OFF        148u
#define UFS44_FS_CSADDR_OFF     152u
#define UFS44_FS_CSSIZE_OFF     156u
#define UFS44_FS_CGSIZE_OFF     160u
#define UFS44_FS_NTRAK_OFF      164u
#define UFS44_FS_NSECT_OFF      168u
#define UFS44_FS_SPC_OFF        172u
#define UFS44_FS_NCYL_OFF       176u
#define UFS44_FS_CPG_OFF        180u
#define UFS44_FS_IPG_OFF        184u
#define UFS44_FS_FPG_OFF        188u
#define UFS44_FS_CSTOTAL_NDIR_OFF   192u
#define UFS44_FS_CSTOTAL_NBFREE_OFF 196u
#define UFS44_FS_CSTOTAL_NIFREE_OFF 200u
#define UFS44_FS_CSTOTAL_NFFREE_OFF 204u
#define UFS44_FS_FMOD_OFF       208u
#define UFS44_FS_CLEAN_OFF      209u
#define UFS44_FS_RONLY_OFF      210u
#define UFS44_FS_FLAGS_OFF      211u
#define UFS44_FS_STATE_OFF     1352u
#define UFS44_FS_POSTBLFMT_OFF 1356u
#define UFS44_FS_NRPOS_OFF     1360u
#define UFS44_FS_POSTBLOFF_OFF 1364u
#define UFS44_FS_ROTBLOFF_OFF  1368u
#define UFS44_FS_MAGIC_OFF     1372u

#define UFS44_CG_LINK_OFF           0u
#define UFS44_CG_MAGIC_OFF          4u
#define UFS44_CG_TIME_OFF           8u
#define UFS44_CG_CGX_OFF           12u
#define UFS44_CG_NCYL_OFF          16u
#define UFS44_CG_NIBLK_OFF         18u
#define UFS44_CG_NDBLK_OFF         20u
#define UFS44_CG_CS_NDIR_OFF       24u
#define UFS44_CG_CS_NBFREE_OFF     28u
#define UFS44_CG_CS_NIFREE_OFF     32u
#define UFS44_CG_CS_NFFREE_OFF     36u
#define UFS44_CG_ROTOR_OFF         40u
#define UFS44_CG_FROTOR_OFF        44u
#define UFS44_CG_IROTOR_OFF        48u
#define UFS44_CG_FRSUM_OFF         52u
#define UFS44_CG_BTOTOFF_OFF       84u
#define UFS44_CG_BOFF_OFF          88u
#define UFS44_CG_IUSEDOFF_OFF      92u
#define UFS44_CG_FREEOFF_OFF       96u
#define UFS44_CG_NEXTFREEOFF_OFF  100u
#define UFS44_CG_CLUSTERSUMOFF_OFF 104u
#define UFS44_CG_CLUSTEROFF_OFF   108u
#define UFS44_CG_NCLUSTERBLKS_OFF 112u

#define UFS44_CG_MAGIC            0x00090255u

#define UFS44_ROOT_INO            2u
#define UFS44_FIRST_INO           3u
#define UFS44_INODE_SIZE          128u
#define UFS44_ROOT_MODE           0040755u

#define UFS44_UI_MODE_OFF         0x00u
#define UFS44_UI_NLINK_OFF        0x02u
#define UFS44_UI_UID16_OFF        0x04u
#define UFS44_UI_GID16_OFF        0x06u
#define UFS44_UI_SIZE_OFF         0x08u
#define UFS44_UI_ATIME_OFF        0x10u
#define UFS44_UI_MTIME_OFF        0x18u
#define UFS44_UI_CTIME_OFF        0x20u
#define UFS44_UI_DB_OFF           0x28u
#define UFS44_UI_IB_OFF           0x58u
#define UFS44_UI_FLAGS_OFF        0x64u
#define UFS44_UI_BLOCKS_OFF       0x68u
#define UFS44_UI_GEN_OFF          0x6cu
#define UFS44_UI_UID_OFF          0x70u
#define UFS44_UI_GID_OFF          0x74u

#define UFS44_DT_DIR              4u
#define UFS44_DIRENT_INO_OFF      0u
#define UFS44_DIRENT_RECLEN_OFF   4u
#define UFS44_DIRENT_TYPE_OFF     6u
#define UFS44_DIRENT_NAMLEN_OFF   7u
#define UFS44_DIRENT_NAME_OFF     8u

#define UFS44_DT_REG              8u
#define UFS44_DT_LNK             10u
#define UFS44_NAME_MAX          255u

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

/* ── Endian helpers ──────────────────────────────────────────────────── */

static int be_mode;  /* -B: write multi-byte fields big-endian (for M68K) */

typedef enum {
    UFS_FORMAT_LEGACY = 0,
    UFS_FORMAT_44BSD = 1,
} ufs_format_t;

static ufs_format_t format_mode = UFS_FORMAT_LEGACY;

static int parse_format(const char *s, ufs_format_t *out)
{
    if (strcmp(s, "legacy") == 0 ||
        strcmp(s, "ppap") == 0 ||
        strcmp(s, "ppap-legacy") == 0) {
        *out = UFS_FORMAT_LEGACY;
        return 0;
    }
    if (strcmp(s, "44bsd") == 0 ||
        strcmp(s, "ufs1-44bsd") == 0) {
        *out = UFS_FORMAT_44BSD;
        return 0;
    }
    return -1;
}

static const char *format_name(ufs_format_t format)
{
    switch (format) {
    case UFS_FORMAT_LEGACY:
        return "legacy";
    case UFS_FORMAT_44BSD:
        return "44bsd";
    }
    return "unknown";
}

static void print_usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [-s SIZE] [-i INODES] [-f FORMAT] [-B] [-v] "
            "[-p DIR] <output>\n",
            argv0);
}

static uint32_t w32(uint32_t v)
{
    if (!be_mode) return v;
    return ((v & 0xFFu) << 24) | (((v >> 8) & 0xFFu) << 16) |
           (((v >> 16) & 0xFFu) << 8) | ((v >> 24) & 0xFFu);
}

static uint16_t w16(uint16_t v)
{
    if (!be_mode) return v;
    return (uint16_t)(((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu));
}

static uint64_t w64(uint64_t v)
{
    if (!be_mode) return v;
    return ((v & 0x00000000000000FFull) << 56) |
           ((v & 0x000000000000FF00ull) << 40) |
           ((v & 0x0000000000FF0000ull) << 24) |
           ((v & 0x00000000FF000000ull) << 8) |
           ((v & 0x000000FF00000000ull) >> 8) |
           ((v & 0x0000FF0000000000ull) >> 24) |
           ((v & 0x00FF000000000000ull) >> 40) |
           ((v & 0xFF00000000000000ull) >> 56);
}

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

static uint32_t ufs44_fs_iblkno;
static uint32_t ufs44_fs_dblkno;
static uint32_t ufs44_fs_dsize;
static uint32_t ufs44_cg_iusedoff;
static uint32_t ufs44_cg_freeoff;
static uint32_t ufs44_cg_clusteroff;
static uint32_t ufs44_dir_count;

/* ── Helpers ─────────────────────────────────────────────────────────── */

static uint8_t *block_ptr(uint32_t blk)
{
    return &img[blk * UFS_BLOCK_SIZE];
}

static void put8(uint32_t off, uint8_t val)
{
    img[off] = val;
}

static void put16(uint32_t off, uint16_t val)
{
    uint16_t raw = w16(val);
    memcpy(&img[off], &raw, sizeof(raw));
}

static void put32(uint32_t off, uint32_t val)
{
    uint32_t raw = w32(val);
    memcpy(&img[off], &raw, sizeof(raw));
}

static void put64(uint32_t off, uint64_t val)
{
    uint64_t raw = w64(val);
    memcpy(&img[off], &raw, sizeof(raw));
}

static uint32_t align_up(uint32_t value, uint32_t align)
{
    return (value + align - 1u) & ~(align - 1u);
}

static void set_bit_le(uint32_t off, uint32_t bit)
{
    img[off + (bit >> 3)] |= (uint8_t)(1u << (bit & 7u));
}

static void put_dirent(uint32_t off, uint32_t ino, uint16_t reclen,
                       uint8_t type, const char *name)
{
    size_t name_len = strlen(name);
    put32(off + UFS44_DIRENT_INO_OFF, ino);
    put16(off + UFS44_DIRENT_RECLEN_OFF, reclen);
    put8(off + UFS44_DIRENT_TYPE_OFF, type);
    put8(off + UFS44_DIRENT_NAMLEN_OFF, (uint8_t)name_len);
    memcpy(&img[off + UFS44_DIRENT_NAME_OFF], name, name_len + 1u);
}

static void clear_bit_le(uint32_t off, uint32_t bit)
{
    img[off + (bit >> 3)] &= (uint8_t)~(1u << (bit & 7u));
}

static uint32_t ufs44_inode_offset(uint32_t ino)
{
    return ufs44_fs_iblkno * UFS_BLOCK_SIZE + ino * UFS44_INODE_SIZE;
}

static void write_44bsd_inode(uint32_t ino, uint16_t mode, uint16_t nlink,
                              uint64_t size, const uint32_t *direct_blocks,
                              uint32_t block_count_512, uint32_t fs_time)
{
    uint32_t inode_off = ufs44_inode_offset(ino);
    put16(inode_off + UFS44_UI_MODE_OFF, mode);
    put16(inode_off + UFS44_UI_NLINK_OFF, nlink);
    put16(inode_off + UFS44_UI_UID16_OFF, 0);
    put16(inode_off + UFS44_UI_GID16_OFF, 0);
    put64(inode_off + UFS44_UI_SIZE_OFF, size);
    put32(inode_off + UFS44_UI_ATIME_OFF + 0u, fs_time);
    put32(inode_off + UFS44_UI_ATIME_OFF + 4u, 0);
    put32(inode_off + UFS44_UI_MTIME_OFF + 0u, fs_time);
    put32(inode_off + UFS44_UI_MTIME_OFF + 4u, 0);
    put32(inode_off + UFS44_UI_CTIME_OFF + 0u, fs_time);
    put32(inode_off + UFS44_UI_CTIME_OFF + 4u, 0);
    for (uint32_t i = 0; i < 12u; i++) {
        uint32_t block = direct_blocks ? direct_blocks[i] : 0;
        put32(inode_off + UFS44_UI_DB_OFF + i * 4u, block);
    }
    put32(inode_off + UFS44_UI_IB_OFF + 0u, 0);
    put32(inode_off + UFS44_UI_IB_OFF + 4u, 0);
    put32(inode_off + UFS44_UI_IB_OFF + 8u, 0);
    put32(inode_off + UFS44_UI_FLAGS_OFF, 0);
    put32(inode_off + UFS44_UI_BLOCKS_OFF, block_count_512);
    put32(inode_off + UFS44_UI_GEN_OFF, 0);
    put32(inode_off + UFS44_UI_UID_OFF, 0);
    put32(inode_off + UFS44_UI_GID_OFF, 0);
}

static uint32_t alloc_44bsd_inode(void)
{
    if (next_free_inode >= inode_count) {
        fprintf(stderr, "mkufs: out of 44bsd inodes\n");
        exit(1);
    }
    uint32_t ino = next_free_inode++;
    set_bit_le(UFS44_CG_OFFSET + ufs44_cg_iusedoff, ino);
    free_inodes_count--;
    return ino;
}

static uint32_t alloc_44bsd_block(void)
{
    if (next_free_block >= ufs44_fs_dsize) {
        fprintf(stderr, "mkufs: out of 44bsd data blocks\n");
        exit(1);
    }
    uint32_t rel = next_free_block++;
    clear_bit_le(UFS44_CG_OFFSET + ufs44_cg_freeoff, rel);
    clear_bit_le(UFS44_CG_OFFSET + ufs44_cg_clusteroff, rel);
    free_blocks_count--;
    return ufs44_fs_dblkno + rel;
}

static void write_44bsd_empty_dir_block(uint32_t blk, uint32_t self_ino,
                                        uint32_t parent_ino)
{
    uint32_t dir_off = blk * UFS_BLOCK_SIZE;
    memset(&img[dir_off], 0, UFS_BLOCK_SIZE);
    put_dirent(dir_off, self_ino, 12u, UFS44_DT_DIR, ".");
    put_dirent(dir_off + 12u, parent_ino,
               (uint16_t)(UFS_BLOCK_SIZE - 12u), UFS44_DT_DIR, "..");
}

static void init_44bsd_allocator_state(uint32_t fs_iblkno, uint32_t fs_dblkno,
                                       uint32_t fs_dsize, uint32_t cg_iusedoff,
                                       uint32_t cg_freeoff,
                                       uint32_t cg_clusteroff)
{
    ufs44_fs_iblkno = fs_iblkno;
    ufs44_fs_dblkno = fs_dblkno;
    ufs44_fs_dsize = fs_dsize;
    ufs44_cg_iusedoff = cg_iusedoff;
    ufs44_cg_freeoff = cg_freeoff;
    ufs44_cg_clusteroff = cg_clusteroff;
    next_free_inode = UFS44_FIRST_INO;
    next_free_block = 0;
}

static uint16_t get16(uint32_t off)
{
    uint16_t val;
    memcpy(&val, &img[off], sizeof(val));
    return w16(val);
}

static uint32_t dirent_min_reclen(uint8_t namlen)
{
    return align_up(8u + (uint32_t)namlen, 4u);
}

typedef struct {
    uint32_t ino;
    uint32_t direct[12];
    uint32_t nblocks;
    uint32_t tail_entry_off;  /* absolute img offset of last dirent */
    uint16_t nlink;
    uint32_t fs_time;
} ufs44_dir_ctx_t;

static void add_44bsd_dirent(ufs44_dir_ctx_t *ctx, uint32_t child_ino,
                             uint8_t type, const char *name)
{
    uint8_t namlen = (uint8_t)strnlen(name, UFS44_NAME_MAX);
    uint32_t new_min = dirent_min_reclen(namlen);

    uint32_t tail = ctx->tail_entry_off;
    uint8_t tail_namlen = img[tail + UFS44_DIRENT_NAMLEN_OFF];
    uint32_t tail_min = dirent_min_reclen(tail_namlen);
    uint32_t tail_reclen = get16(tail + UFS44_DIRENT_RECLEN_OFF);

    if (tail_reclen >= tail_min + new_min) {
        /* Split: shrink current tail, write new entry in freed space */
        put16(tail + UFS44_DIRENT_RECLEN_OFF, (uint16_t)tail_min);
        uint32_t new_off = tail + tail_min;
        put_dirent(new_off, child_ino,
                   (uint16_t)(tail_reclen - tail_min), type, name);
        ctx->tail_entry_off = new_off;
    } else {
        /* Not enough room in current block — allocate a new one */
        if (ctx->nblocks >= 12u) {
            fprintf(stderr, "mkufs: 44bsd directory too large (> 12 blocks)\n");
            exit(1);
        }
        uint32_t new_blk = alloc_44bsd_block();
        memset(block_ptr(new_blk), 0, UFS_BLOCK_SIZE);
        ctx->direct[ctx->nblocks++] = new_blk;
        uint32_t new_off = new_blk * UFS_BLOCK_SIZE;
        put_dirent(new_off, child_ino, (uint16_t)UFS_BLOCK_SIZE, type, name);
        ctx->tail_entry_off = new_off;
    }

    /* Flush dir inode: update size and direct[] */
    uint32_t inode_off = ufs44_inode_offset(ctx->ino);
    put64(inode_off + UFS44_UI_SIZE_OFF,
          (uint64_t)ctx->nblocks * UFS_BLOCK_SIZE);
    for (uint32_t i = 0; i < 12u; i++)
        put32(inode_off + UFS44_UI_DB_OFF + i * 4u, ctx->direct[i]);
    put32(inode_off + UFS44_UI_BLOCKS_OFF,
          ctx->nblocks * UFS_BLOCK_SIZE / 512u);
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
    ufs_inode_t tmp;
    tmp.i_mode     = w16(inode->i_mode);
    tmp.i_nlink    = w16(inode->i_nlink);
    tmp.i_uid      = w16(inode->i_uid);
    tmp.i_gid      = w16(inode->i_gid);
    tmp.i_size     = w32(inode->i_size);
    tmp.i_mtime    = w32(inode->i_mtime);
    tmp.i_ctime    = w32(inode->i_ctime);
    for (int k = 0; k < UFS_DIRECT_BLOCKS; k++)
        tmp.i_direct[k] = w32(inode->i_direct[k]);
    tmp.i_indirect = w32(inode->i_indirect);
    memcpy(block_ptr(blk) + off, &tmp, sizeof(ufs_inode_t));
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
                entries[i].d_ino = w32(file_ino);
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
    entries[0].d_ino = w32(file_ino);
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
            ind[i - UFS_DIRECT_BLOCKS] = w32(blk);
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
        entries[0].d_ino = w32(ino);
        strcpy(entries[0].d_name, ".");
        entries[1].d_ino = w32(parent_ino);
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
            /* Fast symlink: store inline in i_direct.
             * write_inode() byte-swaps each i_direct[k] via w32() for -B
             * mode, but symlink data is raw bytes, not block pointers.
             * Pre-swap so w32(w32(x)) = x cancels both swaps. */
            memcpy(inode.i_direct, target, len);
            if (be_mode) {
                for (int k = 0; k < UFS_DIRECT_BLOCKS; k++)
                    inode.i_direct[k] = w32(inode.i_direct[k]);
            }
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

static int write_output_file(const char *output)
{
    FILE *fp = fopen(output, "wb");
    if (!fp) { perror(output); return 1; }
    if (fwrite(img, 1, img_size, fp) != img_size) {
        perror("fwrite");
        fclose(fp);
        return 1;
    }
    fclose(fp);
    return 0;
}

static void print_summary(const char *output, int verbose)
{
    if (verbose) {
        printf("mkufs: created %s\n", output);
        printf("  format:       %s\n", format_name(format_mode));
        printf("  size:         %u bytes (%u blocks)\n", img_size, block_count);
        printf("  inodes:       %u (%u blocks)\n", inode_count, inode_blocks);
        printf("  data start:   block %u\n", data_start);
        printf("  free blocks:  %u / %u\n", free_blocks_count, block_count);
        printf("  free inodes:  %u / %u\n", free_inodes_count, inode_count);
    } else {
        printf("mkufs: created %s (%s, %u KB, %u blocks, %u inodes)\n",
               output, format_name(format_mode), img_size / 1024,
               block_count, inode_count);
    }
}

static int build_legacy_image(uint32_t size, uint32_t inode_override,
                              const char *populate_dir_path)
{
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
    /* 1 inode per 4 blocks (16 KB), minimum 64; overridable with -i */
    if (inode_override > 0) {
        inode_count = inode_override;
    } else {
        inode_count = block_count / 4;
        if (inode_count < 64) inode_count = 64;
    }
    if (inode_count > UFS_BLOCK_SIZE * 8) inode_count = UFS_BLOCK_SIZE * 8;

    inode_blocks =
        (inode_count + UFS_INODES_PER_BLOCK - 1) / UFS_INODES_PER_BLOCK;
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
    root_entries[0].d_ino = w32(root_ino);
    strcpy(root_entries[0].d_name, ".");
    root_entries[1].d_ino = w32(root_ino);
    strcpy(root_entries[1].d_name, "..");

    write_inode(root_ino, &root_inode);

    /* Populate from host directory if specified */
    if (populate_dir_path) {
        populate_dir(populate_dir_path, root_ino, &root_inode);
    }

    /* Write superblock */
    ufs_super_t *sb = (ufs_super_t *)block_ptr(0);
    sb->s_magic        = w32(UFS_MAGIC);
    sb->s_block_size   = w32(UFS_BLOCK_SIZE);
    sb->s_block_count  = w32(block_count);
    sb->s_inode_count  = w32(inode_count);
    sb->s_free_blocks  = w32(free_blocks_count);
    sb->s_free_inodes  = w32(free_inodes_count);
    sb->s_bmap_block   = w32(1);
    sb->s_imap_block   = w32(2);
    sb->s_itable_block = w32(3);
    sb->s_data_block   = w32(data_start);
    sb->s_inode_blocks = w32(inode_blocks);

    return 0;
}

static void populate_44bsd_dir(const char *host_path, ufs44_dir_ctx_t *ctx,
                               uint32_t fs_time);

static void populate_44bsd_entry(const char *host_path, const char *name,
                                 ufs44_dir_ctx_t *parent_ctx, uint32_t fs_time)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", host_path, name);

    struct stat st;
    if (lstat(path, &st) < 0) {
        perror(path);
        return;
    }

    char short_name[UFS44_NAME_MAX + 1];
    strncpy(short_name, name, UFS44_NAME_MAX);
    short_name[UFS44_NAME_MAX] = '\0';

    if (S_ISREG(st.st_mode)) {
        uint32_t ino = alloc_44bsd_inode();
        uint32_t direct[12] = {0};
        uint32_t nblks = 0;
        uint32_t fsize = 0;

        if (st.st_size > 0) {
            FILE *fp = fopen(path, "rb");
            if (fp) {
                uint8_t *data = malloc((size_t)st.st_size);
                if (data) {
                    size_t n = fread(data, 1, (size_t)st.st_size, fp);
                    fsize = (uint32_t)n;
                    nblks = (fsize + UFS_BLOCK_SIZE - 1u) / UFS_BLOCK_SIZE;
                    if (nblks > 12u) {
                        fprintf(stderr,
                                "mkufs: %s: truncating to 12 blocks (48 KB)\n",
                                path);
                        nblks = 12u;
                        fsize = 12u * UFS_BLOCK_SIZE;
                    }
                    for (uint32_t i = 0; i < nblks; i++) {
                        uint32_t blk = alloc_44bsd_block();
                        uint32_t off_in = i * UFS_BLOCK_SIZE;
                        uint32_t chunk =
                            (fsize - off_in > UFS_BLOCK_SIZE)
                            ? UFS_BLOCK_SIZE : fsize - off_in;
                        memset(block_ptr(blk), 0, UFS_BLOCK_SIZE);
                        memcpy(block_ptr(blk), data + off_in, chunk);
                        direct[i] = blk;
                    }
                    free(data);
                }
                fclose(fp);
            }
        }

        write_44bsd_inode(ino,
                          (uint16_t)(S_IFREG_ | (st.st_mode & 0777u)),
                          1, fsize, direct,
                          nblks * UFS_BLOCK_SIZE / 512u, fs_time);
        add_44bsd_dirent(parent_ctx, ino, UFS44_DT_REG, short_name);

    } else if (S_ISDIR(st.st_mode)) {
        uint32_t ino = alloc_44bsd_inode();
        uint32_t dir_blk = alloc_44bsd_block();
        ufs44_dir_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ino = ino;
        ctx.direct[0] = dir_blk;
        ctx.nblocks = 1;
        ctx.tail_entry_off = dir_blk * UFS_BLOCK_SIZE + 12u;
        ctx.nlink = 2;
        ctx.fs_time = fs_time;

        write_44bsd_empty_dir_block(dir_blk, ino, parent_ctx->ino);
        write_44bsd_inode(ino,
                          (uint16_t)(S_IFDIR_ | (st.st_mode & 0777u)),
                          2, UFS_BLOCK_SIZE, ctx.direct,
                          UFS_BLOCK_SIZE / 512u, fs_time);
        ufs44_dir_count++;
        add_44bsd_dirent(parent_ctx, ino, UFS44_DT_DIR, short_name);

        /* Bump parent nlink for the ".." back-link */
        parent_ctx->nlink++;
        put16(ufs44_inode_offset(parent_ctx->ino) + UFS44_UI_NLINK_OFF,
              parent_ctx->nlink);

        populate_44bsd_dir(path, &ctx, fs_time);

        /* Write final nlink for the new dir (may have grown via sub-dirs) */
        put16(ufs44_inode_offset(ino) + UFS44_UI_NLINK_OFF, ctx.nlink);

    } else if (S_ISLNK(st.st_mode)) {
        char target[UFS44_NAME_MAX + 1];
        ssize_t len = readlink(path, target, sizeof(target) - 1);
        if (len < 0) { perror(path); return; }
        target[len] = '\0';

        uint32_t ino = alloc_44bsd_inode();

        if ((size_t)len <= 12u * sizeof(uint32_t)) {
            /* Fast symlink: target stored inline in di_db, no disk block */
            write_44bsd_inode(ino, (uint16_t)(S_IFLNK_ | 0777u), 1,
                              (uint64_t)(uint32_t)len, NULL, 0u, fs_time);
            /* Overwrite di_db with raw target bytes — no endian swap */
            memcpy(&img[ufs44_inode_offset(ino) + UFS44_UI_DB_OFF],
                   target, (size_t)len);
        } else {
            uint32_t blk = alloc_44bsd_block();
            memset(block_ptr(blk), 0, UFS_BLOCK_SIZE);
            memcpy(block_ptr(blk), target, (size_t)len);
            uint32_t direct[12] = {0};
            direct[0] = blk;
            write_44bsd_inode(ino, (uint16_t)(S_IFLNK_ | 0777u), 1,
                              (uint64_t)(uint32_t)len, direct,
                              UFS_BLOCK_SIZE / 512u, fs_time);
        }
        add_44bsd_dirent(parent_ctx, ino, UFS44_DT_LNK, short_name);
    }
    /* Skip other file types (devices, sockets, FIFOs) */
}

static void populate_44bsd_dir(const char *host_path, ufs44_dir_ctx_t *ctx,
                               uint32_t fs_time)
{
    DIR *dp = opendir(host_path);
    if (!dp) { perror(host_path); return; }

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        populate_44bsd_entry(host_path, de->d_name, ctx, fs_time);
    }
    closedir(dp);
}

static void update_44bsd_summary(uint32_t ndir)
{
    put32(UFS44_SB_OFFSET + UFS44_FS_CSTOTAL_NDIR_OFF, ndir);
    put32(UFS44_SB_OFFSET + UFS44_FS_CSTOTAL_NBFREE_OFF, free_blocks_count);
    put32(UFS44_SB_OFFSET + UFS44_FS_CSTOTAL_NIFREE_OFF, free_inodes_count);
    put32(UFS44_CG_OFFSET + UFS44_CG_CS_NDIR_OFF, ndir);
    put32(UFS44_CG_OFFSET + UFS44_CG_CS_NBFREE_OFF, free_blocks_count);
    put32(UFS44_CG_OFFSET + UFS44_CG_CS_NIFREE_OFF, free_inodes_count);
}

static int build_44bsd_image(uint32_t size, uint32_t inode_override,
                             const char *populate_dir_path, int verbose)
{
    (void)inode_override;

    if (size < UFS44_CG_OFFSET + UFS_BLOCK_SIZE) {
        fprintf(stderr,
                "mkufs: 44bsd image size must be at least %u bytes\n",
                UFS44_CG_OFFSET + UFS_BLOCK_SIZE);
        return 1;
    }
    size &= ~(UFS_BLOCK_SIZE - 1);
    img_size = size;

    img = calloc(1, img_size);
    if (!img) { perror("calloc"); return 1; }

    block_count = img_size / UFS_BLOCK_SIZE;
    next_free_block = 0;
    next_free_inode = 0;

    uint32_t fs_sblkno = UFS44_SB_OFFSET / UFS_BLOCK_SIZE;
    uint32_t fs_cblkno = UFS44_CG_OFFSET / UFS_BLOCK_SIZE;
    uint32_t fs_iblkno = fs_cblkno + 1;
    uint32_t fs_dblkno = fs_iblkno + 1;
    uint32_t fs_size = block_count;
    uint32_t fs_dsize = fs_size > fs_dblkno ? fs_size - fs_dblkno : 0;
    uint32_t fs_ipg = UFS_BLOCK_SIZE / 128u;
    uint32_t fs_fpg = fs_size;
    uint32_t fs_time = (uint32_t)time(NULL);
    uint32_t fs_state = UFS44_FSOK - fs_time;
    uint32_t fs_bmask = ~(UFS_BLOCK_SIZE - 1u);
    uint32_t fs_fmask = ~(UFS_BLOCK_SIZE - 1u);
    uint32_t used_inodes = 3;
    uint32_t used_data_blocks = 1;

    uint32_t cg_iusedoff = 168u;
    uint32_t cg_iused_bytes = align_up((fs_ipg + 7u) >> 3, 4u);
    uint32_t cg_freeoff = cg_iusedoff + cg_iused_bytes;
    uint32_t cg_free_bytes = align_up((fs_dsize + 7u) >> 3, 4u);
    uint32_t cg_clustersumoff = cg_freeoff + cg_free_bytes;
    uint32_t cg_clustersum_bytes = 4u;
    uint32_t cg_clusteroff = cg_clustersumoff + cg_clustersum_bytes;
    uint32_t cg_cluster_bytes = align_up((fs_dsize + 7u) >> 3, 4u);
    uint32_t cg_btotoff = cg_clusteroff + cg_cluster_bytes;
    uint32_t cg_btot_bytes = 4u;
    uint32_t cg_boff = cg_btotoff + cg_btot_bytes;
    uint32_t cg_b_bytes = 2u;
    uint32_t cg_nextfreeoff = align_up(cg_boff + cg_b_bytes, 4u);

    inode_count = fs_ipg;
    inode_blocks = 1;
    data_start = fs_dblkno;
    free_blocks_count = fs_dsize - used_data_blocks;
    free_inodes_count = inode_count - used_inodes;

    put32(UFS44_SB_OFFSET + UFS44_FS_SBLKNO_OFF, fs_sblkno);
    put32(UFS44_SB_OFFSET + UFS44_FS_CBLKNO_OFF, fs_cblkno);
    put32(UFS44_SB_OFFSET + UFS44_FS_IBLKNO_OFF, fs_iblkno);
    put32(UFS44_SB_OFFSET + UFS44_FS_DBLKNO_OFF, fs_dblkno);
    put32(UFS44_SB_OFFSET + UFS44_FS_TIME_OFF, fs_time);
    put32(UFS44_SB_OFFSET + UFS44_FS_SIZE_OFF, fs_size);
    put32(UFS44_SB_OFFSET + UFS44_FS_DSIZE_OFF, fs_dsize);
    put32(UFS44_SB_OFFSET + UFS44_FS_NCG_OFF, 1);
    put32(UFS44_SB_OFFSET + UFS44_FS_BSIZE_OFF, UFS_BLOCK_SIZE);
    put32(UFS44_SB_OFFSET + UFS44_FS_FSIZE_OFF, UFS_BLOCK_SIZE);
    put32(UFS44_SB_OFFSET + UFS44_FS_FRAG_OFF, 1);
    put32(UFS44_SB_OFFSET + UFS44_FS_MINFREE_OFF, 0);
    put32(UFS44_SB_OFFSET + UFS44_FS_ROTDELAY_OFF, 0);
    put32(UFS44_SB_OFFSET + UFS44_FS_RPS_OFF, 60);
    put32(UFS44_SB_OFFSET + UFS44_FS_BMASK_OFF, fs_bmask);
    put32(UFS44_SB_OFFSET + UFS44_FS_FMASK_OFF, fs_fmask);
    put32(UFS44_SB_OFFSET + UFS44_FS_BSHIFT_OFF, 12);
    put32(UFS44_SB_OFFSET + UFS44_FS_FSHIFT_OFF, 12);
    put32(UFS44_SB_OFFSET + UFS44_FS_MAXCONTIG_OFF, 1);
    put32(UFS44_SB_OFFSET + UFS44_FS_MAXBPG_OFF, UFS_BLOCK_SIZE);
    put32(UFS44_SB_OFFSET + UFS44_FS_FRAGSHIFT_OFF, 0);
    put32(UFS44_SB_OFFSET + UFS44_FS_FSBTODB_OFF, 3);
    put32(UFS44_SB_OFFSET + UFS44_FS_SBSIZE_OFF, UFS44_SB_SIZE);
    put32(UFS44_SB_OFFSET + UFS44_FS_CSMASK_OFF, 0);
    put32(UFS44_SB_OFFSET + UFS44_FS_CSSHIFT_OFF, 0);
    put32(UFS44_SB_OFFSET + UFS44_FS_NINDIR_OFF, UFS_BLOCK_SIZE / 4u);
    put32(UFS44_SB_OFFSET + UFS44_FS_INOPB_OFF, UFS_BLOCK_SIZE / 128u);
    put32(UFS44_SB_OFFSET + UFS44_FS_NSPF_OFF, UFS_BLOCK_SIZE / 512u);
    put32(UFS44_SB_OFFSET + UFS44_FS_OPTIM_OFF, UFS44_OPTTIME);
    put32(UFS44_SB_OFFSET + UFS44_FS_INTERLEAVE_OFF, 1);
    put32(UFS44_SB_OFFSET + UFS44_FS_TRACKSKEW_OFF, 0);
    put32(UFS44_SB_OFFSET + UFS44_FS_ID0_OFF, fs_time);
    put32(UFS44_SB_OFFSET + UFS44_FS_ID1_OFF, fs_time ^ 0x50504150u);
    put32(UFS44_SB_OFFSET + UFS44_FS_CSADDR_OFF, 0);
    put32(UFS44_SB_OFFSET + UFS44_FS_CSSIZE_OFF, 0);
    put32(UFS44_SB_OFFSET + UFS44_FS_CGSIZE_OFF, UFS_BLOCK_SIZE);
    put32(UFS44_SB_OFFSET + UFS44_FS_NTRAK_OFF, 1);
    put32(UFS44_SB_OFFSET + UFS44_FS_NSECT_OFF, 1);
    put32(UFS44_SB_OFFSET + UFS44_FS_SPC_OFF, 1);
    put32(UFS44_SB_OFFSET + UFS44_FS_NCYL_OFF, 1);
    put32(UFS44_SB_OFFSET + UFS44_FS_CPG_OFF, 1);
    put32(UFS44_SB_OFFSET + UFS44_FS_IPG_OFF, fs_ipg);
    put32(UFS44_SB_OFFSET + UFS44_FS_FPG_OFF, fs_fpg);
    put32(UFS44_SB_OFFSET + UFS44_FS_CSTOTAL_NDIR_OFF, 1);
    put32(UFS44_SB_OFFSET + UFS44_FS_CSTOTAL_NBFREE_OFF, free_blocks_count);
    put32(UFS44_SB_OFFSET + UFS44_FS_CSTOTAL_NIFREE_OFF, free_inodes_count);
    put32(UFS44_SB_OFFSET + UFS44_FS_CSTOTAL_NFFREE_OFF, 0);
    put8(UFS44_SB_OFFSET + UFS44_FS_FMOD_OFF, 0);
    put8(UFS44_SB_OFFSET + UFS44_FS_CLEAN_OFF, UFS44_FSCLEAN);
    put8(UFS44_SB_OFFSET + UFS44_FS_RONLY_OFF, 0);
    put8(UFS44_SB_OFFSET + UFS44_FS_FLAGS_OFF, 0);
    put32(UFS44_SB_OFFSET + UFS44_FS_STATE_OFF, fs_state);
    put32(UFS44_SB_OFFSET + UFS44_FS_POSTBLFMT_OFF, UFS44_POSTBLFMT);
    put32(UFS44_SB_OFFSET + UFS44_FS_NRPOS_OFF, 1);
    put32(UFS44_SB_OFFSET + UFS44_FS_POSTBLOFF_OFF, 0);
    put32(UFS44_SB_OFFSET + UFS44_FS_ROTBLOFF_OFF, 0);
    put32(UFS44_SB_OFFSET + UFS44_FS_MAGIC_OFF, UFS44_MAGIC);

    put32(UFS44_CG_OFFSET + UFS44_CG_LINK_OFF, 0);
    put32(UFS44_CG_OFFSET + UFS44_CG_MAGIC_OFF, UFS44_CG_MAGIC);
    put32(UFS44_CG_OFFSET + UFS44_CG_TIME_OFF, fs_time);
    put32(UFS44_CG_OFFSET + UFS44_CG_CGX_OFF, 0);
    put16(UFS44_CG_OFFSET + UFS44_CG_NCYL_OFF, 1);
    put16(UFS44_CG_OFFSET + UFS44_CG_NIBLK_OFF, inode_blocks);
    put32(UFS44_CG_OFFSET + UFS44_CG_NDBLK_OFF, fs_dsize);
    put32(UFS44_CG_OFFSET + UFS44_CG_CS_NDIR_OFF, 1);
    put32(UFS44_CG_OFFSET + UFS44_CG_CS_NBFREE_OFF, free_blocks_count);
    put32(UFS44_CG_OFFSET + UFS44_CG_CS_NIFREE_OFF, free_inodes_count);
    put32(UFS44_CG_OFFSET + UFS44_CG_CS_NFFREE_OFF, 0);
    put32(UFS44_CG_OFFSET + UFS44_CG_ROTOR_OFF, 0);
    put32(UFS44_CG_OFFSET + UFS44_CG_FROTOR_OFF, 0);
    put32(UFS44_CG_OFFSET + UFS44_CG_IROTOR_OFF, used_inodes);
    put32(UFS44_CG_OFFSET + UFS44_CG_BTOTOFF_OFF, cg_btotoff);
    put32(UFS44_CG_OFFSET + UFS44_CG_BOFF_OFF, cg_boff);
    put32(UFS44_CG_OFFSET + UFS44_CG_IUSEDOFF_OFF, cg_iusedoff);
    put32(UFS44_CG_OFFSET + UFS44_CG_FREEOFF_OFF, cg_freeoff);
    put32(UFS44_CG_OFFSET + UFS44_CG_NEXTFREEOFF_OFF, cg_nextfreeoff);
    put32(UFS44_CG_OFFSET + UFS44_CG_CLUSTERSUMOFF_OFF, cg_clustersumoff);
    put32(UFS44_CG_OFFSET + UFS44_CG_CLUSTEROFF_OFF, cg_clusteroff);
    put32(UFS44_CG_OFFSET + UFS44_CG_NCLUSTERBLKS_OFF, fs_dsize);

    for (uint32_t ino = 0; ino < used_inodes; ino++)
        set_bit_le(UFS44_CG_OFFSET + cg_iusedoff, ino);

    for (uint32_t blk = used_data_blocks; blk < fs_dsize; blk++) {
        set_bit_le(UFS44_CG_OFFSET + cg_freeoff, blk);
        set_bit_le(UFS44_CG_OFFSET + cg_clusteroff, blk);
    }

    put32(UFS44_CG_OFFSET + cg_clustersumoff, 0);
    put32(UFS44_CG_OFFSET + cg_btotoff, free_blocks_count);
    put16(UFS44_CG_OFFSET + cg_boff, (uint16_t)free_blocks_count);

    init_44bsd_allocator_state(fs_iblkno, fs_dblkno, fs_dsize, cg_iusedoff,
                               cg_freeoff, cg_clusteroff);
    next_free_inode = UFS44_ROOT_INO + 1u;
        next_free_block = 1u;

        uint32_t root_dir_blk = fs_dblkno;
        uint32_t root_direct[12] = {0};
        root_direct[0] = root_dir_blk;
        write_44bsd_inode(UFS44_ROOT_INO, UFS44_ROOT_MODE, 2, UFS_BLOCK_SIZE,
                          root_direct, UFS_BLOCK_SIZE / 512u, fs_time);
        write_44bsd_empty_dir_block(root_dir_blk, UFS44_ROOT_INO, UFS44_ROOT_INO);

    ufs44_dir_count = 1;

    if (populate_dir_path) {
        ufs44_dir_ctx_t root_ctx;
        memset(&root_ctx, 0, sizeof(root_ctx));
        root_ctx.ino = UFS44_ROOT_INO;
        root_ctx.direct[0] = root_dir_blk;
        root_ctx.nblocks = 1;
        root_ctx.tail_entry_off = root_dir_blk * UFS_BLOCK_SIZE + 12u;
        root_ctx.nlink = 2;
        root_ctx.fs_time = fs_time;

        populate_44bsd_dir(populate_dir_path, &root_ctx, fs_time);
        put16(ufs44_inode_offset(UFS44_ROOT_INO) + UFS44_UI_NLINK_OFF,
              root_ctx.nlink);
    }

    update_44bsd_summary(ufs44_dir_count);

    if (verbose) {
        printf("mkufs: format 44bsd selected\n");
        printf("  boot area:    %u bytes\n", UFS44_BOOT_BYTES);
        printf("  superblock:   offset %u, size %u\n",
               UFS44_SB_OFFSET, UFS44_SB_SIZE);
        printf("  cg area:      offset %u, magic 0x%08x\n",
               UFS44_CG_OFFSET, UFS44_CG_MAGIC);
        printf("  inode area:   block %u (root inode %u written)\n",
               fs_iblkno, UFS44_ROOT_INO);
        printf("  data start:   block %u\n", fs_dblkno);
    }

    return 0;
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    uint32_t size = 64 * 1024;  /* default 64K */
    uint32_t inode_override = 0; /* 0 = use default formula */
    int verbose = 0;
    const char *populate_dir_path = NULL;
    const char *output = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            size = parse_size(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            inode_override = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            if (parse_format(argv[++i], &format_mode) < 0) {
                fprintf(stderr, "mkufs: unsupported format '%s' (use legacy or 44bsd)\n",
                        argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-B") == 0) {
            be_mode = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            populate_dir_path = argv[++i];
        } else if (argv[i][0] != '-') {
            output = argv[i];
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!output) {
        print_usage(argv[0]);
        return 1;
    }

    int rc;
    switch (format_mode) {
    case UFS_FORMAT_LEGACY:
        rc = build_legacy_image(size, inode_override, populate_dir_path);
        break;
    case UFS_FORMAT_44BSD:
        rc = build_44bsd_image(size, inode_override, populate_dir_path,
                               verbose);
        break;
    }

    if (rc != 0)
        return 1;

    if (write_output_file(output) != 0)
        return 1;

    print_summary(output, verbose);
    free(img);

    return 0;
}
