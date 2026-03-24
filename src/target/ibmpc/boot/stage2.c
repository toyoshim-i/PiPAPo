/*
 * stage2.c — PPAP IBM PC Stage 2: UFS kernel loader
 *
 * Runs at 0x0800 after stage1.  Reads the UFS filesystem stored on the
 * floppy from sector 9 onward, finds /boot/kernel, loads it to 0x10000
 * (segment 0x1000), and far-jumps to 0x1000:0x0000.
 *
 * Adapted from the X68000 stage2.c but uses BIOS INT 13h for disk I/O
 * and PC 1.44 MB floppy geometry (512 bytes/sector, 18 sec/track, 2 heads).
 *
 * UFS block layout (block size = 4096 bytes = 8 × 512-byte sectors):
 *   UFS block N = floppy sector UFS_FLOPPY_BASE + N*8 … +N*8+7
 */

#include <stdint.h>

/* ── PC 1.44 MB floppy geometry ──────────────────────────────────────────── */

#define FLOPPY_SEC       512u    /* bytes per sector */
#define SECS_PER_TRACK   18u
#define NUM_HEADS        2u
#define SECS_PER_CYL     (SECS_PER_TRACK * NUM_HEADS)  /* 36 */

/* Stage2 occupies sectors 1-8 (4 KB).  UFS starts at sector 9. */
#define UFS_FLOPPY_BASE  9u
#define UFS_BLOCK_SIZE   4096u
#define UFS_FLOPPY_SECS  (UFS_BLOCK_SIZE / FLOPPY_SEC)  /* 8 */

/* Kernel is loaded at linear 0x3000 (segment 0, offset 0x3000).
 * Above stage2 code (0x0800-0x17FF) and scratch buffer (0x1800-0x27FF). */
#define KERNEL_LOAD      0x3000u

/* Scratch buffer for one UFS block (4 KB) — above stage2 code at 0x0800.
 * Stage2 code+data is ≤4 KB (0x0800-0x17FF).  Buffer at 0x1800. */
#define BUF ((uint8_t *)0x1800u)

/* ── UFS on-disk structures (must match mkufs.c / ufs_format.h) ────────── */

#define UFS_MAGIC            0x55465331u
#define UFS_INODE_SIZE       64u
#define UFS_INODES_PER_BLOCK (UFS_BLOCK_SIZE / UFS_INODE_SIZE)
#define UFS_DIRENT_SIZE      32u
#define UFS_DIRENTS_PER_BLOCK (UFS_BLOCK_SIZE / UFS_DIRENT_SIZE)
#define UFS_NAME_MAX         27u
#define UFS_ROOT_INO         1u
#define UFS_DIRECT_BLOCKS    10u

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

/* ── Saved superblock fields (avoid re-reading) ─────────────────────────── */

static uint32_t sb_itable_block;

/* ── Boot drive (from stage2_entry.S) ────────────────────────────────────── */

extern uint8_t boot_drive;

/* ── BIOS screen output ──────────────────────────────────────────────────── */

static void putc_bios(char c)
{
  __asm__ volatile (
    "int $0x10"
    : : "a"((uint16_t)(0x0E00 | (uint8_t)c)), "b"((uint16_t)0)
  );
}

static void puts_bios(const char *s)
{
  while (*s)
    putc_bios(*s++);
}

/* ── Disk I/O via BIOS INT 13h ───────────────────────────────────────────── */

static void read_sector(uint32_t lba, void *dest)
{
  uint16_t cyl   = (uint16_t)(lba / SECS_PER_CYL);
  uint16_t rem   = (uint16_t)(lba % SECS_PER_CYL);
  uint8_t  head  = (uint8_t)(rem / SECS_PER_TRACK);
  uint8_t  sec   = (uint8_t)(rem % SECS_PER_TRACK) + 1;  /* 1-based */

  uint16_t cx = (cyl << 8) | sec;
  uint16_t dx = ((uint16_t)head << 8) | boot_drive;

  /* INT 13h reads to ES:BX.  Ensure ES=0 for flat addressing. */
  __asm__ volatile (
    "pushw %%es\n\t"
    "xorw  %%ax, %%ax\n\t"
    "movw  %%ax, %%es\n\t"
    "movw  %0, %%ax\n\t"
    "int   $0x13\n\t"
    "popw  %%es"
    :
    : "g"((uint16_t)0x0201),     /* AH=02 read, AL=1 sector */
      "b"(dest),                  /* ES:BX = buffer (ES set to 0 above) */
      "c"(cx),                    /* CH=cyl, CL=sec */
      "d"(dx)                     /* DH=head, DL=drive */
    : "memory", "cc", "ax"
  );
}

/* ── UFS block read (8 × 512-byte sectors) ───────────────────────────────── */

static void read_ufs_block(uint32_t block_no, void *dest)
{
  uint32_t lba = UFS_FLOPPY_BASE + block_no * UFS_FLOPPY_SECS;
  uint8_t *p = (uint8_t *)dest;
  for (uint16_t i = 0; i < UFS_FLOPPY_SECS; i++) {
    read_sector(lba + i, p);
    p += FLOPPY_SEC;
  }
}

/* ── UFS helpers ─────────────────────────────────────────────────────────── */

static uint32_t find_in_dir(const uint8_t *dirblock, const char *name)
{
  const ufs_dirent_t *d = (const ufs_dirent_t *)dirblock;
  for (uint16_t i = 0; i < UFS_DIRENTS_PER_BLOCK; i++, d++) {
    if (d->d_ino == 0)
      continue;
    uint16_t j = 0;
    while (j < UFS_NAME_MAX && d->d_name[j] == name[j] && name[j])
      j++;
    if (d->d_name[j] == name[j])
      return d->d_ino;
  }
  return 0;
}

static void read_inode(uint32_t ino, ufs_inode_t *out)
{
  uint32_t blk = sb_itable_block + ino / UFS_INODES_PER_BLOCK;
  read_ufs_block(blk, BUF);
  uint16_t off = (uint16_t)((ino % UFS_INODES_PER_BLOCK) * UFS_INODE_SIZE);
  uint8_t *src = BUF + off;
  uint8_t *dst = (uint8_t *)out;
  for (uint16_t i = 0; i < UFS_INODE_SIZE; i++)
    dst[i] = src[i];
}

/* Load file data blocks to a near destination (DS:dest).
 * Returns bytes loaded. */
static uint32_t load_file(const ufs_inode_t *inode, uint8_t *dest)
{
  /* Copy inode fields locally before any disk reads clobber BUF */
  uint32_t size = inode->i_size;
  uint32_t direct[UFS_DIRECT_BLOCKS];
  for (uint16_t i = 0; i < UFS_DIRECT_BLOCKS; i++)
    direct[i] = inode->i_direct[i];

  uint32_t loaded = 0;

  for (uint16_t i = 0; i < UFS_DIRECT_BLOCKS && loaded < size; i++) {
    if (direct[i] == 0)
      continue;

    uint32_t lba = UFS_FLOPPY_BASE + direct[i] * UFS_FLOPPY_SECS;
    for (uint16_t s = 0; s < UFS_FLOPPY_SECS && loaded < size; s++) {
      read_sector(lba + s, dest);
      dest += FLOPPY_SEC;
      loaded += FLOPPY_SEC;
    }
  }

  return (loaded < size) ? loaded : size;
}

/* ── Main entry ──────────────────────────────────────────────────────────── */

void stage2_main(void)
{
  puts_bios("\r\nStage2: reading UFS...\r\n");

  /* Read superblock (UFS block 0) */
  read_ufs_block(0, BUF);
  ufs_super_t *sb = (ufs_super_t *)BUF;

  if (sb->s_magic != UFS_MAGIC) {
    puts_bios("Bad UFS magic!\r\n");
    return;
  }

  sb_itable_block = sb->s_itable_block;

  /* Find /boot directory */
  ufs_inode_t inode;
  read_inode(UFS_ROOT_INO, &inode);

  /* Read root directory data block */
  read_ufs_block(inode.i_direct[0], BUF);
  uint32_t boot_ino = find_in_dir(BUF, "boot");
  if (boot_ino == 0) {
    puts_bios("boot/ not found\r\n");
    return;
  }

  /* Find kernel in /boot */
  read_inode(boot_ino, &inode);
  read_ufs_block(inode.i_direct[0], BUF);
  uint32_t kernel_ino = find_in_dir(BUF, "kernel");
  if (kernel_ino == 0) {
    puts_bios("boot/kernel not found\r\n");
    return;
  }

  /* Load kernel to 0x0000:0x2000 (linear 0x2000) */
  read_inode(kernel_ino, &inode);
  puts_bios("Loading kernel...\r\n");
  uint32_t ksize = load_file(&inode, (uint8_t *)KERNEL_LOAD);
  (void)ksize;

  puts_bios("Jumping to kernel\r\n");

  /* Jump to kernel entry (near jump, segment 0).
   * Use ljmp with CS=0 to ensure CS is set correctly for the kernel. */
  __asm__ volatile (
    "cli\n\t"
    "ljmp $0x0000, $0x3000"
  );
}
