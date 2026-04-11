/*
 * stage2.c — PPAP IBM PC Stage 2: UFS kernel loader
 *
 * Stage2 runs at 0xC000 (loaded there by stage1).  Detects whether
 * the boot device is a floppy (DL < 0x80) or hard disk (DL >= 0x80),
 * queries disk geometry accordingly, finds the UFS partition, loads
 * /boot/kernel, and jumps there.
 *
 * Memory layout while stage2 runs:
 *   0x0600-0x????  Kernel load area (directly at linked address)
 *   0x7C00         Stack (grows down)
 *   0xC000-0xCFFF  Stage2 code (4 KB)
 *   0xD000-0xDFFF  BUF — UFS metadata scratch (4 KB)
 *   0xE000-0xEFFF  IBUF — indirect block scratch (4 KB)
 *
 * No relocation needed — kernel is loaded to its final address.
 */

#include <stdint.h>

#define FLOPPY_SEC       512u
#define UFS_BLOCK_SIZE   4096u
#define UFS_SECS_PER_BLK (UFS_BLOCK_SIZE / FLOPPY_SEC)

/* Disk geometry — set at boot from known floppy values or INT 13h query */
static uint16_t secs_per_track;
static uint16_t num_heads;
static uint16_t ufs_base_sector;  /* absolute LBA of first UFS sector */

/* Stage2 at 0xC000 (up to 4 KB), BUF/IBUF above it */
#define BUF  ((uint8_t *)0xD000u)
#define IBUF ((uint8_t *)0xE000u)

#define KERNEL_ADDR 0x0600u

#define VFS_DATA_BASE        0xAC00u
#define VFS_DATA_STAGE2_SIZE 0x1400u  /* 0xAC00+0x1400=0xC000: stop before stage2 */

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

/* print_char (in stage2_entry.S): writes to BIOS teletype (VGA) and COM1. */
extern void print_char(void);
static void putc_bios(char c)
{
  /* print_char takes the char in AL and preserves all regs. */
  __asm__ volatile ("call print_char" : : "a"((uint16_t)(uint8_t)c) : "memory");
}

static void puts_bios(const char *s)
{
  while (*s) putc_bios(*s++);
}

/* Read one sector to seg:off via BIOS INT 13h. */
static void __attribute__((noinline)) read_sector_far(uint16_t lba,
    uint16_t seg, uint16_t off)
{
  uint16_t secs_per_cyl = secs_per_track * num_heads;
  uint16_t cyl  = lba / secs_per_cyl;
  uint16_t rem  = lba % secs_per_cyl;
  uint16_t head = rem / secs_per_track;
  uint16_t sec  = rem % secs_per_track + 1;

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
  uint16_t lba = ufs_base_sector + blk * UFS_SECS_PER_BLK;
  uint8_t *p = (uint8_t *)dest;
  for (uint16_t i = 0; i < UFS_SECS_PER_BLK; i++) {
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
  uint16_t lba = ufs_base_sector + blk * UFS_SECS_PER_BLK;
  for (uint16_t s = 0; s < UFS_SECS_PER_BLK && *loaded < size; s++) {
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
  /* Boot device info (read by kernel target_pcxt.c) */
  uint8_t  boot_dev;           /* 0x00 = floppy, 0x80 = HDD */
  uint8_t  reserved;
  uint16_t ufs_base;           /* absolute LBA of first UFS sector */
  uint16_t dev_spt;            /* sectors per track */
  uint16_t dev_heads;          /* number of heads */
  uint32_t dev_sectors;        /* total partition sectors */
} mod_info_t;

/* ── Boot device detection ──────────────────────────────────────────── */

/* Query CHS geometry via BIOS INT 13h AH=08h. */
static void query_disk_geometry(void)
{
  uint16_t cx, dx;
  __asm__ volatile (
    "mov  $0x0800, %%ax\n\t"
    "int  $0x13"
    : "=c"(cx), "=d"(dx)
    : "d"((uint16_t)boot_drive)
    : "ax", "bx", "memory", "cc"
  );
  secs_per_track = cx & 0x3F;       /* CL bits 0-5 */
  num_heads = (dx >> 8) + 1;        /* DH = max head (0-based) */
}

/* MBR partition entry (16 bytes at offset 446 in sector 0). */
typedef struct {
  uint8_t  status;
  uint8_t  chs_first[3];
  uint8_t  type;
  uint8_t  chs_last[3];
  uint32_t lba_start;
  uint32_t lba_size;
} __attribute__((packed)) mbr_part_t;

#define MBR_PART_OFFSET 446u
#define MBR_PART_COUNT  4u
#define MBR_PART_TYPE   0xA9u  /* BSD UFS */
#define MBR_SIG_OFFSET  510u

static uint32_t ufs_partition_sectors;  /* saved from MBR scan */

/* Read sector 0 (MBR), scan for a partition with type MBR_PART_TYPE.
 * Returns the partition's start LBA, or 0 on failure.
 * Also saves the partition size in ufs_partition_sectors. */
static uint16_t find_ufs_partition(void)
{
  read_sector(0, BUF);
  /* Verify MBR signature */
  if (BUF[MBR_SIG_OFFSET] != 0x55 || BUF[MBR_SIG_OFFSET + 1] != 0xAA)
    return 0;
  mbr_part_t *p = (mbr_part_t *)(BUF + MBR_PART_OFFSET);
  for (uint16_t i = 0; i < MBR_PART_COUNT; i++) {
    if (p[i].type == MBR_PART_TYPE) {
      ufs_partition_sectors = p[i].lba_size;
      return (uint16_t)p[i].lba_start;
    }
  }
  return 0;
}

void stage2_main(void)
{
  /* Banner continues: "PiPA" already on screen from stage1+stage2_entry */

  /* Detect boot device and set geometry + UFS base sector */
  if (boot_drive >= 0x80) {
    /* HDD: query geometry and find UFS partition in MBR */
    query_disk_geometry();
    ufs_base_sector = find_ufs_partition();
    if (!ufs_base_sector) { puts_bios("!MBR"); return; }
  } else {
    /* Floppy: known 1.44 MB geometry */
    secs_per_track = 9;
    num_heads = 2;
    ufs_base_sector = 9;
  }

  read_ufs_block(0, BUF);
  ufs_super_t *sb = (ufs_super_t *)BUF;
  if (sb->s_magic != UFS_MAGIC) { puts_bios("!UFS"); return; }
  sb_itable_block = (uint16_t)sb->s_itable_block;

  /* Find /boot directory */
  uint16_t boot_ino = find_file(UFS_ROOT_INO, "boot");
  if (!boot_ino) { puts_bios("!boot"); return; }

  /* Load /boot/kernel to DS:0x0600 (for data access via SS=0) */
  uint16_t kernel_ino = find_file(boot_ino, "kernel");
  if (!kernel_ino) { puts_bios("!kern"); return; }

  uint16_t core_size = load_file(kernel_ino, (uint8_t *)KERNEL_ADDR);
  if (!core_size) { puts_bios("!load"); return; }

  /* Load same kernel binary to a far segment for code execution.
   * Core CS = 0x1000.  Binary starts at file offset 0 = ELF 0x0600.
   * Loading to segment 0x1060 places it at linear 0x10600, so
   * CS:0x0600 = 0x1000:0x0600 = 0x10600 = correct _start address.
   * Near calls to any ELF address X resolve to CS:X = 0x10000+X,
   * which is where that byte was loaded. */
  uint16_t core_cs = 0x1000u;
  uint16_t core_load_seg = core_cs + (KERNEL_ADDR >> 4); /* 0x1060 */
  load_file_far(kernel_ino, core_load_seg);

  /* VFS code module: load after the core far copy, page-aligned.
   * Core far copy occupies core_load_seg..core_load_seg+core_paras. */
  uint16_t core_paras = (core_size + 15u) >> 4;
  uint16_t vfs_seg = core_load_seg + core_paras;
  vfs_seg = (vfs_seg + 0xFFu) & ~0xFFu;  /* page-align (4 KB) */
  uint16_t vfs_ino = find_file(boot_ino, "kernel_vfs");
  uint16_t vfs_size = 0;
  if (vfs_ino) {
    vfs_size = load_file_far(vfs_ino, vfs_seg);
  }

  /* VFS data: zero the safe lower half, then load .rodata+.data.
   * Stage2 itself runs at 0xC000, so it cannot clear the full
   * 0xAC00-0xDFFF VFS reservation without wiping itself. The kernel
   * clears the upper half later in target_early_init(), before any
   * VFS data above 0xC000 is used. */
  {
    uint8_t *p = (uint8_t *)VFS_DATA_BASE;
    for (uint16_t i = 0; i < VFS_DATA_STAGE2_SIZE; i++) p[i] = 0;
  }
  uint16_t vfs_data_ino = find_file(boot_ino, "kernel_vfs_data");
  if (vfs_data_ino) {
    load_file(vfs_data_ino, (uint8_t *)VFS_DATA_BASE);
  }

  /* Compute page pool start: after last far segment, page-aligned */
  uint16_t pool_seg;
  if (vfs_size) {
    uint16_t vfs_paras = (vfs_size + 15u) >> 4;
    pool_seg = (vfs_seg + vfs_paras + 0xFFu) & ~0xFFu;
  } else {
    pool_seg = (core_load_seg + core_paras + 0xFFu) & ~0xFFu;
  }

  /* Write module info block for the kernel to read */
  mod_info_t *info = (mod_info_t *)MOD_INFO_ADDR;
  info->count = vfs_size ? 2 : 1;
  /* Module 0: core code segment (CS=0x1000) */
  info->mod[0].segment = core_cs;
  info->mod[0].size = core_size;
  /* Module 1: VFS */
  if (vfs_size) {
    info->mod[1].segment = vfs_seg;
    info->mod[1].size = vfs_size;
  }
  /* Module 2 slot: page pool start (read by target_pcxt.c) */
  info->mod[2].segment = pool_seg;
  info->mod[2].size = 0;

  /* Boot device parameters (read by bios_blk driver via target_pcxt.c) */
  info->boot_dev = boot_drive;
  info->reserved = 0;
  info->ufs_base = ufs_base_sector;
  info->dev_spt = secs_per_track;
  info->dev_heads = num_heads;
  info->dev_sectors = (boot_drive >= 0x80)
      ? ufs_partition_sectors
      : (uint32_t)(1440u - ufs_base_sector);

  /* Far-jump to core CS, IP=0x0600 (_start in boot.S).
   * Write far pointer to scratch area and use indirect ljmp. */
  {
    uint16_t *fptr = (uint16_t *)0x0400u;
    fptr[0] = KERNEL_ADDR;  /* IP = 0x0600 */
    fptr[1] = core_cs;      /* CS = 0x1000 */
    __asm__ volatile ("cli\n\t" "ljmp *0x0400");
  }
}
