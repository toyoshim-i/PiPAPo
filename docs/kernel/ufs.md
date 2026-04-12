# UFS (44BSD Layout)

This document describes the UFS on-disk format and kernel behavior used by
PPAP after the 44BSD-compatible migration.

## Scope

- On-disk layout and invariants used by `src/kernel/vfs/ufs.c`
- Image-generation expectations for `tools/mkufs/mkufs.c`
- Bootloader assumptions used by ia16 stage2 (`pcxt`)

## On-Disk Layout

PPAP uses a 44BSD-compatible UFS1 layout with a single cylinder group.

- Sector size: 512 bytes
- Block size: 4096 bytes
- Fragment size: 512 bytes (8 fragments per block)
- Superblock sector: 16 (byte offset 8192 from partition start)
- Cylinder-group sector: 32 (byte offset 16384 from partition start)
- Root inode: 2
- Inode size: 128 bytes
- Direct block pointers: 12
- Indirect block pointers: 3 (`i_ib[0..2]`)

Relevant format constants and byte offsets are defined in:

- `src/kernel/vfs/ufs_format.h`

## Directory Format

Directories use variable-length BSD entries:

- `d_ino` (inode)
- `d_reclen` (record length)
- `d_type` (file type)
- `d_namlen` (name length)
- `d_name` payload (NUL-terminated, 4-byte aligned)

Kernel directory traversal (`lookup`, `readdir`, add/remove) iterates entries
using `d_reclen` and does not assume fixed-size slots.

## Allocation Model

Allocation/free state is tracked in cylinder-group bitmaps:

- Free block bitmap (`cg_freeoff`)
- Inode used bitmap (`cg_iusedoff`)

The kernel caches free block/inode counts and synchronizes them to:

- Superblock summary (`fs_cstotal`)
- Cylinder-group summary (`cg_cs`)

## Host Tool Contract (`mkufs`)

`mkufs` must emit consistent values for:

- `fs_iblkno`, `fs_dblkno`, `fs_dsize`
- `fs_ipg`
- CG bitmap offsets and summary counts

For 44BSD mode, inode provisioning now honors `-i` override. If `-i` is not
provided, mkufs applies a default sizing policy and computes inode-table blocks
and data start from that result.

## ia16 Boot Interop

The ia16 stage2 loader reads UFS metadata directly from raw sectors and expects:

- Superblock at sector 16
- UFS magic at superblock byte offset 1372
- 128-byte inode records (4 per sector)
- Variable-length BSD directory entries

The PC/XT image builder defaults to 44BSD UFS so stage2 and kernel parse the
same on-disk format.