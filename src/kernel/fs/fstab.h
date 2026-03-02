/*
 * fstab.h — Boot-time mount table parser
 *
 * Reads /etc/fstab from romfs and mounts filesystems in order.
 */

#ifndef PPAP_FS_FSTAB_H
#define PPAP_FS_FSTAB_H

#include <stdint.h>

#define FSTAB_MAX_ENTRIES 8

typedef struct {
    char     device[64];       /* device path or blkdev name */
    char     mountpoint[64];   /* mount point path */
    char     fstype[16];       /* "vfat", "ufs", "tmpfs", etc. */
    uint8_t  flags;            /* MNT_RDONLY etc. */
    uint8_t  loop;             /* 1 if "loop" option present */
} fstab_entry_t;

/* Parse /etc/fstab into an array of entries.
 * Returns number of entries parsed, or negative errno. */
int fstab_parse(fstab_entry_t *entries, int max_entries);

/* Mount all entries in order.  Skips entries that fail (graceful).
 * Returns 0 always (best-effort). */
int fstab_mount_all(const fstab_entry_t *entries, int count);

#endif /* PPAP_FS_FSTAB_H */
