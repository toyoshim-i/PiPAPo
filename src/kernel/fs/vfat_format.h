/*
 * vfat_format.h — On-disk FAT32 data structures
 *
 * All structures are packed and little-endian, matching the on-disk layout
 * of a FAT32 filesystem.  Used by vfat.c for BPB parsing, directory entry
 * reading, and FAT table manipulation.
 *
 * References:
 *   - Microsoft FAT32 File System Specification (fatgen103.doc)
 *   - Microsoft Extensible Firmware Initiative FAT32 FS Spec v1.03
 */

#ifndef PPAP_FS_VFAT_FORMAT_H
#define PPAP_FS_VFAT_FORMAT_H

#include <stdint.h>

/* ── BPB (BIOS Parameter Block) ──────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  jmp_boot[3];        /* 0x00: jump instruction            */
    uint8_t  oem_name[8];        /* 0x03: OEM name ("MSWIN4.1", etc.) */
    uint16_t bytes_per_sector;   /* 0x0B: must be 512                 */
    uint8_t  sectors_per_cluster;/* 0x0D: 1, 2, 4, 8, 16, 32, 64     */
    uint16_t reserved_sectors;   /* 0x0E: sectors before first FAT    */
    uint8_t  num_fats;           /* 0x10: number of FATs (usually 2)  */
    uint16_t root_entry_count;   /* 0x11: 0 for FAT32                 */
    uint16_t total_sectors_16;   /* 0x13: 0 for FAT32                 */
    uint8_t  media_type;         /* 0x15: 0xF8 = fixed disk           */
    uint16_t fat_size_16;        /* 0x16: 0 for FAT32                 */
    uint16_t sectors_per_track;  /* 0x18: geometry (ignored)          */
    uint16_t num_heads;          /* 0x1A: geometry (ignored)          */
    uint32_t hidden_sectors;     /* 0x1C: sectors before partition     */
    uint32_t total_sectors_32;   /* 0x20: total sectors               */

    /* FAT32-specific fields (offset 0x24) */
    uint32_t fat_size_32;        /* 0x24: sectors per FAT             */
    uint16_t ext_flags;          /* 0x28: mirror flags                */
    uint16_t fs_version;         /* 0x2A: version (0x0000)            */
    uint32_t root_cluster;       /* 0x2C: first cluster of root dir   */
    uint16_t fs_info_sector;     /* 0x30: FSInfo sector number        */
    uint16_t backup_boot_sector; /* 0x32: backup boot sector          */
    uint8_t  reserved[12];       /* 0x34: reserved                    */
    uint8_t  drive_number;       /* 0x40: BIOS drive number           */
    uint8_t  reserved1;          /* 0x41: reserved                    */
    uint8_t  boot_sig;           /* 0x42: 0x29 if next 3 fields valid */
    uint32_t volume_id;          /* 0x43: serial number               */
    uint8_t  volume_label[11];   /* 0x47: volume label                */
    uint8_t  fs_type[8];         /* 0x52: "FAT32   "                  */
} fat32_bpb_t;

/* ── Directory entry (32 bytes) ──────────────────────────────────────────── */

/* Attribute flags */
#define FAT_ATTR_READ_ONLY  0x01u
#define FAT_ATTR_HIDDEN     0x02u
#define FAT_ATTR_SYSTEM     0x04u
#define FAT_ATTR_VOLUME_ID  0x08u
#define FAT_ATTR_DIRECTORY  0x10u
#define FAT_ATTR_ARCHIVE    0x20u
#define FAT_ATTR_LONG_NAME  0x0Fu  /* combination for LFN entries */

typedef struct __attribute__((packed)) {
    uint8_t  name[11];           /* 0x00: 8.3 short filename          */
    uint8_t  attr;               /* 0x0B: attribute flags             */
    uint8_t  nt_reserved;        /* 0x0C: case info (NT extension)    */
    uint8_t  crt_time_tenth;     /* 0x0D: creation time tenths        */
    uint16_t crt_time;           /* 0x0E: creation time               */
    uint16_t crt_date;           /* 0x10: creation date               */
    uint16_t last_acc_date;      /* 0x12: last access date            */
    uint16_t first_cluster_hi;   /* 0x14: high 16 bits of cluster     */
    uint16_t wrt_time;           /* 0x16: last write time             */
    uint16_t wrt_date;           /* 0x18: last write date             */
    uint16_t first_cluster_lo;   /* 0x1A: low 16 bits of cluster      */
    uint32_t file_size;          /* 0x1C: file size in bytes          */
} fat_dirent_t;

/* Special name[0] values */
#define FAT_DIRENT_FREE     0xE5u   /* deleted entry                  */
#define FAT_DIRENT_END      0x00u   /* end of directory                */
#define FAT_DIRENT_KANJI    0x05u   /* actual first byte is 0xE5      */

/* ── Long File Name (LFN) entry (32 bytes, same size as dirent) ──────────── */

#define FAT_LFN_CHARS_PER_ENTRY  13  /* UCS-2 chars per LFN entry     */
#define FAT_LFN_LAST_ENTRY       0x40u /* bit set on last (highest) LFN entry */

typedef struct __attribute__((packed)) {
    uint8_t  order;              /* 0x00: sequence number (1-based, |0x40 on last) */
    uint16_t name1[5];           /* 0x01: UCS-2 chars 1–5            */
    uint8_t  attr;               /* 0x0B: must be FAT_ATTR_LONG_NAME */
    uint8_t  type;               /* 0x0C: 0 = sub-entry of LFN       */
    uint8_t  checksum;           /* 0x0D: SFN checksum               */
    uint16_t name2[6];           /* 0x0E: UCS-2 chars 6–11           */
    uint16_t first_cluster_lo;   /* 0x1A: must be 0                  */
    uint16_t name3[2];           /* 0x1C: UCS-2 chars 12–13          */
} fat_lfn_entry_t;

/* ── In-memory VFAT superblock ───────────────────────────────────────────── */

struct blkdev;

typedef struct {
    struct blkdev *dev;          /* underlying block device            */
    uint32_t part_lba;           /* partition start LBA (0 if no MBR) */

    /* Parsed BPB fields */
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_cluster;
    uint32_t fat_start_sector;   /* first FAT sector (relative to partition) */
    uint32_t fat_size;           /* sectors per FAT                   */
    uint32_t num_fats;
    uint32_t data_start_sector;  /* first data sector                 */
    uint32_t root_cluster;       /* root directory first cluster      */
    uint32_t total_clusters;

    /* FAT cache: one sector of the FAT table */
    uint32_t fat_cache_sector;   /* which FAT sector is cached (0xFFFFFFFF = none) */
    uint8_t  fat_cache[512];     /* cached FAT sector data            */
    int      fat_cache_dirty;    /* 1 if cache has unsaved modifications */
} vfat_sb_t;

/* ── Helper macros ───────────────────────────────────────────────────────── */

/* First sector of a cluster (relative to partition start) */
#define CLUSTER_TO_SECTOR(sb, c) \
    ((sb)->data_start_sector + ((uint32_t)((c) - 2) * (sb)->sectors_per_cluster))

/* End-of-chain marker check */
#define FAT_EOC(val)  (((val) & 0x0FFFFFF8u) == 0x0FFFFFF8u)

/* Free cluster marker */
#define FAT_FREE  0x00000000u

/* Read a 16-bit little-endian value from a byte pointer */
#define LE16(p) ((uint16_t)(p)[0] | ((uint16_t)(p)[1] << 8))

/* Read a 32-bit little-endian value from a byte pointer */
#define LE32(p) ((uint32_t)(p)[0] | ((uint32_t)(p)[1] << 8) | \
                 ((uint32_t)(p)[2] << 16) | ((uint32_t)(p)[3] << 24))

/* Extract the full 32-bit cluster number from a directory entry */
#define DIRENT_CLUSTER(d) \
    ((uint32_t)(d)->first_cluster_lo | ((uint32_t)(d)->first_cluster_hi << 16))

/* SFN checksum for LFN validation */
static inline uint8_t fat_sfn_checksum(const uint8_t name[11])
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1u) ? 0x80u : 0) + (sum >> 1) + name[i]);
    return sum;
}

#endif /* PPAP_FS_VFAT_FORMAT_H */
