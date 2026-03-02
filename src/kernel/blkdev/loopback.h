/*
 * loopback.h — Loopback block device driver
 *
 * Translates block device sector read/write operations into VFS file I/O
 * on an underlying image file.  This is the key mechanism enabling UFS
 * filesystems to be stored as image files on a VFAT partition.
 *
 * I/O path: UFS driver → loopback blkdev → VFAT ops->read/write → SD blkdev
 *
 * Up to LOOP_MAX (3) concurrent loopback devices are supported,
 * corresponding to /dev/loop0, /dev/loop1, /dev/loop2.
 */

#ifndef PPAP_BLKDEV_LOOPBACK_H
#define PPAP_BLKDEV_LOOPBACK_H

#include <stdint.h>

#define LOOP_MAX  3   /* /dev/loop0, loop1, loop2 */

/*
 * Initialise the loopback subsystem.  Call once from kmain() after
 * blkdev_init() and VFS mount of the VFAT partition.
 */
void loopback_init(void);

/*
 * Set up a loopback device from an image file path.
 *
 * Opens the file via vfs_lookup(), determines its size, computes
 * sector_count, and registers a block device named "loopN".
 *
 * The backing vnode is held with an incremented refcnt for the
 * lifetime of the loopback device.
 *
 * Returns the loop device index (0–2) on success, or negative errno:
 *   -ENOMEM   all LOOP_MAX slots are in use
 *   -ENOENT   image file not found
 *   -EINVAL   file size is 0 or not a multiple of sector size
 */
int loopback_setup(const char *image_path);

/*
 * Tear down a loopback device: release the backing vnode, mark the
 * slot as inactive.
 *
 * Returns 0 on success, -EINVAL if index is out of range or not active.
 */
int loopback_teardown(int loop_index);

/*
 * Check if a loopback device slot is active.
 * Returns 1 if active, 0 if inactive or index out of range.
 */
int loopback_is_active(int loop_index);

#endif /* PPAP_BLKDEV_LOOPBACK_H */
