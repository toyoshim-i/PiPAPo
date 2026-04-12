/*
 * bios_blk.c — BIOS INT 13h block device driver for IBM PC
 *
 * Reads and writes sectors via BIOS INT 13h.  Supports both floppy
 * (DL < 0x80) and hard disk (DL >= 0x80).  Geometry and UFS partition
 * offset are set from boot parameters passed by stage2 via mod_info.
 */

#include "common/errno.h"
#include "kernel/common/core/page_types.h"
#include "kernel/common/ioregs.h"
#include "kernel/vfs/driver/blkdev.h"

#define SECTOR_SIZE 512u

/* Boot device parameters — set by target_pcxt.c from mod_info */
extern uint8_t  i16_boot_drive;
extern uint16_t i16_ufs_base_sector;
extern uint16_t i16_dev_spt;
extern uint16_t i16_dev_heads;
extern uint32_t i16_dev_sectors;

/* ── INT 13h sector I/O ──────────────────────────────────────────────── */

/* INT 13h AH=02h (read) or AH=03h (write), one sector at `lba`.
 *
 * BIOS INT 13h internally enables interrupts (STI) for FDC DMA
 * completion.  If our timer ISR (INT 08h) fires during that window,
 * it piles ~150 bytes onto the same kernel stack that is already deep
 * in the syscall read path — enough to overflow the 1 KB stack.
 * Mask IRQ 0 at the PIC before calling INT 13h so the timer stays
 * pending until the disk I/O returns and the mask is restored. */
static int bios_int13(uint16_t lba, void *buf, uint8_t ah_func)
{
  uint16_t spt = i16_dev_spt;
  uint16_t secs_per_cyl = spt * i16_dev_heads;
  uint16_t cyl  = lba / secs_per_cyl;
  uint16_t rem  = lba % secs_per_cyl;
  uint16_t head = rem / spt;
  uint16_t sec  = rem % spt + 1;
  uint16_t ax_in = (uint16_t)((uint16_t)ah_func << 8) | 0x01u;
  uint16_t err;

  /* Mask IRQ 0 (timer) to prevent our ISR from firing while BIOS
   * INT 13h has interrupts enabled for DMA. */
  uint8_t pic_mask = inb(PIC1_DATA);
  outb(PIC1_DATA, pic_mask | 0x01u);

  __asm__ volatile (
    "push %%es\n\t"
    "xor  %%di, %%di\n\t"
    "mov  %%di, %%es\n\t"
    "int  $0x13\n\t"
    "movb %%ah, %%al\n\t"
    "xorb %%ah, %%ah\n\t"
    "mov  %%ax, %0\n\t"
    "pop  %%es"
    : "=m"(err)
    : "a"(ax_in),
      "b"((uint16_t)(uintptr_t)buf),
      "c"((uint16_t)((cyl << 8) | sec)),
      "d"((uint16_t)((head << 8) | i16_boot_drive))
    : "di", "memory", "cc"
  );

  /* Restore original PIC mask. */
  outb(PIC1_DATA, pic_mask);

  return err == 0 ? 0 : -EIO;
}

/* ── Block device interface ───────────────────────────────────────────── */

static int bios_blk_read(blkdev_t *dev, page_id_t page, uint16_t off,
                         uint32_t sector, uint32_t count)
{
  (void)dev;
  uint32_t linear = (uint32_t)page * PAGE_SIZE + off;

  for (uint32_t i = 0; i < count; i++) {
    uint16_t lba = i16_ufs_base_sector + (uint16_t)(sector + i);
    int rc = bios_int13(lba, (void *)(uintptr_t)linear, 0x02);
    if (rc < 0) return rc;
    linear += SECTOR_SIZE;
  }
  return 0;
}

static int bios_blk_write(blkdev_t *dev, page_id_t page, uint16_t off,
                           uint32_t sector, uint32_t count)
{
  (void)dev;
  uint32_t linear = (uint32_t)page * PAGE_SIZE + off;

  for (uint32_t i = 0; i < count; i++) {
    uint16_t lba = i16_ufs_base_sector + (uint16_t)(sector + i);
    int rc = bios_int13(lba, (void *)(uintptr_t)linear, 0x03);
    if (rc < 0) return rc;
    linear += SECTOR_SIZE;
  }
  return 0;
}

/* ── Public API ───────────────────────────────────────────────────────── */

static blkdev_t bios_dev;

void bios_blk_init(void)
{
  if (i16_boot_drive >= 0x80) {
    bios_dev.name = "hd0";
  } else {
    bios_dev.name = "fd0";
  }
  bios_dev.sector_count = i16_dev_sectors;
  bios_dev.priv = (void *)0;
  bios_dev.read = bios_blk_read;
  bios_dev.write = bios_blk_write;
  blkdev_register(&bios_dev);
}
