/*
 * devfs.h — Device pseudo-filesystem interface
 *
 * Provides devfs_ops — a vfs_ops_t for mounting a RAM-resident device
 * filesystem at /dev.  All entries are created at mount time from a
 * static table; there is no on-disk backing.
 */

#ifndef PPAP_KERNEL_VFS_DEVFS_H
#define PPAP_KERNEL_VFS_DEVFS_H

#include "kernel/common/mod/mod_vfs.h"

/* FS operations table — pass to vfs_mount() as the ops parameter */
extern const vfs_ops_t devfs_ops;

/* Register hardware backlight callbacks (pico1calc I2C).
 * Boot-only: call from target_late_init() before sched_start().
 * get: read current brightness (0–255) into *val; return 0 or -1.
 * set: write brightness (0–255); return 0 or -1.
 * /dev/backlight returns ENODEV if not registered. */
void devfs_set_backlight(int (*get)(uint8_t *val), int (*set)(uint8_t val));

/* Register hardware power-off callback (pico1calc I2C).
 * Boot-only: call from target_late_init() before sched_start().
 * off_fn: trigger power-down; should not return.
 * /dev/power write returns ENODEV if not registered. */
void devfs_set_power(int (*off_fn)(void));

#endif /* PPAP_KERNEL_VFS_DEVFS_H */
