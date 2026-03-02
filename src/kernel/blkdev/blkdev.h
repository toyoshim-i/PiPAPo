/*
 * blkdev.h — Block device abstraction layer
 *
 * Universal interface between filesystem drivers (VFAT, UFS) and storage
 * hardware (SPI/SD on RP2040, RAM on QEMU, loopback in Phase 5).
 *
 * All I/O is in units of 512-byte sectors.  The blkdev_t struct holds
 * function pointers for read/write; drivers fill these in at registration.
 *
 * The registry is a fixed-size array of BLKDEV_MAX slots.  Devices are
 * registered by name ("mmcblk0", "loop0", …) and looked up by name.
 */

#ifndef PPAP_BLKDEV_BLKDEV_H
#define PPAP_BLKDEV_BLKDEV_H

#include <stddef.h>
#include <stdint.h>
#include "config.h"

/* ── Block device struct ─────────────────────────────────────────────────── */

typedef struct blkdev {
    const char *name;           /* device name: "mmcblk0", "loop0", … */
    uint32_t    sector_count;   /* total sectors on device */
    void       *priv;           /* driver-private state */

    /* Read `count` sectors starting at `sector` into `buf`.
     * buf must be at least count × BLKDEV_SECTOR_SIZE bytes.
     * Returns 0 on success, negative errno on failure. */
    int (*read)(struct blkdev *dev, void *buf,
                uint32_t sector, uint32_t count);

    /* Write `count` sectors starting at `sector` from `buf`.
     * buf must be at least count × BLKDEV_SECTOR_SIZE bytes.
     * Returns 0 on success, negative errno on failure. */
    int (*write)(struct blkdev *dev, const void *buf,
                 uint32_t sector, uint32_t count);
} blkdev_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Initialise the block device registry.  Call once from kmain(). */
void blkdev_init(void);

/* Register a block device.
 * The blkdev_t is copied into an internal slot.
 * Returns the slot index (0–BLKDEV_MAX-1) on success, negative errno
 * if the registry is full (-ENOMEM) or name is NULL (-EINVAL). */
int blkdev_register(const blkdev_t *dev);

/* Look up a registered block device by name.
 * Returns a pointer to the internal slot, or NULL if not found. */
blkdev_t *blkdev_find(const char *name);

#endif /* PPAP_BLKDEV_BLKDEV_H */
