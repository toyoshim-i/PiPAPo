/*
 * vfat.c — FAT32 (VFAT) filesystem driver
 *
 * Implements vfs_ops_t for FAT32 partitions with support for:
 *   - BPB parsing and mount
 *   - FAT table lookup with single-sector cache
 *   - Directory traversal (SFN + LFN)
 *   - File read and write
 *   - File/directory creation and deletion
 *
 * All block I/O goes through the blkdev layer.  The driver maintains
 * a single-sector FAT cache for efficiency.
 */

#include "vfat.h"
#include "vfat_format.h"
#include "../vfs/vfs.h"
#include "../blkdev/blkdev.h"
#include "../errno.h"
#include "config.h"
#include <stddef.h>

/* ── Sector buffer (on stack or static) ─────────────────────────────────── */

/* We use a static sector buffer to avoid 512-byte stack allocations.
 * Not re-entrant, but the kernel is single-threaded for FS operations. */
static uint8_t sector_buf[512];

/* ── Superblock storage ─────────────────────────────────────────────────── */

static vfat_sb_t vfat_sb;

/* ── String helpers (no libc) ───────────────────────────────────────────── */

static int str_eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return (*a == '\0' && *b == '\0');
}

static uint32_t str_len(const char *s)
{
    uint32_t len = 0;
    while (s[len]) len++;
    return len;
}

static void str_copy(char *dst, const char *src, uint32_t max)
{
    uint32_t i = 0;
    while (i < max && src[i]) { dst[i] = src[i]; i++; }
    if (i < max) dst[i] = '\0';
}

/* ── Block I/O helpers ──────────────────────────────────────────────────── */

static int read_sector(vfat_sb_t *sb, uint32_t sector, void *buf)
{
    return sb->dev->read(sb->dev, buf, sector, 1);
}

static int write_sector(vfat_sb_t *sb, uint32_t sector, const void *buf)
{
    return sb->dev->write(sb->dev, buf, sector, 1);
}

/* ── FAT table access ───────────────────────────────────────────────────── */

/* Load a FAT sector into the cache.  Flushes dirty cache if needed. */
static int fat_cache_load(vfat_sb_t *sb, uint32_t fat_sector)
{
    if (sb->fat_cache_sector == fat_sector)
        return 0;   /* already cached */

    /* Flush dirty cache to both FATs */
    if (sb->fat_cache_dirty && sb->fat_cache_sector != 0xFFFFFFFFu) {
        for (uint32_t f = 0; f < sb->num_fats; f++) {
            uint32_t s = sb->fat_start_sector + f * sb->fat_size
                       + sb->fat_cache_sector;
            int rc = write_sector(sb, s, sb->fat_cache);
            if (rc < 0) return rc;
        }
        sb->fat_cache_dirty = 0;
    }

    /* Load new sector from FAT1 */
    uint32_t s = sb->fat_start_sector + fat_sector;
    int rc = read_sector(sb, s, sb->fat_cache);
    if (rc < 0) return rc;

    sb->fat_cache_sector = fat_sector;
    return 0;
}

/* Read the FAT entry for a given cluster */
static int fat_get(vfat_sb_t *sb, uint32_t cluster, uint32_t *value)
{
    uint32_t offset = cluster * 4;
    uint32_t fat_sector = offset / 512;
    uint32_t byte_off   = offset % 512;

    int rc = fat_cache_load(sb, fat_sector);
    if (rc < 0) return rc;

    *value = LE32(&sb->fat_cache[byte_off]) & 0x0FFFFFFFu;
    return 0;
}

/* Write a FAT entry for a given cluster */
static int fat_set(vfat_sb_t *sb, uint32_t cluster, uint32_t value)
{
    uint32_t offset = cluster * 4;
    uint32_t fat_sector = offset / 512;
    uint32_t byte_off   = offset % 512;

    int rc = fat_cache_load(sb, fat_sector);
    if (rc < 0) return rc;

    /* Preserve top 4 bits */
    uint32_t old = LE32(&sb->fat_cache[byte_off]);
    uint32_t new_val = (old & 0xF0000000u) | (value & 0x0FFFFFFFu);
    sb->fat_cache[byte_off + 0] = (uint8_t)(new_val);
    sb->fat_cache[byte_off + 1] = (uint8_t)(new_val >> 8);
    sb->fat_cache[byte_off + 2] = (uint8_t)(new_val >> 16);
    sb->fat_cache[byte_off + 3] = (uint8_t)(new_val >> 24);
    sb->fat_cache_dirty = 1;
    return 0;
}

/* Flush FAT cache to disk */
static int fat_flush(vfat_sb_t *sb)
{
    if (!sb->fat_cache_dirty || sb->fat_cache_sector == 0xFFFFFFFFu)
        return 0;
    for (uint32_t f = 0; f < sb->num_fats; f++) {
        uint32_t s = sb->fat_start_sector + f * sb->fat_size
                   + sb->fat_cache_sector;
        int rc = write_sector(sb, s, sb->fat_cache);
        if (rc < 0) return rc;
    }
    sb->fat_cache_dirty = 0;
    return 0;
}

/* Find a free cluster and allocate it (mark as EOC) */
static int fat_alloc_cluster(vfat_sb_t *sb, uint32_t *out)
{
    for (uint32_t c = 2; c < sb->total_clusters + 2; c++) {
        uint32_t val;
        int rc = fat_get(sb, c, &val);
        if (rc < 0) return rc;
        if (val == FAT_FREE) {
            rc = fat_set(sb, c, 0x0FFFFFFFu);  /* EOC */
            if (rc < 0) return rc;
            *out = c;
            return 0;
        }
    }
    return -ENOSPC;
}

/* Free a cluster chain starting at `cluster` */
static int fat_free_chain(vfat_sb_t *sb, uint32_t cluster)
{
    while (cluster >= 2 && !FAT_EOC(cluster)) {
        uint32_t next;
        int rc = fat_get(sb, cluster, &next);
        if (rc < 0) return rc;
        rc = fat_set(sb, cluster, FAT_FREE);
        if (rc < 0) return rc;
        cluster = next;
    }
    return 0;
}

/* ── SFN decoding ───────────────────────────────────────────────────────── */

/* Decode an 8.3 SFN entry into a lowercase null-terminated name */
static void sfn_decode(const uint8_t name[11], char *out)
{
    int p = 0;
    /* Base name (8 chars, right-trimmed, lowercased) */
    for (int i = 0; i < 8; i++) {
        if (name[i] == ' ') break;
        char c = (char)name[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        out[p++] = c;
    }
    /* Extension (3 chars) */
    if (name[8] != ' ') {
        out[p++] = '.';
        for (int i = 8; i < 11; i++) {
            if (name[i] == ' ') break;
            char c = (char)name[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            out[p++] = c;
        }
    }
    out[p] = '\0';
}

/* ── LFN decoding ───────────────────────────────────────────────────────── */

/* Extract chars from a single LFN entry, appending to `name` at `pos`.
 * Returns the number of chars extracted. */
static int lfn_extract(const fat_lfn_entry_t *lfn, char *name, int pos)
{
    /* Each LFN entry has 13 UCS-2 characters across name1/name2/name3 */
    uint16_t chars[13];
    for (int i = 0; i < 5; i++) chars[i]     = lfn->name1[i];
    for (int i = 0; i < 6; i++) chars[i + 5]  = lfn->name2[i];
    for (int i = 0; i < 2; i++) chars[i + 11] = lfn->name3[i];

    int count = 0;
    for (int i = 0; i < 13; i++) {
        if (chars[i] == 0x0000u || chars[i] == 0xFFFFu)
            break;
        /* Truncate UCS-2 to ASCII */
        if (pos + count < VFS_NAME_MAX)
            name[pos + count] = (char)(chars[i] & 0x7Fu);
        count++;
    }
    return count;
}

/* ── SFN generation (for creating files) ─────────────────────────────────── */

/* Generate an 8.3 SFN from a long name.  Simple approach: take first
 * 6 chars + "~1", extension from last ".".  No collision detection. */
static void sfn_generate(const char *name, uint8_t sfn[11])
{
    /* Fill with spaces */
    for (int i = 0; i < 11; i++) sfn[i] = ' ';

    /* Find last dot for extension */
    int dot = -1;
    for (int i = (int)str_len(name) - 1; i >= 0; i--) {
        if (name[i] == '.') { dot = i; break; }
    }

    /* Extension */
    if (dot >= 0) {
        int ep = 0;
        for (int i = dot + 1; name[i] && ep < 3; i++, ep++) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            sfn[8 + ep] = (uint8_t)c;
        }
    }

    /* Base name */
    int baselen = dot >= 0 ? dot : (int)str_len(name);
    if (baselen <= 8) {
        /* Fits directly */
        for (int i = 0; i < baselen && i < 8; i++) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c == ' ' || c == '.') c = '_';
            sfn[i] = (uint8_t)c;
        }
    } else {
        /* Needs ~1 suffix */
        for (int i = 0; i < 6; i++) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c == ' ' || c == '.') c = '_';
            sfn[i] = (uint8_t)c;
        }
        sfn[6] = '~';
        sfn[7] = '1';
    }
}

/* ── Private vnode data ─────────────────────────────────────────────────── */

/* We store the cluster number in the vnode's ino field (repurposed).
 * fs_priv points to the superblock. */

#define VNODE_CLUSTER(vn)  ((vn)->ino)

/* ── vfat_mount ─────────────────────────────────────────────────────────── */

static int vfat_mount(mount_entry_t *mnt, const void *dev_data)
{
    blkdev_t *dev = (blkdev_t *)dev_data;
    if (!dev)
        return -EINVAL;

    /* Read sector 0 (BPB) */
    int rc = dev->read(dev, sector_buf, 0, 1);
    if (rc < 0) return rc;

    fat32_bpb_t *bpb = (fat32_bpb_t *)sector_buf;

    /* Validate */
    if (bpb->bytes_per_sector != 512)
        return -EINVAL;
    if (bpb->sectors_per_cluster == 0)
        return -EINVAL;

    /* Populate superblock */
    vfat_sb_t *sb = &vfat_sb;
    sb->dev = dev;
    sb->part_lba = 0;
    sb->sectors_per_cluster = bpb->sectors_per_cluster;
    sb->bytes_per_cluster = (uint32_t)bpb->sectors_per_cluster * 512u;
    sb->num_fats = bpb->num_fats;
    sb->fat_start_sector = bpb->reserved_sectors;
    sb->fat_size = bpb->fat_size_32;
    sb->root_cluster = bpb->root_cluster;
    sb->data_start_sector = bpb->reserved_sectors
                          + bpb->num_fats * bpb->fat_size_32;

    uint32_t total_sectors = bpb->total_sectors_32;
    uint32_t data_sectors = total_sectors - sb->data_start_sector;
    sb->total_clusters = data_sectors / bpb->sectors_per_cluster;

    sb->fat_cache_sector = 0xFFFFFFFFu;
    sb->fat_cache_dirty = 0;

    /* Allocate root vnode */
    vnode_t *root = vnode_alloc();
    if (!root) return -ENOMEM;

    root->type = VNODE_DIR;
    root->mode = S_IFDIR | 0755u;
    root->ino  = sb->root_cluster;
    root->size = 0;
    root->mount = mnt;
    root->fs_priv = sb;

    mnt->root = root;
    mnt->sb_priv = sb;
    return 0;
}

/* ── vfat_lookup ────────────────────────────────────────────────────────── */

static int vfat_lookup(vnode_t *dir, const char *name, vnode_t **result)
{
    vfat_sb_t *sb = (vfat_sb_t *)dir->fs_priv;
    uint32_t cluster = VNODE_CLUSTER(dir);

    char lfn_name[VFS_NAME_MAX + 1];
    int lfn_len = 0;
    int collecting_lfn = 0;

    /* Walk directory cluster chain */
    while (cluster >= 2 && !FAT_EOC(cluster)) {
        uint32_t sec = CLUSTER_TO_SECTOR(sb, cluster);
        for (uint32_t s = 0; s < sb->sectors_per_cluster; s++) {
            int rc = read_sector(sb, sec + s, sector_buf);
            if (rc < 0) return rc;

            for (uint32_t off = 0; off < 512; off += 32) {
                fat_dirent_t *de = (fat_dirent_t *)&sector_buf[off];

                if (de->name[0] == FAT_DIRENT_END)
                    return -ENOENT;
                if (de->name[0] == FAT_DIRENT_FREE) {
                    collecting_lfn = 0;
                    continue;
                }

                /* LFN entry */
                if (de->attr == FAT_ATTR_LONG_NAME) {
                    fat_lfn_entry_t *lfn = (fat_lfn_entry_t *)de;
                    int order = lfn->order & 0x3Fu;
                    if (lfn->order & FAT_LFN_LAST_ENTRY) {
                        /* Start of a new LFN sequence */
                        lfn_len = 0;
                        collecting_lfn = 1;
                        for (int i = 0; i <= VFS_NAME_MAX; i++)
                            lfn_name[i] = '\0';
                    }
                    if (collecting_lfn) {
                        int pos = (order - 1) * FAT_LFN_CHARS_PER_ENTRY;
                        lfn_extract(lfn, lfn_name, pos);
                        int end = pos + FAT_LFN_CHARS_PER_ENTRY;
                        if (end > lfn_len) lfn_len = end;
                    }
                    continue;
                }

                /* Skip volume label */
                if (de->attr & FAT_ATTR_VOLUME_ID) {
                    collecting_lfn = 0;
                    continue;
                }

                /* Regular SFN entry — check for match */
                char sfn_name[13];
                sfn_decode(de->name, sfn_name);

                /* Determine display name: use LFN if available */
                const char *dname;
                if (collecting_lfn && lfn_len > 0) {
                    /* Ensure null termination */
                    if (lfn_len <= VFS_NAME_MAX)
                        lfn_name[lfn_len] = '\0';
                    else
                        lfn_name[VFS_NAME_MAX] = '\0';
                    dname = lfn_name;
                } else {
                    dname = sfn_name;
                }
                collecting_lfn = 0;

                if (str_eq_ci(dname, name)) {
                    /* Match found — allocate vnode */
                    vnode_t *vn = vnode_alloc();
                    if (!vn) return -ENOMEM;

                    uint32_t clus = DIRENT_CLUSTER(de);
                    vn->ino = clus;
                    vn->fs_priv = sb;
                    vn->mount = dir->mount;

                    if (de->attr & FAT_ATTR_DIRECTORY) {
                        vn->type = VNODE_DIR;
                        vn->mode = S_IFDIR | 0755u;
                        vn->size = 0;
                    } else {
                        vn->type = VNODE_FILE;
                        vn->mode = S_IFREG | 0644u;
                        vn->size = de->file_size;
                    }

                    *result = vn;
                    return 0;
                }
            }
        }

        /* Next cluster in chain */
        uint32_t next;
        int rc = fat_get(sb, cluster, &next);
        if (rc < 0) return rc;
        cluster = next;
    }

    return -ENOENT;
}

/* ── vfat_read ──────────────────────────────────────────────────────────── */

static long vfat_read(vnode_t *vn, void *buf, size_t n, uint32_t off)
{
    if (vn->type == VNODE_DIR)
        return -(long)EISDIR;

    vfat_sb_t *sb = (vfat_sb_t *)vn->fs_priv;
    uint32_t cluster = VNODE_CLUSTER(vn);
    uint32_t file_size = vn->size;
    uint8_t *dst = (uint8_t *)buf;

    if (off >= file_size)
        return 0;
    if (off + n > file_size)
        n = file_size - off;

    /* Skip clusters to reach the starting offset */
    uint32_t skip_bytes = off;
    while (skip_bytes >= sb->bytes_per_cluster) {
        uint32_t next;
        int rc = fat_get(sb, cluster, &next);
        if (rc < 0) return (long)rc;
        if (FAT_EOC(next)) return 0;  /* premature end of chain */
        cluster = next;
        skip_bytes -= sb->bytes_per_cluster;
    }

    /* Read data */
    size_t total = 0;
    while (total < n && cluster >= 2 && !FAT_EOC(cluster)) {
        uint32_t sec_base = CLUSTER_TO_SECTOR(sb, cluster);
        uint32_t sec_off  = skip_bytes / 512;
        uint32_t byte_off = skip_bytes % 512;
        skip_bytes = 0;

        for (uint32_t s = sec_off; s < sb->sectors_per_cluster && total < n; s++) {
            int rc = read_sector(sb, sec_base + s, sector_buf);
            if (rc < 0) return (long)rc;

            uint32_t start = (s == sec_off) ? byte_off : 0;
            uint32_t avail = 512 - start;
            if (avail > n - total) avail = (uint32_t)(n - total);
            __builtin_memcpy(dst + total, &sector_buf[start], avail);
            total += avail;
        }

        /* Next cluster */
        uint32_t next;
        int rc = fat_get(sb, cluster, &next);
        if (rc < 0) return (long)rc;
        cluster = next;
    }

    return (long)total;
}

/* ── vfat_write ─────────────────────────────────────────────────────────── */

static long vfat_write(vnode_t *vn, const void *buf, size_t n, uint32_t off)
{
    if (vn->type == VNODE_DIR)
        return -(long)EISDIR;

    vfat_sb_t *sb = (vfat_sb_t *)vn->fs_priv;
    uint32_t cluster = VNODE_CLUSTER(vn);
    const uint8_t *src = (const uint8_t *)buf;

    /* If file has no cluster yet, allocate one */
    if (cluster < 2) {
        uint32_t new_clus;
        int rc = fat_alloc_cluster(sb, &new_clus);
        if (rc < 0) return (long)rc;
        cluster = new_clus;
        vn->ino = new_clus;
        /* TODO: update directory entry's cluster field */
    }

    /* Navigate to the cluster containing `off` */
    uint32_t skip_bytes = off;
    uint32_t prev_cluster = 0;
    while (skip_bytes >= sb->bytes_per_cluster) {
        uint32_t next;
        int rc = fat_get(sb, cluster, &next);
        if (rc < 0) return (long)rc;
        if (FAT_EOC(next)) {
            /* Need to extend the chain */
            uint32_t new_clus;
            rc = fat_alloc_cluster(sb, &new_clus);
            if (rc < 0) return (long)rc;
            rc = fat_set(sb, cluster, new_clus);
            if (rc < 0) return (long)rc;
            next = new_clus;
        }
        prev_cluster = cluster;
        cluster = next;
        skip_bytes -= sb->bytes_per_cluster;
    }

    /* Write data */
    size_t total = 0;
    while (total < n) {
        if (cluster < 2 || FAT_EOC(cluster)) {
            /* Extend chain */
            uint32_t new_clus;
            int rc = fat_alloc_cluster(sb, &new_clus);
            if (rc < 0) break;
            if (prev_cluster >= 2) {
                rc = fat_set(sb, prev_cluster, new_clus);
                if (rc < 0) break;
            }
            cluster = new_clus;
            skip_bytes = 0;
        }

        uint32_t sec_base = CLUSTER_TO_SECTOR(sb, cluster);
        uint32_t sec_off  = skip_bytes / 512;
        uint32_t byte_off = skip_bytes % 512;
        skip_bytes = 0;

        for (uint32_t s = sec_off; s < sb->sectors_per_cluster && total < n; s++) {
            uint32_t start = (s == sec_off) ? byte_off : 0;
            uint32_t avail = 512 - start;
            if (avail > n - total) avail = (uint32_t)(n - total);

            /* Read-modify-write if partial sector */
            if (start != 0 || avail != 512) {
                int rc = read_sector(sb, sec_base + s, sector_buf);
                if (rc < 0) return (long)((total > 0) ? total : rc);
            }
            __builtin_memcpy(&sector_buf[start], src + total, avail);
            int rc = write_sector(sb, sec_base + s, sector_buf);
            if (rc < 0) return (long)((total > 0) ? total : rc);
            total += avail;
        }

        /* Next cluster */
        prev_cluster = cluster;
        uint32_t next;
        int rc = fat_get(sb, cluster, &next);
        if (rc < 0) break;
        cluster = next;
    }

    /* Update file size if we wrote past the end */
    if (off + total > vn->size)
        vn->size = off + (uint32_t)total;

    fat_flush(sb);
    return (long)total;
}

/* ── vfat_readdir ───────────────────────────────────────────────────────── */

static int vfat_readdir(vnode_t *dir, struct dirent *entries,
                        size_t max_entries, uint32_t *cookie)
{
    vfat_sb_t *sb = (vfat_sb_t *)dir->fs_priv;
    uint32_t cluster = VNODE_CLUSTER(dir);
    int count = 0;

    char lfn_name[VFS_NAME_MAX + 1];
    int lfn_len = 0;
    int collecting_lfn = 0;

    /* Skip to the cookie position */
    uint32_t entry_idx = 0;
    uint32_t target_idx = *cookie;

    while (cluster >= 2 && !FAT_EOC(cluster)) {
        uint32_t sec = CLUSTER_TO_SECTOR(sb, cluster);
        for (uint32_t s = 0; s < sb->sectors_per_cluster; s++) {
            int rc = read_sector(sb, sec + s, sector_buf);
            if (rc < 0) return rc;

            for (uint32_t off = 0; off < 512; off += 32) {
                fat_dirent_t *de = (fat_dirent_t *)&sector_buf[off];

                if (de->name[0] == FAT_DIRENT_END)
                    goto done;
                if (de->name[0] == FAT_DIRENT_FREE) {
                    collecting_lfn = 0;
                    continue;
                }

                if (de->attr == FAT_ATTR_LONG_NAME) {
                    fat_lfn_entry_t *lfn = (fat_lfn_entry_t *)de;
                    int order = lfn->order & 0x3Fu;
                    if (lfn->order & FAT_LFN_LAST_ENTRY) {
                        lfn_len = 0;
                        collecting_lfn = 1;
                        for (int i = 0; i <= VFS_NAME_MAX; i++)
                            lfn_name[i] = '\0';
                    }
                    if (collecting_lfn) {
                        int pos = (order - 1) * FAT_LFN_CHARS_PER_ENTRY;
                        lfn_extract(lfn, lfn_name, pos);
                        int end = pos + FAT_LFN_CHARS_PER_ENTRY;
                        if (end > lfn_len) lfn_len = end;
                    }
                    continue;
                }

                if (de->attr & FAT_ATTR_VOLUME_ID) {
                    collecting_lfn = 0;
                    continue;
                }

                /* Visible entry — skip "." and ".." */
                if (de->name[0] == '.' &&
                    (de->name[1] == ' ' || de->name[1] == '.')) {
                    collecting_lfn = 0;
                    entry_idx++;
                    continue;
                }

                if (entry_idx >= target_idx) {
                    if ((size_t)count >= max_entries)
                        goto done;

                    /* Determine display name */
                    char sfn_name[13];
                    sfn_decode(de->name, sfn_name);

                    const char *dname;
                    if (collecting_lfn && lfn_len > 0) {
                        if (lfn_len <= VFS_NAME_MAX)
                            lfn_name[lfn_len] = '\0';
                        else
                            lfn_name[VFS_NAME_MAX] = '\0';
                        dname = lfn_name;
                    } else {
                        dname = sfn_name;
                    }

                    entries[count].d_ino = DIRENT_CLUSTER(de);
                    entries[count].d_type = (de->attr & FAT_ATTR_DIRECTORY)
                                          ? DT_DIR : DT_REG;
                    str_copy(entries[count].d_name, dname, VFS_NAME_MAX);
                    entries[count].d_name[VFS_NAME_MAX] = '\0';
                    count++;
                }

                collecting_lfn = 0;
                entry_idx++;
            }
        }

        uint32_t next;
        int rc = fat_get(sb, cluster, &next);
        if (rc < 0) return rc;
        cluster = next;
    }

done:
    *cookie = target_idx + (uint32_t)count;
    return count;
}

/* ── vfat_stat ──────────────────────────────────────────────────────────── */

static int vfat_stat(vnode_t *vn, struct stat *st)
{
    st->st_ino   = vn->ino;
    st->st_mode  = vn->mode;
    st->st_nlink = 1;
    st->st_size  = vn->size;
    return 0;
}

/* ── vfat_create — create a new file in a directory ──────────────────────── */

static int vfat_create(vnode_t *dir, const char *name, uint32_t mode,
                       vnode_t **result)
{
    (void)mode;
    vfat_sb_t *sb = (vfat_sb_t *)dir->fs_priv;
    uint32_t dir_cluster = VNODE_CLUSTER(dir);

    /* Allocate a cluster for the new file */
    uint32_t file_cluster;
    int rc = fat_alloc_cluster(sb, &file_cluster);
    if (rc < 0) return rc;

    /* Generate SFN */
    uint8_t sfn[11];
    sfn_generate(name, sfn);

    /* Find a free directory entry slot */
    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && !FAT_EOC(cluster)) {
        uint32_t sec = CLUSTER_TO_SECTOR(sb, cluster);
        for (uint32_t s = 0; s < sb->sectors_per_cluster; s++) {
            rc = read_sector(sb, sec + s, sector_buf);
            if (rc < 0) return rc;

            for (uint32_t off = 0; off < 512; off += 32) {
                fat_dirent_t *de = (fat_dirent_t *)&sector_buf[off];
                if (de->name[0] == FAT_DIRENT_END ||
                    de->name[0] == FAT_DIRENT_FREE) {
                    /* Found a free slot — write the entry */
                    __builtin_memset(de, 0, 32);
                    __builtin_memcpy(de->name, sfn, 11);
                    de->attr = FAT_ATTR_ARCHIVE;
                    de->first_cluster_hi = (uint16_t)(file_cluster >> 16);
                    de->first_cluster_lo = (uint16_t)(file_cluster & 0xFFFF);
                    de->file_size = 0;

                    rc = write_sector(sb, sec + s, sector_buf);
                    if (rc < 0) return rc;

                    fat_flush(sb);

                    /* Allocate vnode */
                    vnode_t *vn = vnode_alloc();
                    if (!vn) return -ENOMEM;
                    vn->type = VNODE_FILE;
                    vn->mode = S_IFREG | 0644u;
                    vn->ino = file_cluster;
                    vn->size = 0;
                    vn->fs_priv = sb;
                    vn->mount = dir->mount;
                    *result = vn;
                    return 0;
                }
            }
        }

        uint32_t next;
        rc = fat_get(sb, cluster, &next);
        if (rc < 0) return rc;
        cluster = next;
    }

    return -ENOSPC;  /* no free directory entries */
}

/* ── vfat_mkdir ─────────────────────────────────────────────────────────── */

static int vfat_mkdir(vnode_t *dir, const char *name, uint32_t mode)
{
    (void)mode;
    vfat_sb_t *sb = (vfat_sb_t *)dir->fs_priv;
    uint32_t dir_cluster = VNODE_CLUSTER(dir);

    /* Allocate cluster for new directory */
    uint32_t new_cluster;
    int rc = fat_alloc_cluster(sb, &new_cluster);
    if (rc < 0) return rc;

    /* Initialize new directory with . and .. entries */
    __builtin_memset(sector_buf, 0, 512);
    fat_dirent_t *dot = (fat_dirent_t *)&sector_buf[0];
    __builtin_memcpy(dot->name, ".          ", 11);
    dot->attr = FAT_ATTR_DIRECTORY;
    dot->first_cluster_hi = (uint16_t)(new_cluster >> 16);
    dot->first_cluster_lo = (uint16_t)(new_cluster & 0xFFFF);

    fat_dirent_t *dotdot = (fat_dirent_t *)&sector_buf[32];
    __builtin_memcpy(dotdot->name, "..         ", 11);
    dotdot->attr = FAT_ATTR_DIRECTORY;
    dotdot->first_cluster_hi = (uint16_t)(dir_cluster >> 16);
    dotdot->first_cluster_lo = (uint16_t)(dir_cluster & 0xFFFF);

    uint32_t sec = CLUSTER_TO_SECTOR(sb, new_cluster);
    rc = write_sector(sb, sec, sector_buf);
    if (rc < 0) return rc;

    /* Zero remaining sectors in the cluster */
    if (sb->sectors_per_cluster > 1) {
        __builtin_memset(sector_buf, 0, 512);
        for (uint32_t s = 1; s < sb->sectors_per_cluster; s++) {
            rc = write_sector(sb, sec + s, sector_buf);
            if (rc < 0) return rc;
        }
    }

    /* Add entry in parent directory */
    uint8_t sfn[11];
    sfn_generate(name, sfn);

    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && !FAT_EOC(cluster)) {
        uint32_t psec = CLUSTER_TO_SECTOR(sb, cluster);
        for (uint32_t s = 0; s < sb->sectors_per_cluster; s++) {
            rc = read_sector(sb, psec + s, sector_buf);
            if (rc < 0) return rc;

            for (uint32_t off = 0; off < 512; off += 32) {
                fat_dirent_t *de = (fat_dirent_t *)&sector_buf[off];
                if (de->name[0] == FAT_DIRENT_END ||
                    de->name[0] == FAT_DIRENT_FREE) {
                    __builtin_memset(de, 0, 32);
                    __builtin_memcpy(de->name, sfn, 11);
                    de->attr = FAT_ATTR_DIRECTORY;
                    de->first_cluster_hi = (uint16_t)(new_cluster >> 16);
                    de->first_cluster_lo = (uint16_t)(new_cluster & 0xFFFF);

                    rc = write_sector(sb, psec + s, sector_buf);
                    if (rc < 0) return rc;
                    fat_flush(sb);
                    return 0;
                }
            }
        }

        uint32_t next;
        rc = fat_get(sb, cluster, &next);
        if (rc < 0) return rc;
        cluster = next;
    }

    return -ENOSPC;
}

/* ── vfat_unlink — delete a file ─────────────────────────────────────────── */

static int vfat_unlink(vnode_t *dir, const char *name)
{
    vfat_sb_t *sb = (vfat_sb_t *)dir->fs_priv;
    uint32_t cluster = VNODE_CLUSTER(dir);

    char lfn_name[VFS_NAME_MAX + 1];
    int lfn_len = 0;
    int collecting_lfn = 0;

    while (cluster >= 2 && !FAT_EOC(cluster)) {
        uint32_t sec = CLUSTER_TO_SECTOR(sb, cluster);
        for (uint32_t s = 0; s < sb->sectors_per_cluster; s++) {
            int rc = read_sector(sb, sec + s, sector_buf);
            if (rc < 0) return rc;

            for (uint32_t off = 0; off < 512; off += 32) {
                fat_dirent_t *de = (fat_dirent_t *)&sector_buf[off];

                if (de->name[0] == FAT_DIRENT_END)
                    return -ENOENT;
                if (de->name[0] == FAT_DIRENT_FREE) {
                    collecting_lfn = 0;
                    continue;
                }

                if (de->attr == FAT_ATTR_LONG_NAME) {
                    fat_lfn_entry_t *lfn = (fat_lfn_entry_t *)de;
                    int order = lfn->order & 0x3Fu;
                    if (lfn->order & FAT_LFN_LAST_ENTRY) {
                        lfn_len = 0;
                        collecting_lfn = 1;
                        for (int i = 0; i <= VFS_NAME_MAX; i++)
                            lfn_name[i] = '\0';
                    }
                    if (collecting_lfn) {
                        int pos = (order - 1) * FAT_LFN_CHARS_PER_ENTRY;
                        lfn_extract(lfn, lfn_name, pos);
                        int end = pos + FAT_LFN_CHARS_PER_ENTRY;
                        if (end > lfn_len) lfn_len = end;
                    }
                    continue;
                }

                if (de->attr & FAT_ATTR_VOLUME_ID) {
                    collecting_lfn = 0;
                    continue;
                }

                /* Check for name match */
                char sfn_name_buf[13];
                sfn_decode(de->name, sfn_name_buf);
                const char *dname;
                if (collecting_lfn && lfn_len > 0) {
                    if (lfn_len <= VFS_NAME_MAX)
                        lfn_name[lfn_len] = '\0';
                    else
                        lfn_name[VFS_NAME_MAX] = '\0';
                    dname = lfn_name;
                } else {
                    dname = sfn_name_buf;
                }
                collecting_lfn = 0;

                if (str_eq_ci(dname, name)) {
                    /* Cannot unlink directories with unlink */
                    if (de->attr & FAT_ATTR_DIRECTORY)
                        return -EISDIR;

                    /* Free cluster chain */
                    uint32_t file_clus = DIRENT_CLUSTER(de);
                    if (file_clus >= 2) {
                        rc = fat_free_chain(sb, file_clus);
                        if (rc < 0) return rc;
                    }

                    /* Mark directory entry as deleted */
                    de->name[0] = FAT_DIRENT_FREE;
                    rc = write_sector(sb, sec + s, sector_buf);
                    if (rc < 0) return rc;

                    fat_flush(sb);
                    return 0;
                }
            }
        }

        uint32_t next;
        int rc = fat_get(sb, cluster, &next);
        if (rc < 0) return rc;
        cluster = next;
    }

    return -ENOENT;
}

/* ── Operations table ───────────────────────────────────────────────────── */

const vfs_ops_t vfat_ops = {
    .mount    = vfat_mount,
    .lookup   = vfat_lookup,
    .read     = vfat_read,
    .write    = vfat_write,
    .readdir  = vfat_readdir,
    .stat     = vfat_stat,
    .readlink = NULL,
    .create   = vfat_create,
    .mkdir    = vfat_mkdir,
    .unlink   = vfat_unlink,
    .truncate = NULL,
};
