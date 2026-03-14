/*
 * stage2.c — PPAP X68000 Stage 2: UFS kernel + rootfs loader
 *
 * Runs at 0x003000 after stage1.  Reads the UFS filesystem stored on the
 * floppy from sector 4 onward, loads:
 *   boot/kernel    → 0x006000      (kernel binary)
 *   boot/rootfs.ufs → ROOTFS_BASE  (inner rootfs UFS image)
 *
 * ROOTFS_BASE is passed by mkx68kimg.sh via -DROOTFS_BASE=0x...u.  It is set
 * to the kernel's __page_pool_start so rootfs lands above the kernel BSS
 * region (which Reset_Handler zeros at boot before the rootfs can be read).
 *
 * On completion, writes a handoff record at 0x002FF4–0x002FFC:
 *   0x002FF4  RAMD magic (0x52414D44)
 *   0x002FF8  rootfs RAM address  (= ROOTFS_BASE)
 *   0x002FFC  rootfs size in bytes
 *
 * Then copies the kernel's vector table (1 KB at 0x006000) to 0x000000,
 * restores TRAP #15 from the saved value at 0x002FF0, and jumps to
 * Reset_Handler at 0x006400.
 *
 * UFS block layout (block size = 4096 bytes, floppy sector = 1024 bytes):
 *   UFS block N = floppy logical sectors 4 + N*4 … 4 + N*4 + 3
 *
 * UFS inode layout (64 bytes each, 64 per block):
 *   Inode N in inode table block: N / 64 + s_itable_block
 *   Offset in block:             (N % 64) * 64
 */

#include <stdint.h>

/* ── Fixed addresses (must match stage1.S, target_x68k.c, and x68k.ld) ───── */

#define STAGE2_IOCS_SAVE  ((volatile uint32_t *)0x002FF0u)  /* stage1 saved IOCS */
#define STAGE2_MAGIC      ((volatile uint32_t *)0x002FF4u)  /* 'RAMD' */
#define STAGE2_RFS_ADDR   ((volatile uint32_t *)0x002FF8u)  /* rootfs RAM addr */
#define STAGE2_RFS_SIZE   ((volatile uint32_t *)0x002FFCu)  /* rootfs size */
#define STAGE2_RAMD_MAGIC 0x52414D44u

#define KERNEL_LOAD_ADDR  0x006000u
#define KERNEL_RESET_HANDLER 0x006400u

/* ROOTFS_BASE: address where stage2 loads boot/rootfs.ufs.
 * Must be >= __page_pool_start (kernel BSS end rounded to 4 KB) so that
 * Reset_Handler's BSS-zeroing loop does not overwrite the rootfs image.
 * mkx68kimg.sh reads __page_pool_start from the kernel ELF and defines this
 * macro via -DROOTFS_BASE=0x...u; the fallback below is for direct builds. */
#ifndef ROOTFS_BASE
#define ROOTFS_BASE  0x030000u  /* conservative fallback: 192 KB */
#endif

/* Scratch buffer: 4096 bytes above IOCS work area ($000400–$0007FF).
 * IOCS uses $000400–$0007FF for its own work area (puni/programmers.txt),
 * so BUF must start at $000800 or higher to avoid corrupting it. */
#define BUF               ((uint8_t *)0x000800u)

/* ── X68000 2HD floppy geometry ─────────────────────────────────────────────── */

#define FDC_PDA      0x90u  /* 2HD FDD0 */
#define FDC_MODE     0x70u  /* MFM | retry | seek */
#define SEC_LEN_CODE 3u     /* sector length code 3 = 1024 bytes */
#define FLOPPY_SEC   1024u  /* bytes per floppy sector */
#define SECS_PER_CYL 16u    /* 2 heads × 8 sectors */
#define SECS_PER_HEAD 8u

/* UFS occupies floppy sectors 4..end (sectors 0=stage1, 1-3=stage2) */
#define UFS_FLOPPY_BASE  4u
#define UFS_BLOCK_SIZE   4096u
#define UFS_FLOPPY_SECS  (UFS_BLOCK_SIZE / FLOPPY_SEC)  /* 4 sectors per UFS block */

/* ── UFS constants ────────────────────────────────────────────────────────── */

#define UFS_MAGIC             0x55465331u
#define UFS_INODE_SIZE        64u
#define UFS_INODES_PER_BLOCK  (UFS_BLOCK_SIZE / UFS_INODE_SIZE)   /* 64 */
#define UFS_DIRENT_SIZE       32u
#define UFS_DIRENTS_PER_BLOCK (UFS_BLOCK_SIZE / UFS_DIRENT_SIZE)  /* 128 */
#define UFS_NAME_MAX          27u
#define UFS_ROOT_INO          1u
#define UFS_DIRECT_BLOCKS     10u

/* ── Data structures (must match mkufs.c and ufs_format.h) ──────────────── */

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
} ufs_super_t;  /* 128 bytes */

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
} ufs_inode_t;  /* 64 bytes */

typedef struct {
    uint32_t d_ino;
    char     d_name[UFS_NAME_MAX + 1];  /* 28 bytes */
} ufs_dirent_t;  /* 32 bytes */

/* ── IOCS _B_READ wrapper ─────────────────────────────────────────────────── */

static void read_floppy_sector(uint32_t lsec, void *dest)
{
    uint32_t cyl  = lsec / SECS_PER_CYL;
    uint32_t wcyl = lsec % SECS_PER_CYL;
    uint32_t head = wcyl >> 3;
    uint32_t sec  = (wcyl & 7u) + 1u;  /* 1-based */

    uint32_t d2 = (SEC_LEN_CODE << 24) | (cyl << 16) | (head << 8) | sec;

    register uint32_t d0 asm("d0") = 0x46u;
    register uint32_t d1 asm("d1") = (FDC_PDA << 8) | FDC_MODE;
    register uint32_t _d2 asm("d2") = d2;
    register uint32_t d3 asm("d3") = FLOPPY_SEC;
    register void *a1 asm("a1") = dest;
    asm volatile("trap #15"
                 : "+r"(d0)
                 : "r"(d1), "r"(_d2), "r"(d3), "r"(a1)
                 : "a0", "memory");
    /* Ignore FDC status for simplicity; stage1 already verified the boot */
    (void)d0;
}

/* ── UFS block read ───────────────────────────────────────────────────────── */

static void read_ufs_block(uint32_t block_no, void *dest)
{
    uint32_t lsec = UFS_FLOPPY_BASE + block_no * UFS_FLOPPY_SECS;
    uint8_t *p = (uint8_t *)dest;
    for (uint32_t i = 0; i < UFS_FLOPPY_SECS; i++) {
        read_floppy_sector(lsec + i, p);
        p += FLOPPY_SEC;
    }
}

/* ── Name lookup in a directory block ────────────────────────────────────── */

static uint32_t find_in_dir(const uint8_t *dirblock, const char *name)
{
    const ufs_dirent_t *d = (const ufs_dirent_t *)dirblock;
    for (uint32_t i = 0; i < UFS_DIRENTS_PER_BLOCK; i++, d++) {
        if (d->d_ino == 0u)
            continue;
        uint32_t j = 0;
        while (j < UFS_NAME_MAX && d->d_name[j] == name[j] && name[j])
            j++;
        if (d->d_name[j] == name[j])  /* both '\0' */
            return d->d_ino;
    }
    return 0u;
}

/* ── Read inode from a loaded inode-table block ───────────────────────────── */

static void copy_inode(const uint8_t *itable_block, uint32_t ino, ufs_inode_t *out)
{
    uint32_t idx = ino % UFS_INODES_PER_BLOCK;  /* mkufs: blk=3+ino/64, off=(ino%64)*64 */
    const uint8_t *src = itable_block + idx * UFS_INODE_SIZE;
    uint8_t *dst = (uint8_t *)out;
    for (uint32_t i = 0; i < UFS_INODE_SIZE; i++)
        dst[i] = src[i];
}

/* ── Load a file's data blocks to dest, return bytes loaded ──────────────── */

static uint32_t load_file(const ufs_inode_t *inode, uint8_t *dest)
{
    uint32_t remaining = inode->i_size;
    uint32_t off = 0u;

    /* Direct blocks */
    for (uint32_t i = 0; i < UFS_DIRECT_BLOCKS && remaining > 0u; i++) {
        if (inode->i_direct[i] == 0u)
            break;
        read_ufs_block(inode->i_direct[i], dest + off);
        uint32_t chunk = remaining < UFS_BLOCK_SIZE ? remaining : UFS_BLOCK_SIZE;
        off += chunk;
        remaining -= chunk;
    }

    /* Single indirect block (needed for files > 40 KB) */
    if (remaining > 0u && inode->i_indirect != 0u) {
        /* Read indirect block pointer list into BUF (reuse it — we are
         * done with directory/superblock data at this point) */
        read_ufs_block(inode->i_indirect, BUF);
        const uint32_t *ptrs = (const uint32_t *)BUF;
        uint32_t nptrs = UFS_BLOCK_SIZE / sizeof(uint32_t);
        for (uint32_t i = 0; i < nptrs && remaining > 0u; i++) {
            if (ptrs[i] == 0u)
                break;
            read_ufs_block(ptrs[i], dest + off);
            uint32_t chunk = remaining < UFS_BLOCK_SIZE ? remaining : UFS_BLOCK_SIZE;
            off += chunk;
            remaining -= chunk;
        }
    }

    return inode->i_size;
}

/* ── Vector copy and kernel launch ───────────────────────────────────────── */

static void __attribute__((noreturn)) stage2_final(void)
{
    /* Mask all interrupts before overwriting the vector table */
    asm volatile("or.w #0x0700, %%sr" ::: "memory");

    /* Copy 1024 bytes (256 longwords) from 0x006000 to 0x000000 */
    /* Suppress null-pointer warning: intentional hardware vector table write */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    volatile uint32_t *src = (volatile uint32_t *)KERNEL_LOAD_ADDR;
    volatile uint32_t *dst = (volatile uint32_t *)0x0u;
    for (uint32_t i = 0u; i < 256u; i++)
        dst[i] = src[i];

    /* Restore TRAP #15 (vector 47) to IPL IOCS handler */
    dst[47] = *STAGE2_IOCS_SAVE;
#pragma GCC diagnostic pop

    /* Jump to kernel Reset_Handler */
    asm volatile("jmp 0x006400" ::: "memory");
    __builtin_unreachable();
}

/* ── Diagnostic helper: IOCS _B_PUTC via TRAP #15 ────────────────────────── */

static inline void iocs_putc(char c)
{
    register uint32_t d0 asm("d0") = 0x20u;
    register uint32_t d1 asm("d1") = (uint32_t)(uint8_t)c;
    asm volatile("trap #15" : "+r"(d0) : "r"(d1) : "a0", "a1", "memory");
}

/* ── Stage 2 main ─────────────────────────────────────────────────────────── */

void stage2_main(void)
{
    uint32_t itable_block;
    ufs_inode_t inode;
    uint32_t boot_ino, kernel_ino, rootfs_ino;

    /* ── Read UFS superblock (block 0) — NO iocs_putc before first _B_READ ── */
    read_ufs_block(0u, BUF);
    /* Diagnostic: "PA" printed AFTER the first floppy read so that iocs_putc
     * does not corrupt the IOCS work area ($400-$7FF) before _B_READ runs */
    iocs_putc('P'); iocs_putc('A');
    const ufs_super_t *sb = (const ufs_super_t *)BUF;
    if (sb->s_magic != UFS_MAGIC)
        goto halt;
    iocs_putc('1');  /* superblock magic OK */
    itable_block = sb->s_itable_block;

    /* ── Read inode table block (contains root inode at index 0) ─────────── */
    read_ufs_block(itable_block, BUF);
    copy_inode(BUF, UFS_ROOT_INO, &inode);

    /* ── Walk root directory to find "boot" ──────────────────────────────── */
    if (inode.i_direct[0] == 0u)
        goto halt;
    iocs_putc('2');  /* root inode has data block */
    read_ufs_block(inode.i_direct[0], BUF);
    boot_ino = find_in_dir(BUF, "boot");
    if (boot_ino == 0u)
        goto halt;
    iocs_putc('3');  /* found "boot" directory */

    /* ── Load boot directory inode ───────────────────────────────────────── */
    {
        uint32_t blk = itable_block + boot_ino / UFS_INODES_PER_BLOCK;
        read_ufs_block(blk, BUF);
        copy_inode(BUF, boot_ino, &inode);
    }

    /* ── Walk boot/ directory to find "kernel" and "rootfs.ufs" ─────────── */
    if (inode.i_direct[0] == 0u)
        goto halt;
    iocs_putc('4');  /* boot inode has data block */
    read_ufs_block(inode.i_direct[0], BUF);
    kernel_ino = find_in_dir(BUF, "kernel");
    rootfs_ino = find_in_dir(BUF, "rootfs.ufs");
    if (kernel_ino == 0u || rootfs_ino == 0u)
        goto halt;
    iocs_putc('5');  /* found kernel and rootfs.ufs */

    /* ── Load kernel to 0x006000 ─────────────────────────────────────────── */
    {
        uint32_t blk = itable_block + kernel_ino / UFS_INODES_PER_BLOCK;
        read_ufs_block(blk, BUF);
        copy_inode(BUF, kernel_ino, &inode);
    }
    uint32_t kernel_size = load_file(&inode, (uint8_t *)KERNEL_LOAD_ADDR);
    (void)kernel_size;
    iocs_putc('6');  /* kernel loaded */

    /* Load rootfs at the kernel page pool base (passed via -DROOTFS_BASE by
     * mkx68kimg.sh from __page_pool_start in the kernel ELF).  This ensures
     * rootfs lands above the kernel BSS region, which Reset_Handler zeros
     * before the kernel can read the rootfs. */
    uint32_t rootfs_addr = ROOTFS_BASE;

    /* ── Load rootfs.ufs after kernel ────────────────────────────────────── */
    {
        uint32_t blk = itable_block + rootfs_ino / UFS_INODES_PER_BLOCK;
        read_ufs_block(blk, BUF);
        copy_inode(BUF, rootfs_ino, &inode);
    }
    uint32_t rootfs_size = load_file(&inode, (uint8_t *)rootfs_addr);
    iocs_putc('!');  /* rootfs loaded — about to jump to kernel */

    /* ── Write handoff record for kernel ─────────────────────────────────── */
    *STAGE2_MAGIC    = STAGE2_RAMD_MAGIC;
    *STAGE2_RFS_ADDR = rootfs_addr;
    *STAGE2_RFS_SIZE = rootfs_size;

    /* ── Patch vectors and jump to kernel ────────────────────────────────── */
    stage2_final();

halt:
    for (;;)
        asm volatile("stop #0x2700" ::: "memory");
}
