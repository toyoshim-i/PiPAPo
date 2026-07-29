/*
 * scsi_blk.c — X68000 SCSI hard-disk block device backed by SCSI IOCS
 *
 * Presents a SCSI hard disk as a blkdev_t ("sd0") in 512-byte sectors,
 * driven through the IPL ROM's internal SCSI BIOS via IOCS _SCSIDRV
 * (IOCS $F5, TRAP #15).  On XEiJ's Hybrid model the ROM's SCSI driver talks
 * to the emulated MB89352 (SPC), so no direct controller access is needed.
 *
 * Disk layout (XEiJ .hds): logical block 0 holds the "X68SCSI1" device-init
 * header, so the rootfs UFS starts at LBA SCSI_UFS_BASE; a blkdev sector S
 * maps to LBA (SCSI_UFS_BASE + S).  Records are 512 bytes = one blkdev
 * sector, so a read goes straight into the target page (no cache, unlike the
 * 1024-byte floppy sectors of iocs_blk).
 *
 * Like iocs_blk, every IOCS call takes the x68k IOCS guard and masks Timer-C
 * while lowering IPL, so the ROM's SCSI/DMAC completion interrupts fire but
 * the scheduler tick cannot re-enter IOCS.
 */

#include "kernel/vfs/driver/scsi_blk.h"

#include <stdint.h>

#include "common/errno.h"
#include "kernel/common/mod/mod_core.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/vfs/driver/blkdev.h"
#include "kernel/vfs/driver/x68k_iocs.h"

/* IOCS _SCSIDRV dispatch: d0.b = $F5, d1.l = SCSI call number. */
#define IOCS_SCSIDRV 0xF5u
#define SCSI_S_READ 0x21u
#define SCSI_S_READCAP 0x25u

/* Target 0 (XEiJ -sc0 → SCSI ID 0); IDs 0-6 are selectable, 7 is the host. */
#define SCSI_TARGET_ID 0u
/* Block-size code for _S_READ/_S_WRITE d5: 0=256, 1=512, 2=1024. */
#define SCSI_BLKSIZE_CODE 1u

/* LBA 0 holds the X68SCSI1 header; the UFS begins at the next record. */
#define SCSI_UFS_BASE 1u

/* Issue _S_READ for `nblocks` 512-byte records starting at `lba` into `buf`.
 * Returns 0 on success, -EIO on a negative IOCS status. */
static int scsi_read_blocks(uint32_t lba, uint32_t nblocks, void *buf) {
  x68k_iocs_enter();
  x68k_iocs_irq_state_t irq = x68k_iocs_irq_begin();

  register uint32_t d0 asm("d0") = IOCS_SCSIDRV;
  register uint32_t d1 asm("d1") = SCSI_S_READ;
  register uint32_t d2 asm("d2") = lba;
  register uint32_t d3 asm("d3") = nblocks;
  register uint32_t d4 asm("d4") = SCSI_TARGET_ID;
  register uint32_t d5 asm("d5") = SCSI_BLKSIZE_CODE;
  register void *a1 asm("a1") = buf;
  asm volatile("trap #15"
               : "+r"(d0), "+r"(d1), "+r"(d2), "+r"(d3), "+r"(d4), "+r"(d5),
                 "+r"(a1)
               :
               : "a0", "memory");

  x68k_iocs_irq_end(irq);
  x68k_iocs_exit();
  return ((int32_t)d0) < 0 ? -EIO : 0;
}

/* _S_READCAP: fills `cap` (8 bytes: last-LBA long, block-length long, both
 * big-endian).  Returns 0 on success, -EIO otherwise. */
static int scsi_read_capacity(uint8_t *cap) {
  x68k_iocs_enter();
  x68k_iocs_irq_state_t irq = x68k_iocs_irq_begin();

  register uint32_t d0 asm("d0") = IOCS_SCSIDRV;
  register uint32_t d1 asm("d1") = SCSI_S_READCAP;
  register uint32_t d4 asm("d4") = SCSI_TARGET_ID;
  register void *a1 asm("a1") = cap;
  asm volatile("trap #15"
               : "+r"(d0), "+r"(d1), "+r"(d4), "+r"(a1)
               :
               : "d2", "d3", "a0", "memory");

  x68k_iocs_irq_end(irq);
  x68k_iocs_exit();
  return ((int32_t)d0) < 0 ? -EIO : 0;
}

static int scsi_blk_read(struct blkdev *dev, page_id_t page, uint16_t off,
                         uint32_t sector, uint32_t count) {
  if (sector + count > dev->sector_count) return -EIO;

  /* 512 B stack bounce: _S_READ needs a linear buffer; the page is written
   * from it afterwards (matches spi_sd's read path). */
  uint8_t buf[BLKDEV_SECTOR_SIZE];
  for (uint32_t i = 0; i < count; i++) {
    int rc = scsi_read_blocks(SCSI_UFS_BASE + sector + i, 1u, buf);
    if (rc < 0) return rc;
    mod_core.page_write(page, (uint16_t)(off + i * BLKDEV_SECTOR_SIZE), buf,
                        BLKDEV_SECTOR_SIZE);
  }
  return 0;
}

static int scsi_blk_write(struct blkdev *dev, page_id_t page, uint16_t off,
                          uint32_t sector, uint32_t count) {
  (void)dev;
  (void)page;
  (void)off;
  (void)sector;
  (void)count;
  return -EIO; /* read-only until _S_WRITE lands (X-9) */
}

static blkdev_t scsi_dev;

void scsi_blk_init(void) {
  /* Probe with READ CAPACITY: a target that returns a valid geometry is
   * present and usable (and gives the sector count).  No disk / no image
   * yields a negative status.  (TEST UNIT READY is unreliable here — it
   * returns a non-zero non-error status for a ready disk.) */
  uint8_t cap[8];
  if (scsi_read_capacity(cap) < 0) {
    mod_vfs.klogf("SCSI: no disk at id %u\n", (unsigned)SCSI_TARGET_ID);
    return;
  }
  uint32_t last_lba = ((uint32_t)cap[0] << 24) | ((uint32_t)cap[1] << 16) |
                      ((uint32_t)cap[2] << 8) | (uint32_t)cap[3];
  uint32_t total = last_lba + 1u;
  if (total <= SCSI_UFS_BASE) {
    mod_vfs.klogf("SCSI: disk too small (%u records)\n", (unsigned)total);
    return;
  }

  scsi_dev.name = "sd0";
  scsi_dev.sector_count = total - SCSI_UFS_BASE;
  scsi_dev.flags = BLKDEV_F_NOCACHE;
  scsi_dev.priv = (void *)0;
  scsi_dev.read = scsi_blk_read;
  scsi_dev.write = scsi_blk_write;
  blkdev_register(&scsi_dev);

  mod_vfs.klogf("SCSI: sd0 id %u, %u sectors\n", (unsigned)SCSI_TARGET_ID,
                (unsigned)scsi_dev.sector_count);
}
