/*
 * floppy_blk.c — Floppy disk block device driver for IBM PC
 *
 * Reads sectors from the 1.44 MB floppy via BIOS INT 13h.
 * The UFS filesystem occupies sectors 9+ on the floppy (sectors 0-8
 * are stage1 + stage2).  The block device presents the UFS partition
 * as sector 0 = floppy sector 9.
 *
 * Write is not supported (read-only floppy UFS).
 */

#include "common/errno.h"
#include "kernel/common/ioregs.h"
#include "kernel/vfs/driver/blkdev.h"

#define FLOPPY_SEC       512u
#define SECS_PER_TRACK   18u
#define NUM_HEADS        2u
#define SECS_PER_CYL     (SECS_PER_TRACK * NUM_HEADS)
#define UFS_FLOPPY_BASE  9u   /* UFS starts at floppy sector 9 */
#define FLOPPY_TOTAL     2880u /* 1.44 MB */

/* Boot drive number — saved by stage1, available as a global */
static uint8_t drive_no;

/* ── INT 13h sector read ──────────────────────────────────────────────── */

static int read_sector_bios(uint16_t lba, void *dest)
{
  uint16_t cyl  = lba / SECS_PER_CYL;
  uint16_t rem  = lba % SECS_PER_CYL;
  uint16_t head = rem / SECS_PER_TRACK;
  uint16_t sec  = rem % SECS_PER_TRACK + 1;
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
      "d"((uint16_t)((head << 8) | drive_no))
    : "ax", "memory", "cc"
  );

  return err == 0 ? 0 : -EIO;
}

/* ── Block device interface ───────────────────────────────────────────── */

static int floppy_read(blkdev_t *dev, void *buf, uint32_t sector,
                       uint32_t count)
{
  (void)dev;
  uint8_t *p = (uint8_t *)buf;

  int rc = 0;
  for (uint32_t i = 0; i < count; i++) {
    uint16_t lba = UFS_FLOPPY_BASE + (uint16_t)(sector + i);
    if (lba >= FLOPPY_TOTAL) { rc = -EIO; break; }
    rc = read_sector_bios(lba, p);
    if (rc < 0) break;
    p += FLOPPY_SEC;
  }
  return rc;
}

static int floppy_write(blkdev_t *dev, const void *buf, uint32_t sector,
                        uint32_t count)
{
  (void)dev; (void)buf; (void)sector; (void)count;
  return -EROFS; /* read-only */
}

/* ── Public API ───────────────────────────────────────────────────────── */

static blkdev_t floppy_dev = {
  .name = "fd0",
  .sector_count = FLOPPY_TOTAL - UFS_FLOPPY_BASE,
  .priv = (void *)0,
  .read = floppy_read,
  .write = floppy_write,
};

void floppy_blk_init(void)
{
  /* Drive number 0 (floppy A:) — could be passed from stage2 */
  drive_no = 0;
  blkdev_register(&floppy_dev);
}
