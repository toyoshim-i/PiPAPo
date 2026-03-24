/*
 * stage2.c — PPAP IBM PC Stage 2: UFS kernel loader
 *
 * Stage2 runs at 0x8000 (loaded there by stage1).  Reads the UFS
 * filesystem on the floppy (sector 9+), finds /boot/kernel, loads it
 * directly to 0x0600 (its linked address), and jumps there.
 *
 * Memory layout while stage2 runs:
 *   0x0600-0x????  Kernel load area (directly at linked address)
 *   0x7C00         Stack (grows down)
 *   0x8000-0x8FFF  Stage2 code (4 KB)
 *   0x9000-0x9FFF  BUF — UFS metadata scratch (4 KB)
 *   0xA000-0xAFFF  IBUF — indirect block scratch (4 KB)
 *
 * No relocation needed — kernel is loaded to its final address.
 */

#include <stdint.h>

#define FLOPPY_SEC       512u
#define SECS_PER_TRACK   18u
#define NUM_HEADS        2u
#define SECS_PER_CYL     (SECS_PER_TRACK * NUM_HEADS)

#define UFS_FLOPPY_BASE  9u
#define UFS_BLOCK_SIZE   4096u
#define UFS_FLOPPY_SECS  (UFS_BLOCK_SIZE / FLOPPY_SEC)

#define BUF  ((uint8_t *)0x9000u)
#define IBUF ((uint8_t *)0xA000u)

#define KERNEL_ADDR 0x0600u

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

static uint16_t sb_itable_block;
extern uint8_t boot_drive;

static void putc_bios(char c)
{
  __asm__ volatile ("int $0x10" : : "a"((uint16_t)(0x0E00|(uint8_t)c)), "b"((uint16_t)0));
}

static void puts_bios(const char *s)
{
  while (*s) putc_bios(*s++);
}

/* Read one sector to 0:dest. Push/pop ES in one asm block. */
static void __attribute__((noinline)) read_sector(uint16_t lba, void *dest)
{
  uint16_t cyl  = lba / SECS_PER_CYL;
  uint16_t rem  = lba % SECS_PER_CYL;
  uint16_t head = rem / SECS_PER_TRACK;
  uint16_t sec  = rem % SECS_PER_TRACK + 1;

  __asm__ volatile (
    "push %%es\n\t"
    "xor  %%ax, %%ax\n\t"
    "mov  %%ax, %%es\n\t"
    "mov  $0x0201, %%ax\n\t"
    "int  $0x13\n\t"
    "pop  %%es"
    :
    : "b"((uint16_t)(uintptr_t)dest),
      "c"((uint16_t)((cyl << 8) | sec)),
      "d"((uint16_t)((head << 8) | boot_drive))
    : "ax", "memory", "cc"
  );
}

static void __attribute__((noinline)) read_ufs_block(uint16_t blk, void *dest)
{
  uint16_t lba = UFS_FLOPPY_BASE + blk * UFS_FLOPPY_SECS;
  uint8_t *p = (uint8_t *)dest;
  for (uint16_t i = 0; i < UFS_FLOPPY_SECS; i++) {
    read_sector(lba + i, p);
    p += FLOPPY_SEC;
  }
}

static uint16_t find_in_dir(const uint8_t *blk, const char *name)
{
  const ufs_dirent_t *d = (const ufs_dirent_t *)blk;
  for (uint16_t i = 0; i < UFS_DIRENTS_PER_BLOCK; i++, d++) {
    if (d->d_ino == 0) continue;
    uint16_t j = 0;
    while (j < UFS_NAME_MAX && d->d_name[j] == name[j] && name[j]) j++;
    if (d->d_name[j] == name[j]) return (uint16_t)d->d_ino;
  }
  return 0;
}

static void read_inode(uint16_t ino, ufs_inode_t *out)
{
  uint16_t blk = sb_itable_block + ino / UFS_INODES_PER_BLOCK;
  read_ufs_block(blk, BUF);
  uint16_t off = (ino % UFS_INODES_PER_BLOCK) * UFS_INODE_SIZE;
  uint8_t *s = BUF + off, *d = (uint8_t *)out;
  for (uint16_t i = 0; i < UFS_INODE_SIZE; i++) d[i] = s[i];
}

static void __attribute__((noinline)) load_block(uint16_t blk,
    uint8_t **dest, uint32_t *loaded, uint32_t size)
{
  if (blk == 0) return;
  uint16_t lba = UFS_FLOPPY_BASE + blk * UFS_FLOPPY_SECS;
  for (uint16_t s = 0; s < UFS_FLOPPY_SECS && *loaded < size; s++) {
    read_sector(lba + s, *dest);
    *dest += FLOPPY_SEC;
    *loaded += FLOPPY_SEC;
  }
}

void stage2_main(void)
{
  puts_bios("\r\nStage2: reading UFS...\r\n");

  read_ufs_block(0, BUF);
  ufs_super_t *sb = (ufs_super_t *)BUF;
  if (sb->s_magic != UFS_MAGIC) { puts_bios("Bad UFS magic!\r\n"); return; }
  sb_itable_block = (uint16_t)sb->s_itable_block;

  ufs_inode_t inode;
  read_inode(UFS_ROOT_INO, &inode);
  read_ufs_block((uint16_t)inode.i_direct[0], BUF);
  uint16_t boot_ino = find_in_dir(BUF, "boot");
  if (!boot_ino) { puts_bios("boot/ not found\r\n"); return; }

  read_inode(boot_ino, &inode);
  read_ufs_block((uint16_t)inode.i_direct[0], BUF);
  uint16_t kernel_ino = find_in_dir(BUF, "kernel");
  if (!kernel_ino) { puts_bios("boot/kernel not found\r\n"); return; }

  read_inode(kernel_ino, &inode);
  puts_bios("Loading kernel...\r\n");

  uint32_t size = inode.i_size;
  uint16_t direct[UFS_DIRECT_BLOCKS];
  for (uint16_t i = 0; i < UFS_DIRECT_BLOCKS; i++)
    direct[i] = (uint16_t)inode.i_direct[i];
  uint16_t indirect = (uint16_t)inode.i_indirect;

  uint8_t *dest = (uint8_t *)KERNEL_ADDR;
  uint32_t loaded = 0;

  for (uint16_t i = 0; i < UFS_DIRECT_BLOCKS && loaded < size; i++)
    load_block(direct[i], &dest, &loaded, size);

  if (indirect && loaded < size) {
    read_ufs_block(indirect, IBUF);
    uint32_t *ind = (uint32_t *)IBUF;
    for (uint16_t i = 0; i < UFS_BLOCK_SIZE / 4 && loaded < size; i++)
      load_block((uint16_t)ind[i], &dest, &loaded, size);
  }

  puts_bios("Jumping to kernel\r\n");
  __asm__ volatile ("cli\n\t" "ljmp $0x0000, $0x0600");
}
