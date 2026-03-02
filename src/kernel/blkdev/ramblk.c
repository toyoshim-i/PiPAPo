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

#include "blkdev.h"
#include "ramblk.h"
#include "../mm/page.h"
#include "../errno.h"

/* ── Overlay ────────────────────────────────────────────────────────────── */

#define OVERLAY_BASE  ((uint8_t *)SRAM_IOBUF_BASE)
#define OVERLAY_CAP   (SRAM_IOBUF_SIZE / BLKDEV_SECTOR_SIZE)   /* 48 */
#define SLOT_CLEAN    0xFFFFu
#define MAX_SECTORS   512u   /* max sectors the slot map can track */

static uint16_t slot_map[MAX_SECTORS];
static uint32_t dirty_count;

/* ── Driver state ───────────────────────────────────────────────────────── */

static const uint8_t *rom_base;
static uint32_t       rom_sectors;

/* ── Read / write handlers ──────────────────────────────────────────────── */

static int ramblk_read(struct blkdev *dev, void *buf,
                       uint32_t sector, uint32_t count)
{
    (void)dev;
    uint8_t *dst = (uint8_t *)buf;

    for (uint32_t s = sector; s < sector + count; s++) {
        if (s >= rom_sectors)
            return -EIO;

        if (slot_map[s] != SLOT_CLEAN) {
            /* Dirty: read from overlay */
            uint32_t slot = slot_map[s];
            __builtin_memcpy(dst,
                             OVERLAY_BASE + slot * BLKDEV_SECTOR_SIZE,
                             BLKDEV_SECTOR_SIZE);
        } else {
            /* Clean: read from ROM/RAM original */
            __builtin_memcpy(dst,
                             rom_base + (uint32_t)s * BLKDEV_SECTOR_SIZE,
                             BLKDEV_SECTOR_SIZE);
        }
        dst += BLKDEV_SECTOR_SIZE;
    }
    return 0;
}

static int ramblk_write(struct blkdev *dev, const void *buf,
                        uint32_t sector, uint32_t count)
{
    (void)dev;
    const uint8_t *src = (const uint8_t *)buf;

    for (uint32_t s = sector; s < sector + count; s++) {
        if (s >= rom_sectors)
            return -EIO;

        if (slot_map[s] != SLOT_CLEAN) {
            /* Already dirty: overwrite in place */
            uint32_t slot = slot_map[s];
            __builtin_memcpy(OVERLAY_BASE + slot * BLKDEV_SECTOR_SIZE,
                             src, BLKDEV_SECTOR_SIZE);
        } else {
            /* First write to this sector: allocate overlay slot */
            if (dirty_count >= OVERLAY_CAP)
                return -ENOSPC;
            uint32_t slot = dirty_count++;
            slot_map[s] = (uint16_t)slot;
            __builtin_memcpy(OVERLAY_BASE + slot * BLKDEV_SECTOR_SIZE,
                             src, BLKDEV_SECTOR_SIZE);
        }
        src += BLKDEV_SECTOR_SIZE;
    }
    return 0;
}

/* ── Public init ────────────────────────────────────────────────────────── */

int ramblk_init(const void *base, uint32_t size_bytes)
{
    if (!base || size_bytes < BLKDEV_SECTOR_SIZE)
        return -EINVAL;

    rom_base    = (const uint8_t *)base;
    rom_sectors = size_bytes / BLKDEV_SECTOR_SIZE;

    if (rom_sectors > MAX_SECTORS)
        rom_sectors = MAX_SECTORS;

    /* Clear overlay state */
    dirty_count = 0;
    for (uint32_t i = 0; i < MAX_SECTORS; i++)
        slot_map[i] = SLOT_CLEAN;

    /* Register as "mmcblk0" */
    blkdev_t dev = {
        .name         = "mmcblk0",
        .sector_count = rom_sectors,
        .priv         = (void *)0,
        .read         = ramblk_read,
        .write        = ramblk_write,
    };

    return blkdev_register(&dev);
}
