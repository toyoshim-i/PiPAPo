/*
 * flatblk.c — Flat read-only memory-backed block device
 *
 * Provides a block device that reads directly from a contiguous RAM buffer.
 * No write overlay.  Used on the X68000 target for the rootfs UFS image
 * loaded into RAM by stage2.
 */

#include "kernel/vfs/driver/flatblk.h"

#include <stdint.h>

#include "common/errno.h"
#include "kernel/common/mod/mod_core.h"
#include "kernel/vfs/driver/blkdev.h"

static const uint8_t *flat_base;
static uint32_t flat_sectors;

static int flatblk_read(struct blkdev *dev, page_id_t page, uint16_t off,
                        uint32_t sector, uint32_t count) {
  (void)dev;
  if (sector + count > flat_sectors) return -EIO;
  mod_core.page_write(page, off,
                      flat_base + (uint32_t)sector * BLKDEV_SECTOR_SIZE,
                      (uint16_t)((uint32_t)count * BLKDEV_SECTOR_SIZE));
  return 0;
}

static int flatblk_write(struct blkdev *dev, page_id_t page, uint16_t off,
                         uint32_t sector, uint32_t count) {
  (void)dev;
  (void)page;
  (void)off;
  (void)sector;
  (void)count;
  return -EIO; /* read-only */
}

int flatblk_init(const char *name, const void *base, uint32_t size_bytes) {
  if (!base || size_bytes < BLKDEV_SECTOR_SIZE) return -EINVAL;

  flat_base = (const uint8_t *)base;
  flat_sectors = size_bytes / BLKDEV_SECTOR_SIZE;

  blkdev_t dev = {
      .name = name,
      .sector_count = flat_sectors,
      .priv = (void *)0,
      .read = flatblk_read,
      .write = flatblk_write,
  };
  return blkdev_register(&dev);
}
