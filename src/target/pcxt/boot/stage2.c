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

#define UFS_MAGIC         0x00011954u
#define UFS_ROOT_INO      2u
#define UFS_DIRECT_BLOCKS 12u

#define UFS_INODE_SIZE        128u
#define UFS_INODES_PER_SECTOR 4u

#define UFS_SB_SECTOR 16u

/* 44BSD superblock byte offsets from SB start (byte 8192). */
#define UFS_FS_IBLKNO_OFF 16u
#define UFS_FS_BSIZE_OFF  48u
#define UFS_FS_MAGIC_OFF   1372u

/* Inode field offsets (ufs_inode_t, 128 bytes). */
#define UFS_I_SIZE_OFF   8u
#define UFS_I_DIRECT_OFF 40u
#define UFS_I_IB_OFF     88u

/* Disk geometry — set at boot from known floppy values or INT 13h query */
static uint16_t secs_per_track;
static uint16_t num_heads;
static uint16_t ufs_base_sector;  /* absolute LBA of first UFS sector */

/* Stage2 at 0xC000 (up to 4 KB), BUF/IBUF above it */
#define BUF  ((uint8_t *)0xD000u)
#define IBUF ((uint8_t *)0xE000u)

#define KERNEL_ADDR 0x0600u

#define VFS_DATA_BASE        0xB600u
#define VFS_DATA_STAGE2_SIZE 0x0A00u  /* 0xB600+0x0A00=0xC000: stop before stage2 */

static uint32_t sb_itable_sector;
extern uint8_t boot_drive;

static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint16_t str_len16(const char *s) {
  uint16_t n = 0;
  while (s[n]) n++;
  return n;
}

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

static void __attribute__((noinline)) read_ufs_sector(uint16_t sec, void *dest)
{
  read_sector((uint16_t)(ufs_base_sector + sec), dest);
}

static void __attribute__((noinline)) read_ufs_block(uint32_t frag, void *dest)
{
  uint16_t lba = (uint16_t)(ufs_base_sector + frag);
  uint8_t *p = (uint8_t *)dest;
  for (uint16_t i = 0; i < UFS_SECS_PER_BLK; i++) {
    read_sector(lba + i, p);
    p += FLOPPY_SEC;
  }
}

static uint16_t find_in_dir(const uint8_t *blk, const char *name)
{
  uint16_t target_len = str_len16(name);
  for (uint16_t s = 0; s < UFS_SECS_PER_BLK; s++) {
    const uint8_t *sec = blk + s * FLOPPY_SEC;
    uint16_t off = 0;
    while ((uint16_t)(off + 8u) <= FLOPPY_SEC) {
      uint32_t ino = rd32(&sec[off + 0]);
      uint16_t reclen = rd16(&sec[off + 4]);
      uint8_t namlen = sec[off + 7];
      if (reclen == 0) break;
      if ((uint16_t)(off + reclen) > FLOPPY_SEC) break;
      if (ino != 0 && namlen == target_len) {
        const uint8_t *dn = &sec[off + 8];
        uint16_t i = 0;
        while (i < target_len && dn[i] == (uint8_t)name[i]) i++;
        if (i == target_len) return (uint16_t)ino;
      }
      off = (uint16_t)(off + reclen);
    }
  }
  return 0;
}

static void read_inode(uint16_t ino, uint8_t *out)
{
  uint16_t sec = (uint16_t)(sb_itable_sector + ino / UFS_INODES_PER_SECTOR);
  read_ufs_sector(sec, BUF);
  uint16_t off = (uint16_t)((ino % UFS_INODES_PER_SECTOR) * UFS_INODE_SIZE);
  uint8_t *s = BUF + off, *d = (uint8_t *)out;
  for (uint16_t i = 0; i < UFS_INODE_SIZE; i++) d[i] = s[i];
}

static void __attribute__((noinline)) load_block_far(uint32_t frag,
    uint16_t seg, uint16_t *off, uint32_t *loaded, uint32_t size)
{
  if (frag == 0) return;
  uint16_t lba = (uint16_t)(ufs_base_sector + frag);
  for (uint16_t s = 0; s < UFS_SECS_PER_BLK && *loaded < size; s++) {
    read_sector_far(lba + s, seg, *off);
    *off += FLOPPY_SEC;
    *loaded += FLOPPY_SEC;
  }
}

static void __attribute__((noinline)) load_block(uint32_t frag,
    uint8_t **dest, uint32_t *loaded, uint32_t size)
{
  uint16_t off = (uint16_t)(uintptr_t)*dest;
  load_block_far(frag, 0, &off, loaded, size);
  *dest = (uint8_t *)(uintptr_t)off;
}

/*
 * Load a file from UFS to seg:0000.
 * Returns the number of bytes loaded, or 0 on failure.
 */
static uint16_t load_file_far(uint16_t ino, uint16_t seg)
{
  uint8_t inode[UFS_INODE_SIZE];
  read_inode(ino, inode);

  uint32_t size = rd32(&inode[UFS_I_SIZE_OFF]);
  uint32_t direct[UFS_DIRECT_BLOCKS];
  for (uint16_t i = 0; i < UFS_DIRECT_BLOCKS; i++)
    direct[i] = rd32(&inode[UFS_I_DIRECT_OFF + i * 4u]);
  uint32_t indirect = rd32(&inode[UFS_I_IB_OFF]);

  uint16_t off = 0;
  uint32_t loaded = 0;
  for (uint16_t i = 0; i < UFS_DIRECT_BLOCKS && loaded < size; i++)
    load_block_far(direct[i], seg, &off, &loaded, size);

  if (indirect && loaded < size) {
    read_ufs_block(indirect, IBUF);
    for (uint16_t i = 0; i < UFS_BLOCK_SIZE / 4 && loaded < size; i++)
      load_block_far(rd32(&IBUF[i * 4u]), seg, &off, &loaded, size);
  }

  return (uint16_t)loaded;
}

/* Load a file to 0:dest (near, segment 0). */
static uint16_t load_file(uint16_t ino, uint8_t *dest)
{
  uint8_t inode[UFS_INODE_SIZE];
  read_inode(ino, inode);

  uint32_t size = rd32(&inode[UFS_I_SIZE_OFF]);
  uint32_t direct[UFS_DIRECT_BLOCKS];
  for (uint16_t i = 0; i < UFS_DIRECT_BLOCKS; i++)
    direct[i] = rd32(&inode[UFS_I_DIRECT_OFF + i * 4u]);
  uint32_t indirect = rd32(&inode[UFS_I_IB_OFF]);

  uint32_t loaded = 0;
  for (uint16_t i = 0; i < UFS_DIRECT_BLOCKS && loaded < size; i++)
    load_block(direct[i], &dest, &loaded, size);

  if (indirect && loaded < size) {
    read_ufs_block(indirect, IBUF);
    for (uint16_t i = 0; i < UFS_BLOCK_SIZE / 4 && loaded < size; i++)
      load_block(rd32(&IBUF[i * 4u]), &dest, &loaded, size);
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
  uint8_t inode[UFS_INODE_SIZE];
  read_inode(dir_ino, inode);
  for (uint16_t i = 0; i < UFS_DIRECT_BLOCKS; i++) {
    uint32_t frag = rd32(&inode[UFS_I_DIRECT_OFF + i * 4u]);
    if (!frag) break;
    read_ufs_block(frag, BUF);
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

  read_ufs_sector(UFS_SB_SECTOR, BUF);
  if (rd32(&BUF[UFS_FS_BSIZE_OFF]) != UFS_BLOCK_SIZE) {
    puts_bios("!UFS");
    return;
  }
  sb_itable_sector = rd32(&BUF[UFS_FS_IBLKNO_OFF]);

  /* Magic is at SB+1372, i.e. sector 18 offset 348. */
  read_ufs_sector((uint16_t)(UFS_SB_SECTOR + UFS_FS_MAGIC_OFF / FLOPPY_SEC),
                  BUF);
  if (rd32(&BUF[UFS_FS_MAGIC_OFF % FLOPPY_SEC]) != UFS_MAGIC) {
    puts_bios("!UFS");
    return;
  }

  /* Find /boot directory */
  uint16_t boot_ino = find_file(UFS_ROOT_INO, "boot");
  if (!boot_ino) { puts_bios("!boot"); return; }

  /* Load /boot/kernel_text to CS=0x1000:0x0000 (linear 0x10000).
   * This is THE copy of .text+.rodata — no DS=0 duplicate. */
  uint16_t kernel_text_ino = find_file(boot_ino, "kernel_text");
  if (!kernel_text_ino) { puts_bios("!ktxt"); return; }
  uint16_t core_cs = 0x1000u;
  uint16_t core_text_size = load_file_far(kernel_text_ino, core_cs);
  if (!core_text_size) { puts_bios("!ltxt"); return; }

  /* Load /boot/kernel_data to DS=0:0x0600 (data + initialized globals). */
  uint16_t kernel_data_ino = find_file(boot_ino, "kernel_data");
  if (!kernel_data_ino) { puts_bios("!kdat"); return; }
  uint16_t core_data_size = load_file(kernel_data_ino, (uint8_t *)KERNEL_ADDR);
  if (!core_data_size) { puts_bios("!ldat"); return; }

  /* VFS code module: load after the core far copy, page-aligned. */
  uint16_t core_paras = (core_text_size + 15u) >> 4;
  uint16_t vfs_seg = core_cs + core_paras;
  vfs_seg = (vfs_seg + 0xFFu) & ~0xFFu;  /* page-align (4 KB) */
  uint16_t vfs_ino = find_file(boot_ino, "kernel_vfs");
  uint16_t vfs_size = 0;
  if (vfs_ino) {
    vfs_size = load_file_far(vfs_ino, vfs_seg);
  }

  /* VFS data: zero the safe lower half, then load .rodata+.data.
   * Stage2 itself runs at 0xC000, so it cannot clear the full
   * 0xB600-0xDFFF VFS reservation without wiping itself. The kernel
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
    pool_seg = (core_cs + core_paras + 0xFFu) & ~0xFFu;
  }

  /* Write module info block for the kernel to read */
  mod_info_t *info = (mod_info_t *)MOD_INFO_ADDR;
  info->count = vfs_size ? 2 : 1;
  /* Module 0: core code segment (CS=0x1000) */
  info->mod[0].segment = core_cs;
  info->mod[0].size = core_text_size;
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

  /* Far-jump to core CS, IP=0x0000 (_start in boot.S, .text VMA base).
   * Write far pointer to scratch area and use indirect ljmp. */
  {
    uint16_t *fptr = (uint16_t *)0x0400u;
    fptr[0] = 0x0000u;      /* IP = 0x0000 (.text starts at VMA 0) */
    fptr[1] = core_cs;      /* CS = 0x1000 */
    __asm__ volatile ("cli\n\t" "ljmp *0x0400");
  }
}
