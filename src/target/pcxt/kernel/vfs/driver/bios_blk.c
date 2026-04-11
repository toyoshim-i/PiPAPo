/*
 * bios_blk.c — BIOS INT 13h block device driver for IBM PC
 *
 * Reads sectors via BIOS INT 13h.  Supports both floppy (DL < 0x80)
 * and hard disk (DL >= 0x80).  Geometry and UFS partition offset are
 * set from boot parameters passed by stage2 via mod_info.
 *
 * Write is not supported (read-only).
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

/* ── INT 13h sector read ──────────────────────────────────────────────── */

static int read_sector_bios(uint16_t lba, void *dest)
{
  uint16_t spt = i16_dev_spt;
  uint16_t secs_per_cyl = spt * i16_dev_heads;
  uint16_t cyl  = lba / secs_per_cyl;
  uint16_t rem  = lba % secs_per_cyl;
  uint16_t head = rem / spt;
  uint16_t sec  = rem % spt + 1;
  uint16_t err;

  __asm__ volatile (
    "push %%es\n\t"
    "xor  %%ax, %%ax\n\t"
    "mov  %%ax, %%es\n\t"
    "mov  $0x0201, %%ax\n\t"
    "int  $0x13\n\t"
    "movb %%ah, %%al\n\t"
    "xorb %%ah, %%ah\n\t"
    "mov  %%ax, %0\n\t"
    "pop  %%es"
    : "=m"(err)
    : "b"((uint16_t)(uintptr_t)dest),
      "c"((uint16_t)((cyl << 8) | sec)),
      "d"((uint16_t)((head << 8) | i16_boot_drive))
    : "ax", "memory", "cc"
  );

  return err == 0 ? 0 : -EIO;
}

/* ── Block device interface ───────────────────────────────────────────── */

static int bios_blk_read(blkdev_t *dev, page_id_t page, uint16_t off,
                         uint32_t sector, uint32_t count)
{
  (void)dev;
  uint32_t linear = (uint32_t)page * PAGE_SIZE + off;

  int rc = 0;
  for (uint32_t i = 0; i < count; i++) {
    uint16_t lba = i16_ufs_base_sector + (uint16_t)(sector + i);
    rc = read_sector_bios(lba, (void *)(uintptr_t)linear);
    if (rc < 0) break;
    linear += SECTOR_SIZE;
  }
  return rc;
}

static int bios_blk_write(blkdev_t *dev, page_id_t page, uint16_t off,
                          uint32_t sector, uint32_t count)
{
  (void)dev; (void)page; (void)off; (void)sector; (void)count;
  return -EROFS; /* read-only */
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
