/*
 * ramblk.h — RAM/ROM-backed block device driver (QEMU only)
 *
 * Provides a block device backed by a read-only ROM buffer with a writable
 * SRAM overlay.  Used on QEMU to emulate an SD card for FAT32 testing.
 *
 * Call ramblk_init() once from kmain() after blkdev_init().
 */

#ifndef PPAP_BLKDEV_RAMBLK_H
#define PPAP_BLKDEV_RAMBLK_H

#include <stdint.h>

/*
 * Initialise the RAM block device and register it as "mmcblk0".
 *
 * `base`       — pointer to the backing data (ROM-embedded FAT32 image or
 *                 a test buffer in SRAM).
 * `size_bytes` — size of the backing data in bytes (must be a multiple of
 *                 BLKDEV_SECTOR_SIZE).
 *
 * Returns the blkdev slot index on success, negative errno on failure.
 */
int ramblk_init(const void *base, uint32_t size_bytes);

#endif /* PPAP_BLKDEV_RAMBLK_H */
