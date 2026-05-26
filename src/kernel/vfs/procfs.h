/*
 * procfs.h — Process information pseudo-filesystem interface
 *
 * Provides procfs_ops — a vfs_ops_t for mounting a RAM-resident process
 * information filesystem at /proc.  All entries are generated dynamically
 * on each read.
 */

#ifndef PPAP_KERNEL_VFS_PROCFS_H
#define PPAP_KERNEL_VFS_PROCFS_H

#include "kernel/common/mod/mod_vfs.h"

/* FS operations table — pass to vfs_mount() as the ops parameter */
extern const vfs_ops_t procfs_ops;

/* Register hardware battery read callback (pico1calc I2C).
 * Boot-only: call from target_late_init() before sched_start().
 * read_fn(buf, len): read 1 byte into buf[0].
 *   bits 0-6 = percentage (0-100), bit 7 = charging flag.
 * /proc/battery shows "not available" if not registered. */
void procfs_set_battery(int (*read_fn)(uint8_t *buf, int len));

#endif /* PPAP_KERNEL_VFS_PROCFS_H */
