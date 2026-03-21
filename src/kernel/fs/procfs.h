/*
 * procfs.h — Process information pseudo-filesystem interface
 *
 * Provides procfs_ops — a vfs_ops_t for mounting a RAM-resident process
 * information filesystem at /proc.  All entries are generated dynamically
 * on each read.
 */

#ifndef PPAP_KERNEL_FS_PROCFS_H
#define PPAP_KERNEL_FS_PROCFS_H

#include "../vfs/vfs.h"

/* FS operations table — pass to vfs_mount() as the ops parameter */
extern const vfs_ops_t procfs_ops;

/* Register hardware battery read callback (pico1calc I2C).
 * read_fn(buf, len): read 1 byte into buf[0].
 *   bits 0-6 = percentage (0-100), bit 7 = charging flag.
 * /proc/battery shows "not available" if not registered. */
void procfs_set_battery(int (*read_fn)(uint8_t *buf, int len));

/* Register an eCPU core name for /proc/ecpu listing.
 * Call once per core at boot (e.g. from subsystem init). */
void procfs_register_ecpu(const char *name);

/* Register a subsystem name for /proc/subsys and /proc/<pid>/subsys.
 * @tag: SUBSYS_xxx index, @name: human-readable name string (static). */
void procfs_register_subsys(uint8_t tag, const char *name);

#endif /* PPAP_KERNEL_FS_PROCFS_H */
