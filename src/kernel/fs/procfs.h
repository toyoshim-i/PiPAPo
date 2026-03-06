/*
 * procfs.h — Process information pseudo-filesystem interface
 *
 * Provides procfs_ops — a vfs_ops_t for mounting a RAM-resident process
 * information filesystem at /proc.  All entries are generated dynamically
 * on each read.
 */

#ifndef PPAP_FS_PROCFS_H
#define PPAP_FS_PROCFS_H

#include "../vfs/vfs.h"

/* FS operations table — pass to vfs_mount() as the ops parameter */
extern const vfs_ops_t procfs_ops;

/* Register hardware battery read callback (pico1calc I2C).
 * read_fn(buf, len): read len bytes from battery register into buf.
 * /proc/battery shows "not available" if not registered. */
void procfs_set_battery(int (*read_fn)(uint8_t *buf, int len));

#endif /* PPAP_FS_PROCFS_H */
