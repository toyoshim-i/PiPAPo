/*
 * mkfatimg.c — Generate a minimal FAT32 image for QEMU testing
 *
 * Creates a small FAT32 filesystem image (256 KB, 1 sector/cluster)
 * with pre-populated test files.  The image is written to stdout or
 * to a named output file.
 *
 * Usage:   mkfatimg <output.bin> [<ufs_image>]
 *
 * The generated image contains:
 *   /hello.txt     "Hello from FAT32!\n"
 *   /data.bin      256 bytes: 0x00..0xFF
 *   /subdir/       empty directory
 *   /testloop.bin  2048 bytes: sector N filled with byte N (loopback test)
 *   /testufs.img   optional UFS image (from mkufs, passed as 2nd arg)
 *
 * Build:   cc -O2 -o mkfatimg mkfatimg.c
 *
 * The image parameters:
 *   Total size:       256 KB (512 sectors)
 *   Bytes/sector:     512
 *   Sectors/cluster:  1
 *   Reserved sectors: 32  (sector 0 = BPB, sectors 1-31 reserved)
 *   Number of FATs:   1   (saves space; unusual but valid)
 *   FAT size:         2 sectors (128 entries × 4 bytes = 512 entries)
 *   Data start:       sector 34
 *   Root cluster:     2 (first data cluster)
 *   Usable clusters:  478 (sectors 34..511)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SECTOR_SIZE      512
#define TOTAL_SECTORS    512
#define RESERVED_SECTORS  32
#define NUM_FATS           1
#define FAT_SECTORS        2      /* 512 entries × 4B = 2 sectors */
#define DATA_START        (RESERVED_SECTORS + NUM_FATS * FAT_SECTORS)  /* 34 */
#define ROOT_CLUSTER       2
#define TOTAL_SIZE        (TOTAL_SECTORS * SECTOR_SIZE)  /* 256 KB */

static uint8_t img[TOTAL_SIZE];

/* Write a 16-bit LE value */
static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

/* Write a 32-bit LE value */
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Write a FAT32 entry */
static void fat_set(uint32_t cluster, uint32_t value)
{
    uint32_t fat_offset = RESERVED_SECTORS * SECTOR_SIZE + cluster * 4;
    put32(&img[fat_offset], value);
}

/* Sector offset for a given cluster number */
static uint32_t cluster_offset(uint32_t cluster)
{
    return (DATA_START + (cluster - 2)) * SECTOR_SIZE;
}

/* Write a short (8.3) directory entry */
static void write_dirent(uint8_t *p, const char name[11], uint8_t attr,
                          uint32_t cluster, uint32_t size)
{
    memcpy(p, name, 11);
    p[11] = attr;           /* attribute */
    /* bytes 12-25 are zero (timestamps, etc.) */
    put16(&p[20], (uint16_t)(cluster >> 16));   /* first_cluster_hi */
    put16(&p[26], (uint16_t)(cluster & 0xFFFF));/* first_cluster_lo */
    put32(&p[28], size);                        /* file_size */
}

int main(int argc, char *argv[])
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <output.bin> [<ufs_image>]\n", argv[0]);
        return 1;
    }

    /* Read optional UFS image */
    uint8_t *ufs_data = NULL;
    uint32_t ufs_size = 0;
    if (argc == 3) {
        FILE *uf = fopen(argv[2], "rb");
        if (!uf) { perror(argv[2]); return 1; }
        fseek(uf, 0, SEEK_END);
        ufs_size = (uint32_t)ftell(uf);
        fseek(uf, 0, SEEK_SET);
        if (ufs_size == 0 || (ufs_size % SECTOR_SIZE) != 0) {
            fprintf(stderr, "mkfatimg: UFS image must be sector-aligned\n");
            return 1;
        }
        ufs_data = malloc(ufs_size);
        if (!ufs_data) { perror("malloc"); return 1; }
        if (fread(ufs_data, 1, ufs_size, uf) != ufs_size) {
            perror("fread"); return 1;
        }
        fclose(uf);
    }
    uint32_t ufs_sectors = ufs_size / SECTOR_SIZE;

    memset(img, 0, sizeof(img));

    /* ── BPB (BIOS Parameter Block) at sector 0 ────────────────────── */
    uint8_t *bpb = &img[0];
    bpb[0] = 0xEB; bpb[1] = 0x58; bpb[2] = 0x90;  /* jmp_boot */
    memcpy(&bpb[3], "PPAP    ", 8);                  /* oem_name */
    put16(&bpb[11], SECTOR_SIZE);                     /* bytes_per_sector */
    bpb[13] = 1;                                      /* sectors_per_cluster */
    put16(&bpb[14], RESERVED_SECTORS);                /* reserved_sectors */
    bpb[16] = NUM_FATS;                               /* num_fats */
    put16(&bpb[17], 0);                               /* root_entry_count (0 for FAT32) */
    put16(&bpb[19], 0);                               /* total_sectors_16 (0 for FAT32) */
    bpb[21] = 0xF8;                                   /* media_type */
    put16(&bpb[22], 0);                               /* fat_size_16 (0 for FAT32) */
    put16(&bpb[24], 0);                               /* sectors_per_track */
    put16(&bpb[26], 0);                               /* num_heads */
    put32(&bpb[28], 0);                               /* hidden_sectors */
    put32(&bpb[32], TOTAL_SECTORS);                   /* total_sectors_32 */

    /* FAT32-specific fields (offset 36) */
    put32(&bpb[36], FAT_SECTORS);                     /* fat_size_32 */
    put16(&bpb[40], 0);                               /* ext_flags */
    put16(&bpb[42], 0);                               /* fs_version */
    put32(&bpb[44], ROOT_CLUSTER);                    /* root_cluster */
    put16(&bpb[48], 0);                               /* fs_info_sector (unused) */
    put16(&bpb[50], 0);                               /* backup_boot_sector */
    bpb[64] = 0x80;                                   /* drive_number */
    bpb[66] = 0x29;                                   /* boot_sig */
    put32(&bpb[67], 0x12345678);                      /* volume_id */
    memcpy(&bpb[71], "PPAPTEST   ", 11);              /* volume_label */
    memcpy(&bpb[82], "FAT32   ", 8);                  /* fs_type */

    /* Boot sector signature */
    img[510] = 0x55;
    img[511] = 0xAA;

    /* ── FAT table ──────────────────────────────────────────────────── */
    /* Entry 0: media type marker */
    fat_set(0, 0x0FFFFFF8);
    /* Entry 1: EOC marker */
    fat_set(1, 0x0FFFFFFF);
    /* Cluster 2: root directory (EOC) */
    fat_set(2, 0x0FFFFFFF);
    /* Cluster 3: hello.txt data (EOC) */
    fat_set(3, 0x0FFFFFFF);
    /* Cluster 4: data.bin data (EOC) */
    fat_set(4, 0x0FFFFFFF);
    /* Cluster 5: subdir directory (EOC) */
    fat_set(5, 0x0FFFFFFF);
    /* Clusters 6-9: testloop.bin (4 sectors, chain: 6→7→8→9→EOC) */
    fat_set(6, 7);
    fat_set(7, 8);
    fat_set(8, 9);
    fat_set(9, 0x0FFFFFFF);
    /* Clusters 10..(10+ufs_sectors-1): testufs.img (if provided) */
    for (uint32_t c = 0; c < ufs_sectors; c++) {
        if (c + 1 < ufs_sectors)
            fat_set(10 + c, 10 + c + 1);
        else
            fat_set(10 + c, 0x0FFFFFFF);  /* EOC */
    }

    /* ── Root directory (cluster 2) ─────────────────────────────────── */
    uint8_t *root = &img[cluster_offset(2)];

    /* Volume label entry */
    write_dirent(&root[0], "PPAPTEST   ", 0x08, 0, 0);

    /* hello.txt — "Hello from FAT32!\n" = 19 bytes */
    /*             "HELLO   TXT" in 8.3 format */
    write_dirent(&root[32], "HELLO   TXT", 0x20, 3, 19);

    /* data.bin — 256 bytes (0x00..0xFF) */
    write_dirent(&root[64], "DATA    BIN", 0x20, 4, 256);

    /* subdir/ — directory */
    write_dirent(&root[96], "SUBDIR     ", 0x10, 5, 0);

    /* testloop.bin — 2048 bytes (4 sectors, for loopback testing) */
    /*                "TESTLOOPBIN" in 8.3 format */
    write_dirent(&root[128], "TESTLOOPBIN", 0x20, 6, 2048);

    /* testufs.img — optional UFS image (from mkufs) */
    if (ufs_data)
        write_dirent(&root[160], "TESTUFS IMG", 0x20, 10, ufs_size);

    /* ── hello.txt data (cluster 3) ─────────────────────────────────── */
    memcpy(&img[cluster_offset(3)], "Hello from FAT32!\n", 19);

    /* ── data.bin data (cluster 4) ──────────────────────────────────── */
    for (int i = 0; i < 256; i++)
        img[cluster_offset(4) + i] = (uint8_t)i;

    /* ── testloop.bin data (clusters 6-9) ────────────────────────────── */
    /* Each sector (cluster) is filled with its sector index byte:
     * sector 0 → all 0x00, sector 1 → all 0x01, etc. */
    for (int s = 0; s < 4; s++)
        memset(&img[cluster_offset(6 + s)], s, SECTOR_SIZE);

    /* ── subdir/ directory (cluster 5) ──────────────────────────────── */
    uint8_t *subdir = &img[cluster_offset(5)];
    write_dirent(&subdir[0],  ".          ", 0x10, 5, 0);  /* . */
    write_dirent(&subdir[32], "..         ", 0x10, 2, 0);  /* .. */

    /* ── testufs.img data (clusters 10..) ──────────────────────────── */
    if (ufs_data) {
        if (cluster_offset(10) + ufs_size > TOTAL_SIZE) {
            fprintf(stderr, "mkfatimg: UFS image (%u B) exceeds FAT image\n",
                    ufs_size);
            free(ufs_data);
            return 1;
        }
        for (uint32_t s = 0; s < ufs_sectors; s++)
            memcpy(&img[cluster_offset(10 + s)],
                   &ufs_data[s * SECTOR_SIZE], SECTOR_SIZE);
        free(ufs_data);
    }

    /* ── Write output file ──────────────────────────────────────────── */
    FILE *fp = fopen(argv[1], "wb");
    if (!fp) {
        perror(argv[1]);
        return 1;
    }
    if (fwrite(img, 1, TOTAL_SIZE, fp) != TOTAL_SIZE) {
        perror("fwrite");
        fclose(fp);
        return 1;
    }
    fclose(fp);

    printf("mkfatimg: created %s (%d KB, %d sectors)\n",
           argv[1], TOTAL_SIZE / 1024, TOTAL_SECTORS);
    return 0;
}
