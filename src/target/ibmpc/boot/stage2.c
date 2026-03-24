/*
 * stage2.c — PPAP IBM PC Stage 2: UFS kernel loader
 *
 * Runs at 0x0800 after stage1.  Reads the UFS filesystem on the floppy
 * (sector 9+), finds /boot/kernel, loads it to segment 0x1000 (linear
 * 0x10000), copies it down to 0x0600 (linked address), and jumps there.
 */

#include <stdint.h>

/* ── PC 1.44 MB floppy geometry ──────────────────────────────────────────── */

#define FLOPPY_SEC       512u
#define SECS_PER_TRACK   18u
#define NUM_HEADS        2u
#define SECS_PER_CYL     (SECS_PER_TRACK * NUM_HEADS)

#define UFS_FLOPPY_BASE  9u
#define UFS_BLOCK_SIZE   4096u
#define UFS_FLOPPY_SECS  (UFS_BLOCK_SIZE / FLOPPY_SEC)

/* Scratch buffer for UFS metadata reads (superblock, inodes, dirs) */
#define BUF ((uint8_t *)0x1800u)
/* Scratch buffer for indirect block */
#define IBUF ((uint8_t *)0x2800u)

/* Kernel linked at 0x0600, loaded to segment 0x1000 (linear 0x10000) */
#define KERNEL_LINK      0x0600u
#define KERNEL_LOAD_SEG  0x1000u

/* ── UFS structures ──────────────────────────────────────────────────────── */

#define UFS_MAGIC            0x55465331u
#define UFS_INODE_SIZE       64u
#define UFS_INODES_PER_BLOCK (UFS_BLOCK_SIZE / UFS_INODE_SIZE)
#define UFS_DIRENT_SIZE      32u
#define UFS_DIRENTS_PER_BLOCK (UFS_BLOCK_SIZE / UFS_DIRENT_SIZE)
#define UFS_NAME_MAX         27u
#define UFS_ROOT_INO         1u
#define UFS_DIRECT_BLOCKS    10u

typedef struct {
  uint32_t s_magic, s_block_size, s_block_count, s_inode_count;
  uint32_t s_free_blocks, s_free_inodes;
  uint32_t s_bmap_block, s_imap_block, s_itable_block, s_data_block;
  uint32_t s_inode_blocks;
  uint8_t  s_pad[84];
} ufs_super_t;

typedef struct {
  uint16_t i_mode, i_nlink, i_uid, i_gid;
  uint32_t i_size, i_mtime, i_ctime;
  uint32_t i_direct[UFS_DIRECT_BLOCKS];
  uint32_t i_indirect;
} ufs_inode_t;

typedef struct {
  uint32_t d_ino;
  char     d_name[UFS_NAME_MAX + 1];
} ufs_dirent_t;

static uint32_t sb_itable_block;
extern uint8_t boot_drive;

/* ── BIOS output ─────────────────────────────────────────────────────────── */

static void putc_bios(char c)
{
  __asm__ volatile ("int $0x10" : : "a"((uint16_t)(0x0E00 | (uint8_t)c)), "b"((uint16_t)0));
}

static void puts_bios(const char *s)
{
  while (*s) putc_bios(*s++);
}

/* ── Disk I/O ────────────────────────────────────────────────────────────── */

/* Read one 512-byte sector to ES:BX via INT 13h. ES must be set by caller. */
static void read_sector_esbx(uint32_t lba, uint16_t bx)
{
  uint16_t cyl = (uint16_t)(lba / SECS_PER_CYL);
  uint16_t rem = (uint16_t)(lba % SECS_PER_CYL);
  uint8_t  head = (uint8_t)(rem / SECS_PER_TRACK);
  uint8_t  sec  = (uint8_t)(rem % SECS_PER_TRACK) + 1;
  uint16_t cx = (cyl << 8) | sec;
  uint16_t dx = ((uint16_t)head << 8) | boot_drive;

  __asm__ volatile (
    "movw $0x0201, %%ax\n\t"
    "int  $0x13"
    :
    : "b"(bx), "c"(cx), "d"(dx)
    : "ax", "memory", "cc"
  );
}

/* Read one sector to DS:dest (sets ES=0 internally) */
static void read_sector(uint32_t lba, void *dest)
{
  uint16_t zero = 0;
  __asm__ volatile ("push %%es; mov %0, %%es" : : "r"(zero));
  read_sector_esbx(lba, (uint16_t)(uintptr_t)dest);
  __asm__ volatile ("pop %es");
}

/* Read one UFS block (8 sectors) to DS:dest */
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
    if (d->d_ino == 0) continue;
    uint16_t j = 0;
    while (j < UFS_NAME_MAX && d->d_name[j] == name[j] && name[j]) j++;
    if (d->d_name[j] == name[j]) return d->d_ino;
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
  for (uint16_t i = 0; i < UFS_INODE_SIZE; i++) dst[i] = src[i];
}

/* ── Kernel loader (loads to segment 0x1000:offset via ES) ───────────────── */

/* Read one UFS block to ES:dest_off, advancing dest_off. */
static void load_block_far(uint32_t blk_no, uint16_t seg,
                           uint16_t *off, uint32_t *loaded, uint32_t size)
{
  if (blk_no == 0) return;
  uint32_t lba = UFS_FLOPPY_BASE + blk_no * UFS_FLOPPY_SECS;
  uint16_t cyl, rem, cx, dx;
  for (uint16_t s = 0; s < UFS_FLOPPY_SECS && *loaded < size; s++) {
    uint32_t sector = lba + s;
    cyl = (uint16_t)(sector / SECS_PER_CYL);
    rem = (uint16_t)(sector % SECS_PER_CYL);
    cx = (cyl << 8) | ((uint8_t)(rem % SECS_PER_TRACK) + 1);
    dx = ((uint16_t)(uint8_t)(rem / SECS_PER_TRACK) << 8) | boot_drive;
    /* Read to seg:*off via INT 13h */
    __asm__ volatile (
      "push %%es\n\t"
      "mov  %4, %%es\n\t"
      "mov  $0x0201, %%ax\n\t"
      "int  $0x13\n\t"
      "pop  %%es"
      :
      : "b"(*off), "c"(cx), "d"(dx), "m"(seg), "r"(seg)
      : "ax", "memory", "cc"
    );
    *off += FLOPPY_SEC;
    *loaded += FLOPPY_SEC;
  }
}

/* ── Main ────────────────────────────────────────────────────────────────── */

void stage2_main(void)
{
  puts_bios("\r\nStage2: reading UFS...\r\n");

  read_ufs_block(0, BUF);
  ufs_super_t *sb = (ufs_super_t *)BUF;
  if (sb->s_magic != UFS_MAGIC) { puts_bios("Bad UFS magic!\r\n"); return; }
  sb_itable_block = sb->s_itable_block;

  ufs_inode_t inode;
  read_inode(UFS_ROOT_INO, &inode);
  read_ufs_block(inode.i_direct[0], BUF);
  uint32_t boot_ino = find_in_dir(BUF, "boot");
  if (!boot_ino) { puts_bios("boot/ not found\r\n"); return; }

  read_inode(boot_ino, &inode);
  read_ufs_block(inode.i_direct[0], BUF);
  uint32_t kernel_ino = find_in_dir(BUF, "kernel");
  if (!kernel_ino) { puts_bios("boot/kernel not found\r\n"); return; }

  read_inode(kernel_ino, &inode);
  puts_bios("Loading kernel...\r\n");

  /* Copy inode fields locally before reads clobber BUF */
  uint32_t size = inode.i_size;
  uint32_t direct[UFS_DIRECT_BLOCKS];
  for (uint16_t i = 0; i < UFS_DIRECT_BLOCKS; i++)
    direct[i] = inode.i_direct[i];
  uint32_t indirect = inode.i_indirect;

  /* Load to 0x1000:0x0000 (linear 0x10000) via far reads */
  uint16_t dest_off = 0;
  uint32_t loaded = 0;

  for (uint16_t i = 0; i < UFS_DIRECT_BLOCKS && loaded < size; i++)
    load_block_far(direct[i], KERNEL_LOAD_SEG, &dest_off, &loaded, size);

  if (indirect && loaded < size) {
    read_ufs_block(indirect, IBUF);
    uint32_t *ind = (uint32_t *)IBUF;
    for (uint16_t i = 0; i < UFS_BLOCK_SIZE / 4 && loaded < size; i++)
      load_block_far(ind[i], KERNEL_LOAD_SEG, &dest_off, &loaded, size);
  }

  puts_bios("Relocating...\r\n");

  /* Copy from 0x1000:0x0000 → 0x0000:0x0600 then jump */
  __asm__ volatile (
    "cli\n\t"
    "pushw %%ds\n\t"
    "movw  $0x1000, %%ax\n\t"
    "movw  %%ax, %%ds\n\t"
    "xorw  %%ax, %%ax\n\t"
    "movw  %%ax, %%es\n\t"
    "xorw  %%si, %%si\n\t"
    "movw  $0x0600, %%di\n\t"
    "movw  $0xB800, %%cx\n\t"
    "cld\n\t"
    "rep   movsb\n\t"
    "popw  %%ds\n\t"
    "ljmp  $0x0000, $0x0600"
    ::: "ax", "cx", "si", "di", "memory"
  );
}
