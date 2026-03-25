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

/* Stage2 at 0xC000 (up to 4 KB), BUF/IBUF above it */
#define BUF  ((uint8_t *)0xD000u)
#define IBUF ((uint8_t *)0xE000u)

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

/* Read one sector to seg:off via BIOS INT 13h. */
static void __attribute__((noinline)) read_sector_far(uint16_t lba,
    uint16_t seg, uint16_t off)
{
  uint16_t cyl  = lba / SECS_PER_CYL;
  uint16_t rem  = lba % SECS_PER_CYL;
  uint16_t head = rem / SECS_PER_TRACK;
  uint16_t sec  = rem % SECS_PER_TRACK + 1;

  __asm__ volatile (
    "push %%es\n\t"
    "mov  %0, %%es\n\t"
    "mov  $0x0201, %%ax\n\t"
    "int  $0x13\n\t"
    "pop  %%es"
    :
    : "r"(seg),
      "b"(off),
      "c"((uint16_t)((cyl << 8) | sec)),
      "d"((uint16_t)((head << 8) | boot_drive))
    : "ax", "memory", "cc"
  );
}

/* Read one sector to 0:dest (near pointer, segment 0). */
static void __attribute__((noinline)) read_sector(uint16_t lba, void *dest)
{
  read_sector_far(lba, 0, (uint16_t)(uintptr_t)dest);
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

static void __attribute__((noinline)) load_block_far(uint16_t blk,
    uint16_t seg, uint16_t *off, uint32_t *loaded, uint32_t size)
{
  if (blk == 0) return;
  uint16_t lba = UFS_FLOPPY_BASE + blk * UFS_FLOPPY_SECS;
  for (uint16_t s = 0; s < UFS_FLOPPY_SECS && *loaded < size; s++) {
    read_sector_far(lba + s, seg, *off);
    *off += FLOPPY_SEC;
    *loaded += FLOPPY_SEC;
  }
}

static void __attribute__((noinline)) load_block(uint16_t blk,
    uint8_t **dest, uint32_t *loaded, uint32_t size)
{
  uint16_t off = (uint16_t)(uintptr_t)*dest;
  load_block_far(blk, 0, &off, loaded, size);
  *dest = (uint8_t *)(uintptr_t)off;
}

/*
 * Load a file from UFS to seg:0000.
 * Returns the number of bytes loaded, or 0 on failure.
 */
static uint16_t load_file_far(uint16_t ino, uint16_t seg)
{
  ufs_inode_t inode;
  read_inode(ino, &inode);

  uint32_t size = inode.i_size;
  uint16_t direct[UFS_DIRECT_BLOCKS];
  for (uint16_t i = 0; i < UFS_DIRECT_BLOCKS; i++)
    direct[i] = (uint16_t)inode.i_direct[i];
  uint16_t indirect = (uint16_t)inode.i_indirect;

  uint16_t off = 0;
  uint32_t loaded = 0;
  for (uint16_t i = 0; i < UFS_DIRECT_BLOCKS && loaded < size; i++)
    load_block_far(direct[i], seg, &off, &loaded, size);

  if (indirect && loaded < size) {
    read_ufs_block(indirect, IBUF);
    uint32_t *ind = (uint32_t *)IBUF;
    for (uint16_t i = 0; i < UFS_BLOCK_SIZE / 4 && loaded < size; i++)
      load_block_far((uint16_t)ind[i], seg, &off, &loaded, size);
  }

  return (uint16_t)loaded;
}

/* Load a file to 0:dest (near, segment 0). */
static uint16_t load_file(uint16_t ino, uint8_t *dest)
{
  ufs_inode_t inode;
  read_inode(ino, &inode);

  uint32_t size = inode.i_size;
  uint16_t direct[UFS_DIRECT_BLOCKS];
  for (uint16_t i = 0; i < UFS_DIRECT_BLOCKS; i++)
    direct[i] = (uint16_t)inode.i_direct[i];
  uint16_t indirect = (uint16_t)inode.i_indirect;

  uint32_t loaded = 0;
  for (uint16_t i = 0; i < UFS_DIRECT_BLOCKS && loaded < size; i++)
    load_block(direct[i], &dest, &loaded, size);

  if (indirect && loaded < size) {
    read_ufs_block(indirect, IBUF);
    uint32_t *ind = (uint32_t *)IBUF;
    for (uint16_t i = 0; i < UFS_BLOCK_SIZE / 4 && loaded < size; i++)
      load_block((uint16_t)ind[i], &dest, &loaded, size);
  }

  return (uint16_t)loaded;
}

/*
 * Find a file in a directory by name.
 * Returns the inode number, or 0 if not found.
 * Searches all direct blocks of the directory.
 */
static uint16_t find_file(uint16_t dir_ino, const char *name)
{
  ufs_inode_t inode;
  read_inode(dir_ino, &inode);
  for (uint16_t i = 0; i < UFS_DIRECT_BLOCKS; i++) {
    if (!inode.i_direct[i]) break;
    read_ufs_block((uint16_t)inode.i_direct[i], BUF);
    uint16_t ino = find_in_dir(BUF, name);
    if (ino) return ino;
  }
  return 0;
}

/*
 * Module info block: passed to the kernel at a known address.
 * The kernel reads this to find where each module was loaded
 * and initialize the segment manager.
 */
#define MOD_INFO_ADDR  0x0500u  /* In the free gap 0x0500-0x05FF */
#define MOD_MAX        4u

typedef struct {
  uint16_t count;              /* number of loaded modules */
  struct {
    uint16_t segment;          /* paragraph address (linear >> 4) */
    uint16_t size;             /* size in bytes */
  } mod[MOD_MAX];
} mod_info_t;

void stage2_main(void)
{
  /* Banner continues: "PiPA" already on screen from stage1+stage2_entry */

  read_ufs_block(0, BUF);
  ufs_super_t *sb = (ufs_super_t *)BUF;
  if (sb->s_magic != UFS_MAGIC) { puts_bios("!UFS"); return; }
  sb_itable_block = (uint16_t)sb->s_itable_block;

  /* Find /boot directory */
  uint16_t boot_ino = find_file(UFS_ROOT_INO, "boot");
  if (!boot_ino) { puts_bios("!boot"); return; }

  /* Load /boot/kernel (core module) to 0x0600 */
  uint16_t kernel_ino = find_file(boot_ino, "kernel");
  if (!kernel_ino) { puts_bios("!kern"); return; }

  uint16_t core_size = load_file(kernel_ino, (uint8_t *)KERNEL_ADDR);
  if (!core_size) { puts_bios("!load"); return; }

  /* VFS code module: load at segment 0x1000 (linear 0x10000).
   * Must be above core's full footprint (text+data+BSS+stack+page_pool).
   * Core ends at __page_pool_start = ~0xC000, so 0x10000 is safe. */
  uint16_t vfs_seg = 0x1000u;
  uint16_t vfs_ino = find_file(boot_ino, "kernel_vfs");
  uint16_t vfs_size = 0;
  if (vfs_ino) {
    vfs_size = load_file_far(vfs_ino, vfs_seg);
  }

  /* VFS data: zero region then load .rodata+.data to DS:0xA000.
   * Zeroing first ensures BSS (after loaded data) is clean. */
  {
    uint8_t *p = (uint8_t *)0xA000u;
    for (uint16_t i = 0; i < 8192u; i++) p[i] = 0;
  }
  uint16_t vfs_data_ino = find_file(boot_ino, "kernel_vfs_data");
  if (vfs_data_ino) {
    load_file(vfs_data_ino, (uint8_t *)0xA000u);
  }

  /* Write module info block for the kernel to read */
  mod_info_t *info = (mod_info_t *)MOD_INFO_ADDR;
  info->count = vfs_size ? 2 : 1;
  /* Module 0: core (segment = KERNEL_ADDR >> 4 = 0x0060) */
  info->mod[0].segment = KERNEL_ADDR >> 4;
  info->mod[0].size = core_size;
  /* Module 1: VFS */
  if (vfs_size) {
    info->mod[1].segment = vfs_seg;
    info->mod[1].size = vfs_size;
  }

  /* Kernel continues the "PiPA" banner with "Po booting..." */
  __asm__ volatile ("cli\n\t" "ljmp $0x0000, $0x0600");
}
