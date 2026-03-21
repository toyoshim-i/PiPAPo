/*
 * flatblk.h — Flat read-only memory-backed block device
 *
 * Provides a block device that reads directly from a contiguous memory
 * buffer.  Write operations are not supported (returns -EIO).
 *
 * Used on the X68000 target to expose the rootfs UFS image loaded into
 * RAM by stage2 as a block device mountable by the kernel UFS driver.
 */

#ifndef PPAP_KERNEL_BLKDEV_FLATBLK_H
#define PPAP_KERNEL_BLKDEV_FLATBLK_H

#include <stdint.h>

/*
 * Register a flat read-only block device.
 *
 * name       — device name (e.g. "ram0")
 * base       — pointer to the memory buffer
 * size_bytes — buffer size in bytes (must be a multiple of BLKDEV_SECTOR_SIZE)
 *
 * Returns the blkdev slot index on success, negative errno on failure.
 */
int flatblk_init(const char *name, const void *base, uint32_t size_bytes);

#endif /* PPAP_KERNEL_BLKDEV_FLATBLK_H */
