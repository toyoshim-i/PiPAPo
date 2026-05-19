/*
 * stage2.c — PPAP X68000 Stage 2: 44bsd UFS kernel loader
 *
 * Runs at 0x003000 after stage1.  Reads the 44bsd UFS that starts at
 * floppy byte 4096 (1024-byte sector 4) and loads /boot/kernel to
 * 0x006000.  The same UFS is later mounted live as the rootfs via the
 * kernel's iocs_blk driver, so no in-RAM rootfs image and no second
 * (outer) UFS layer is needed.
 *
 * Floppy layout:
 *   Sector 0       Stage1 (loaded by IPL ROM to 0x002000)
 *   Sectors 1-3    Stage2 (loaded by Stage1 to 0x003000)
 *   Sector 4+      44bsd UFS — contains /boot/kernel + userland
 *
 * 44bsd UFS layout (within the UFS partition):
 *   Bytes 0-8191       Boot block area (zeros)
 *   Bytes 8192-9727    Superblock (1536 bytes; magic at +1372)
 *   Bytes 16384-       Cylinder group descriptor (sector 32)
 *   Fragment fs_iblkno Inode table (128 B per inode, 4 inodes/sector)
 *   Fragment fs_dblkno Data area
 *
 * Endianness: mkufs is invoked with -B (big-endian) so on-disk fields
 * read natively on m68k.  Inodes are accessed via a packed struct.
 */

#include <stdint.h>

/* ── Fixed addresses (must match stage1.S, target_x68k.c, x68k.ld) ───────── */

#define STAGE2_IOCS_SAVE \
  ((volatile uint32_t *)0x002FF0u)                       /* stage1 saved IOCS */
#define STAGE2_MAGIC ((volatile uint32_t *)0x002FF4u)    /* 'RAMD' */
#define STAGE2_RAMD_MAGIC 0x52414D44u

#define KERNEL_LOAD_ADDR 0x006000u
#define KERNEL_RESET_HANDLER 0x006400u

/* Scratch buffer: one UFS block (4096 bytes) at the first free address
 * above stage2 code.  $003C00 is the first byte after stage2's 3 KB code
 * area ($003000-$003BFF) and is above all system work areas (vectors,
 * IOCS, OPM, FDC).  See arch/m68k/boot/stage2_head.S for the SSP choice. */
#define BUF ((uint8_t *)0x003C00u)

/* ── X68000 2HD floppy geometry ──────────────────────────────────────────── */

#define FDC_PDA 0x90u           /* 2HD FDD0 */
#define FDC_MODE 0x70u          /* MFM | retry | seek */
#define SEC_LEN_CODE 3u         /* sector length code 3 = 1024 bytes */
#define FLOPPY_SEC 1024u        /* bytes per floppy sector */
#define SECS_PER_CYL 16u        /* 2 heads × 8 sectors */
#define SECS_PER_HEAD 8u

/* The 44bsd UFS starts at floppy sector 4 (sectors 0=stage1, 1-3=stage2). */
#define UFS_FLOPPY_BASE 4u
#define UFS_BLOCK_SIZE 4096u
#define UFS_FLOPPY_SECS \
  (UFS_BLOCK_SIZE / FLOPPY_SEC) /* 4 floppy sectors per UFS block */

/* ── 44bsd UFS constants (must match src/kernel/vfs/ufs_format.h) ────────── */

#define UFS_MAGIC          0x00011954u
#define UFS_FRAG_SIZE      512u
#define UFS_FRAGS_PER_BLK  8u
#define UFS_INODE_SIZE     128u
#define UFS_DIRECT_BLOCKS  12u
#define UFS_ROOT_INO       2u

#define UFS_FS_IBLKNO_OFF  16u    /* offset within SB: inode-table start frag */
#define UFS_FS_MAGIC_OFF   1372u  /* offset within SB: magic */
#define UFS_SB_FRAG        16u    /* SB at fragment 16 (= byte 8192) */

/* ── On-disk 44bsd inode (128 bytes, native big-endian on m68k) ─────────── */

typedef struct {
  uint16_t i_mode;
  uint16_t i_nlink;
  uint16_t i_uid16, i_gid16;
  uint32_t i_size_hi;
  uint32_t i_size_lo;
  uint32_t i_atime, i_atimensec;
  uint32_t i_mtime, i_mtimensec;
  uint32_t i_ctime, i_ctimensec;
  uint32_t i_direct[UFS_DIRECT_BLOCKS];
  uint32_t i_ib[3];
  uint32_t i_flags;
  uint32_t i_blocks;
  uint32_t i_gen;
  uint32_t i_uid, i_gid;
  uint32_t i_spare[2];
} ufs_inode_t;

_Static_assert(sizeof(ufs_inode_t) == UFS_INODE_SIZE,
               "ufs_inode_t must be 128 bytes");

/* ── 44bsd variable-length directory entry header ───────────────────────── */

typedef struct {
  uint32_t d_ino;
  uint16_t d_reclen;
  uint8_t  d_type;
  uint8_t  d_namlen;
  /* char d_name[]; — NUL-terminated, padded to 4-byte boundary */
} ufs_dirent_t;

/* ── IOCS _B_READ wrapper ───────────────────────────────────────────────── */

static void read_floppy_sector(uint32_t lsec, void *dest) {
  uint32_t cyl  = lsec / SECS_PER_CYL;
  uint32_t wcyl = lsec % SECS_PER_CYL;
  uint32_t head = wcyl >> 3;
  uint32_t sec  = (wcyl & 7u) + 1u; /* 1-based */

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
  (void)d0; /* stage1 already verified the boot — ignore FDC status */
}

/* Read one 4 KB UFS block, addressed by its starting fragment number.
 * `frag` must be a multiple of UFS_FRAGS_PER_BLK (i.e. block-aligned). */
static void read_ufs_block(uint32_t frag, void *dest) {
  /* fragment N starts at byte UFS_FLOPPY_BASE*1024 + N*512 in floppy.
   * In 1024-byte sectors: lsec = UFS_FLOPPY_BASE + N/2.  Fragment is
   * block-aligned, so N is a multiple of 8 → N/2 is a multiple of 4. */
  uint32_t lsec = UFS_FLOPPY_BASE + (frag >> 1);
  uint8_t *p = (uint8_t *)dest;
  for (uint32_t i = 0; i < UFS_FLOPPY_SECS; i++) {
    read_floppy_sector(lsec + i, p);
    p += FLOPPY_SEC;
  }
}

/* ── Inode I/O ──────────────────────────────────────────────────────────── */

static uint32_t g_iblkno_frag; /* fs_iblkno from SB — first frag of inode table */

/* Read inode `ino` into `out`.  Clobbers BUF. */
static void read_inode(uint32_t ino, ufs_inode_t *out) {
  /* Inode N byte offset within UFS: iblkno_frag*512 + N*128.
   * Find the enclosing 4 KB block and read it. */
  uint32_t byte_in_ufs   = g_iblkno_frag * UFS_FRAG_SIZE + ino * UFS_INODE_SIZE;
  uint32_t block_byte    = byte_in_ufs & ~(UFS_BLOCK_SIZE - 1u);
  uint32_t off_in_block  = byte_in_ufs - block_byte;
  uint32_t block_frag    = block_byte / UFS_FRAG_SIZE;
  read_ufs_block(block_frag, BUF);
  const uint8_t *src = BUF + off_in_block;
  uint8_t *dst = (uint8_t *)out;
  for (uint32_t i = 0; i < UFS_INODE_SIZE; i++) dst[i] = src[i];
}

/* ── Directory lookup ───────────────────────────────────────────────────── */

/* Look up `name` in directory inode `dir`.  Returns the entry's inode
 * number, or 0 if not found.  Only direct[0] is consulted — adequate
 * for the small /boot directory used by our boot path.  Clobbers BUF. */
static uint32_t find_in_dir(const ufs_inode_t *dir, const char *name) {
  uint32_t frag = dir->i_direct[0];
  uint32_t size = dir->i_size_lo;
  if (frag == 0u || size == 0u) return 0u;
  if (size > UFS_BLOCK_SIZE) size = UFS_BLOCK_SIZE;

  read_ufs_block(frag, BUF);

  uint32_t namelen = 0;
  while (name[namelen]) namelen++;

  uint32_t off = 0u;
  while (off + 8u <= size) {
    const ufs_dirent_t *d = (const ufs_dirent_t *)(BUF + off);
    uint16_t reclen = d->d_reclen;
    if (reclen < 8u || reclen > size - off) break;
    if (d->d_ino != 0u && d->d_namlen == namelen) {
      const char *dname = (const char *)(BUF + off + sizeof(ufs_dirent_t));
      uint32_t j = 0;
      while (j < namelen && dname[j] == name[j]) j++;
      if (j == namelen) return d->d_ino;
    }
    off += reclen;
  }
  return 0u;
}

/* ── File load ──────────────────────────────────────────────────────────── */

/* Load file's data blocks to `dest`.  Returns bytes loaded.  Handles
 * up to one level of indirection — enough for the ~120 KB kernel
 * (12 direct × 4 KB = 48 KB + 1024 indirect × 4 KB = 4 MB). */
static uint32_t load_file(const ufs_inode_t *inode, uint8_t *dest) {
  /* Copy fields to locals before any further floppy I/O: the IOCS
   * _B_READ handler uses the supervisor stack heavily and can clobber
   * the caller's inode struct if it shares the stack. */
  uint32_t size     = inode->i_size_lo;
  uint32_t indirect = inode->i_ib[0];
  uint32_t direct[UFS_DIRECT_BLOCKS];
  for (uint32_t i = 0; i < UFS_DIRECT_BLOCKS; i++)
    direct[i] = inode->i_direct[i];

  uint32_t remaining = size;
  uint32_t off       = 0u;

  for (uint32_t i = 0; i < UFS_DIRECT_BLOCKS && remaining > 0u; i++) {
    if (direct[i] == 0u) break;
    read_ufs_block(direct[i], dest + off);
    uint32_t chunk = remaining < UFS_BLOCK_SIZE ? remaining : UFS_BLOCK_SIZE;
    off += chunk;
    remaining -= chunk;
  }

  if (remaining > 0u && indirect != 0u) {
    read_ufs_block(indirect, BUF);
    const uint32_t *ptrs = (const uint32_t *)BUF;
    uint32_t nptrs = UFS_BLOCK_SIZE / sizeof(uint32_t);
    for (uint32_t i = 0; i < nptrs && remaining > 0u; i++) {
      if (ptrs[i] == 0u) break;
      read_ufs_block(ptrs[i], dest + off);
      uint32_t chunk = remaining < UFS_BLOCK_SIZE ? remaining : UFS_BLOCK_SIZE;
      off += chunk;
      remaining -= chunk;
    }
  }

  return size;
}

/* ── Vector copy and kernel launch ──────────────────────────────────────── */

static void __attribute__((noreturn)) stage2_final(void) {
  /* Mask all interrupts before overwriting the vector table */
  asm volatile("or.w #0x0700, %%sr" ::: "memory");

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
  volatile uint32_t *src = (volatile uint32_t *)KERNEL_LOAD_ADDR;
  volatile uint32_t *dst = (volatile uint32_t *)0x0u;

  /* Save IPL ROM handlers BEFORE the copy overwrites them.  IOCS depends
   * on autovectors (24-31), MFP vectored interrupts (64-79), the DMAC
   * channel NIV/EIV window (80-127), and TRAP #15 (47) — preserve all
   * of those so floppy / keyboard / timers keep working in the kernel. */
  uint32_t iocs_handler = *STAGE2_IOCS_SAVE;
  uint32_t saved_auto[8];
  uint32_t saved_mfp[16];
  uint32_t saved_ext[48];
  for (uint32_t i = 0; i < 8u; i++)  saved_auto[i] = dst[24u + i];
  for (uint32_t i = 0; i < 16u; i++) saved_mfp[i]  = dst[64u + i];
  for (uint32_t i = 0; i < 48u; i++) saved_ext[i]  = dst[80u + i];

  for (uint32_t i = 0u; i < 256u; i++) dst[i] = src[i];

  dst[47] = iocs_handler;
  for (uint32_t i = 0; i < 8u; i++)  dst[24u + i] = saved_auto[i];
  for (uint32_t i = 0; i < 16u; i++) dst[64u + i] = saved_mfp[i];
  for (uint32_t i = 0; i < 48u; i++) dst[80u + i] = saved_ext[i];
#pragma GCC diagnostic pop

  asm volatile("jmp 0x006400" ::: "memory");
  __builtin_unreachable();
}

/* ── Diagnostic helper: IOCS _B_PUTC via TRAP #15 ───────────────────────── */

static inline void iocs_putc(char c) {
  register uint32_t d0 asm("d0") = 0x20u;
  register uint32_t d1 asm("d1") = (uint32_t)(uint8_t)c;
  asm volatile("trap #15" : "+r"(d0) : "r"(d1) : "a0", "a1", "memory");
}

/* ── Stage 2 main ───────────────────────────────────────────────────────── */

void stage2_main(void) {
  ufs_inode_t inode;

  /* Read the UFS block containing the superblock.  Magic at byte 1372
   * within the SB; SB itself starts at byte 8192 of the UFS = the
   * second 4 KB block (fragment 16).  NO iocs_putc before the first
   * _B_READ — putc into the IOCS work area can confuse the FDC handler. */
  read_ufs_block(UFS_SB_FRAG, BUF);
  iocs_putc('P');
  iocs_putc('A');

  uint32_t magic;
  {
    const uint32_t *p = (const uint32_t *)(BUF + UFS_FS_MAGIC_OFF);
    magic = *p;
  }
  if (magic != UFS_MAGIC) goto halt;
  {
    const uint32_t *p = (const uint32_t *)(BUF + UFS_FS_IBLKNO_OFF);
    g_iblkno_frag = *p;
  }

  /* Walk / → boot → kernel */
  read_inode(UFS_ROOT_INO, &inode);
  uint32_t boot_ino = find_in_dir(&inode, "boot");
  if (boot_ino == 0u) goto halt;

  read_inode(boot_ino, &inode);
  uint32_t kernel_ino = find_in_dir(&inode, "kernel");
  if (kernel_ino == 0u) goto halt;

  read_inode(kernel_ino, &inode);
  load_file(&inode, (uint8_t *)KERNEL_LOAD_ADDR);

  /* Handoff: stage2 ran to completion.  No rootfs address/size — kernel
   * mounts the same floppy UFS live via iocs_blk. */
  *STAGE2_MAGIC = STAGE2_RAMD_MAGIC;

  stage2_final();

halt:
  for (;;) asm volatile("stop #0x2700" ::: "memory");
}
