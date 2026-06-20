/*
 * iocs_blk.c — X68000 floppy block device backed by IOCS _B_READ
 *
 * Presents the post-bootloader portion of the boot floppy as a blkdev_t
 * ("fd0") in 512-byte sectors.  The X68000 2HD floppy native sector is
 * 1024 bytes, so each blkdev sector is one half of a physical sector; a
 * single-sector buffer caches the last 1024-byte read to make consecutive
 * 512-byte reads within the same physical sector free.
 *
 * The first four physical sectors (0..3) hold stage1 and stage2; the UFS
 * filesystem begins at physical sector 4 (== blkdev sector 8).
 */

#include "kernel/vfs/driver/iocs_blk.h"

#include <stdint.h>

#include "common/errno.h"
#include "kernel/common/mod/mod_core.h"
#include "kernel/vfs/driver/blkdev.h"
#include "kernel/vfs/driver/x68k_iocs.h"

/* X68000 2HD floppy geometry: 77 cyl x 2 head x 8 sec x 1024 B = 1232 KB. */
#define FLOPPY_SEC_BYTES 1024u
#define FLOPPY_SECS_PER_CYL 16u /* 2 heads x 8 sectors */
#define FLOPPY_SECS_PER_HEAD 8u
#define FLOPPY_TOTAL_PHYS_SECS (77u * FLOPPY_SECS_PER_CYL) /* 1232 */

/* IOCS _B_READ argument constants — match stage2.c. */
#define IOCS_B_READ 0x46u
#define FDC_PDA 0x90u   /* 2HD FDD unit 0 */
#define FDC_MODE 0x70u  /* MFM | retry | seek */
#define SEC_LEN_CODE 3u /* 1024-byte sectors */

/* UFS partition starts after stage1 (sector 0) and stage2 (sectors 1..3).
 * In 512-byte blkdev units that is sector 8. */
#define UFS_BASE_512 8u

/* One 1024-byte physical sector contains two blkdev sectors. */
#define BLKDEV_SECTORS_PER_PHYS (FLOPPY_SEC_BYTES / BLKDEV_SECTOR_SIZE)

#define CACHE_PHYS_NONE ((uint32_t) - 1)

static uint8_t cache_buf[FLOPPY_SEC_BYTES] __attribute__((aligned(4)));
static uint32_t cache_phys_lba = CACHE_PHYS_NONE;

/* Read one 1024-byte physical sector into `dest` via IOCS _B_READ.
 *
 * IPL handling: the X68000 kernel runs at IPL=7 from reset until
 * sched_start() lowers it (see arch_irq_save in arch/m68k); at IPL=7
 * the FDC completion IRQ can never fire and _B_READ blocks forever.
 * Clear-and-set IPL=0 across the trap so the IPL ROM's _B_READ handler
 * sees the same SR state stage1/stage2 had (IPL=0, interrupts on).
 * Pair with an MFP-level Timer-C mask so that lowering IPL does not
 * let the scheduler tick re-enter IOCS via uart console writes. */
static int iocs_read_phys(uint32_t lba, void *dest) {
  uint32_t cyl = lba / FLOPPY_SECS_PER_CYL;
  uint32_t track_off = lba % FLOPPY_SECS_PER_CYL;
  uint32_t head = track_off / FLOPPY_SECS_PER_HEAD;
  uint32_t sec = (track_off % FLOPPY_SECS_PER_HEAD) + 1u; /* 1-based */

  uint32_t d2_val = (SEC_LEN_CODE << 24) | (cyl << 16) | (head << 8) | sec;

  x68k_iocs_enter();
  x68k_iocs_irq_state_t irq = x68k_iocs_irq_begin();

  register uint32_t d0 asm("d0") = IOCS_B_READ;
  register uint32_t d1 asm("d1") = (FDC_PDA << 8) | FDC_MODE;
  register uint32_t d2 asm("d2") = d2_val;
  register uint32_t d3 asm("d3") = FLOPPY_SEC_BYTES;
  register void *a1 asm("a1") = dest;
  asm volatile("trap #15"
               : "+r"(d0)
               : "r"(d1), "r"(d2), "r"(d3), "r"(a1)
               : "a0", "memory");

  x68k_iocs_irq_end(irq);
  x68k_iocs_exit();

  /* d0 returns FDC status: negative = error, 0 = success. */
  return ((int32_t)d0) < 0 ? -EIO : 0;
}

static int iocs_blk_read(struct blkdev *dev, page_id_t page, uint16_t off,
                         uint32_t sector, uint32_t count) {
  (void)dev;
  if (sector + count > dev->sector_count) return -EIO;

  for (uint32_t i = 0; i < count; i++) {
    uint32_t abs512 = UFS_BASE_512 + sector + i;
    uint32_t phys = abs512 / BLKDEV_SECTORS_PER_PHYS;
    uint32_t half = abs512 % BLKDEV_SECTORS_PER_PHYS;

    if (cache_phys_lba != phys) {
      int rc = iocs_read_phys(phys, cache_buf);
      if (rc < 0) {
        cache_phys_lba = CACHE_PHYS_NONE;
        return rc;
      }
      cache_phys_lba = phys;
    }
    mod_core.page_write(page, (uint16_t)(off + i * BLKDEV_SECTOR_SIZE),
                        cache_buf + half * BLKDEV_SECTOR_SIZE,
                        BLKDEV_SECTOR_SIZE);
  }
  return 0;
}

static int iocs_blk_write(struct blkdev *dev, page_id_t page, uint16_t off,
                          uint32_t sector, uint32_t count) {
  (void)dev;
  (void)page;
  (void)off;
  (void)sector;
  (void)count;
  return -EIO; /* read-only */
}

static blkdev_t iocs_dev;

void iocs_blk_init(void) {
  iocs_dev.name = "fd0";
  iocs_dev.sector_count =
      FLOPPY_TOTAL_PHYS_SECS * BLKDEV_SECTORS_PER_PHYS - UFS_BASE_512;
  iocs_dev.flags = BLKDEV_F_NOCACHE;
  iocs_dev.priv = (void *)0;
  iocs_dev.read = iocs_blk_read;
  iocs_dev.write = iocs_blk_write;
  blkdev_register(&iocs_dev);
}
