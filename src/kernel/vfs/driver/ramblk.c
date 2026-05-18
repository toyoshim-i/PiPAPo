/*
 * ramblk.c — RAM/ROM-backed block device driver (QEMU only)
 *
 * Provides a block device backed by a read-only ROM buffer (the FAT32 image
 * embedded via .incbin in Step 8) with a write overlay in SRAM.
 *
 * On read:  if the sector is dirty, return the SRAM overlay copy;
 *           otherwise return the original ROM/RAM data.
 * On write: copy the sector to the SRAM overlay and mark it dirty.
 *
 * Write Overlay (SRAM_IOBUF_BASE, 24 KB):
 *   The overlay stores modified sectors sequentially.  A slot_map[] array
 *   in kernel BSS records each sector's overlay index (0xFFFF = clean).
 *   New dirty sectors are appended; re-writes update in place.
 *
 *   Max overlay capacity = SRAM_IOBUF_SIZE / 512 = 48 sectors.
 *
 * Registers as "mmcblk0" via blkdev_register().
 */

#include "kernel/vfs/driver/ramblk.h"

#include "common/errno.h"
#include "kernel/common/config.h"
#include "kernel/common/mod/mod_core.h"
#include "kernel/vfs/driver/blkdev.h"

/* ── Overlay ────────────────────────────────────────────────────────────── */

#define OVERLAY_BASE ((uint8_t *)SRAM_IOBUF_BASE)
#define OVERLAY_CAP (SRAM_IOBUF_SIZE / BLKDEV_SECTOR_SIZE) /* 48 */
#define SLOT_CLEAN 0xFFFFu
#define MAX_SECTORS 512u /* max sectors the slot map can track */

static uint16_t slot_map[MAX_SECTORS];
static uint32_t dirty_count;

/* ── Driver state ───────────────────────────────────────────────────────── */

static const uint8_t *rom_base;
static uint32_t rom_sectors;

/* ── Read / write handlers ──────────────────────────────────────────────── */

static int ramblk_read(struct blkdev *dev, page_id_t page, uint16_t off,
                       uint32_t sector, uint32_t count) {
  (void)dev;

  for (uint32_t s = sector; s < sector + count; s++) {
    if (s >= rom_sectors) return -EIO;

    const uint8_t *src =
        (slot_map[s] != SLOT_CLEAN)
            ? OVERLAY_BASE + (uint32_t)slot_map[s] * BLKDEV_SECTOR_SIZE
            : rom_base + (uint32_t)s * BLKDEV_SECTOR_SIZE;
    mod_core.page_write(page, off, src, BLKDEV_SECTOR_SIZE);
    off += BLKDEV_SECTOR_SIZE;
  }
  return 0;
}

static int ramblk_write(struct blkdev *dev, page_id_t page, uint16_t off,
                        uint32_t sector, uint32_t count) {
  (void)dev;

  for (uint32_t s = sector; s < sector + count; s++) {
    if (s >= rom_sectors) return -EIO;

    uint32_t slot;
    if (slot_map[s] != SLOT_CLEAN) {
      slot = slot_map[s];
    } else {
      if (dirty_count >= OVERLAY_CAP) return -ENOSPC;
      slot = dirty_count++;
      slot_map[s] = (uint16_t)slot;
    }
    mod_core.page_read(page, off, OVERLAY_BASE + slot * BLKDEV_SECTOR_SIZE,
                       BLKDEV_SECTOR_SIZE);
    off += BLKDEV_SECTOR_SIZE;
  }
  return 0;
}

/* ── Public init ────────────────────────────────────────────────────────── */

int ramblk_init(const void *base, uint32_t size_bytes) {
  if (!base || size_bytes < BLKDEV_SECTOR_SIZE) return -EINVAL;

  rom_base = (const uint8_t *)base;
  rom_sectors = size_bytes / BLKDEV_SECTOR_SIZE;

  if (rom_sectors > MAX_SECTORS) rom_sectors = MAX_SECTORS;

  /* Clear overlay state */
  dirty_count = 0;
  for (uint32_t i = 0; i < MAX_SECTORS; i++) slot_map[i] = SLOT_CLEAN;

  /* Register as "mmcblk0" */
  blkdev_t dev = {
      .name = "mmcblk0",
      .sector_count = rom_sectors,
      .priv = (void *)0,
      .read = ramblk_read,
      .write = ramblk_write,
  };

  return blkdev_register(&dev);
}
