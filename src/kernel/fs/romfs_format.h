/*
 * romfs_format.h — On-flash romfs structures
 *
 * Shared between the kernel romfs driver (romfs.c) and the host-side
 * mkromfs tool (tools/mkromfs/).  All fields are little-endian (ARM
 * native byte order).  All structures are 4-byte aligned for direct
 * Thumb instruction fetch from XIP flash.
 *
 * Image layout:
 *   [romfs_super_t]            — 16-byte superblock at offset 0
 *   [romfs_entry_t + name + data] ...  — variable-length entries
 *
 * Entries form a tree: directories have a child_off pointing to
 * their first child, and siblings are chained via next_off.
 * Offsets are relative to the image start (position-independent).
 */

#ifndef PPAP_FS_ROMFS_FORMAT_H
#define PPAP_FS_ROMFS_FORMAT_H

#include <stdint.h>

/* ── Superblock ────────────────────────────────────────────────────────────── */

#define ROMFS_MAGIC  0x50504653u   /* "PPFS" in little-endian */

typedef struct {
    uint32_t magic;       /* ROMFS_MAGIC                                  */
    uint32_t size;        /* total image size in bytes                    */
    uint32_t file_count;  /* total number of entries (files+dirs+symlinks)*/
    uint32_t root_off;    /* offset from image start to root dir entry   */
} romfs_super_t;

/* ── Entry types ───────────────────────────────────────────────────────────── */

#define ROMFS_TYPE_FILE    0u
#define ROMFS_TYPE_DIR     1u
#define ROMFS_TYPE_SYMLINK 2u

/* ── Directory / file / symlink entry ──────────────────────────────────────── */
/*
 * Variable-length on flash:
 *   [romfs_entry_t header]  — 20 bytes (5 × uint32_t)
 *   [name]                  — name_len bytes + NUL, padded to 4-byte boundary
 *   [data]                  — size bytes (file content or symlink target),
 *                             padded to 4-byte boundary
 *
 * For directories, size = 0 and data is absent.
 *
 * Helper macros:
 *   ROMFS_ALIGN4(x)       — round x up to next 4-byte boundary
 *   ROMFS_NAME_OFF        — byte offset of name[] from entry start
 *   ROMFS_DATA_OFF(e)     — byte offset of data[] from entry start
 *   ROMFS_ENTRY_SIZE(e)   — total on-flash size of this entry (header+name+data)
 */
typedef struct {
    uint32_t next_off;    /* offset to next sibling (0 = last entry)      */
    uint32_t type;        /* ROMFS_TYPE_FILE / DIR / SYMLINK              */
    uint32_t size;        /* file: content length; symlink: target length;
                             dir: 0                                       */
    uint32_t child_off;   /* DIR: offset to first child; FILE/SYMLINK: 0  */
    uint32_t name_len;    /* length of name (excluding NUL, before pad)   */
} romfs_entry_t;

/* ── Alignment and layout helpers ──────────────────────────────────────────── */

#define ROMFS_ALIGN4(x)      (((x) + 3u) & ~3u)

/* Byte offset of the name field (immediately after the fixed header) */
#define ROMFS_NAME_OFF       ((uint32_t)sizeof(romfs_entry_t))

/* Byte offset of the data field (after header + padded name) */
#define ROMFS_DATA_OFF(e) \
    (ROMFS_NAME_OFF + ROMFS_ALIGN4((e)->name_len + 1u))

/* Total on-flash size of an entry (header + padded name + padded data) */
#define ROMFS_ENTRY_SIZE(e) \
    (ROMFS_DATA_OFF(e) + ROMFS_ALIGN4((e)->size))

#endif /* PPAP_FS_ROMFS_FORMAT_H */
