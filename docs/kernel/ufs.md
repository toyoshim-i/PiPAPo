# UFS (44BSD Layout)

This document describes the UFS on-disk format and kernel behavior used by
PPAP after the 44BSD-compatible migration.

## Scope

- On-disk layout and invariants used by `src/kernel/vfs/ufs.c`
- Image-generation expectations for `tools/mkufs/mkufs.c`
- Bootloader assumptions used by ia16 stage2 (`pcxt`) and m68k stage2 (`x68k`)

## Endianness

UFS images on shared FAT32 media are little-endian (see
[targets/m68k.md §4](../targets/m68k.md#4-endianness-strategy)), but a
dedicated boot floppy can be built big-endian so a big-endian CPU reads its
fields natively. `mkufs` selects this with `-B`; the `x68k` image uses it so
stage2 and the kernel read the on-disk UFS without byte-swapping.

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

### Big-endian symlink fast-symlink encoding

In big-endian mode (`-B`), `mkufs` pre-swaps the `i_direct[]` bytes when it
stores a fast symlink target inline, so that `write_inode()`'s `w32()`
byte-swap cancels out and the raw target bytes land on disk uncorrupted.

## ia16 Boot Interop

The ia16 stage2 loader reads UFS metadata directly from raw sectors and expects:

- Superblock at sector 16
- UFS magic at superblock byte offset 1372
- 128-byte inode records (4 per sector)
- Variable-length BSD directory entries

The PC/XT image builder defaults to 44BSD UFS so stage2 and kernel parse the
same on-disk format.

## x68k Boot Interop

The X68000 boot floppy holds Stage1 (sector 0) and Stage2 (sectors 1-3)
ahead of a **single big-endian 44BSD UFS starting at sector 4** (byte
offset 4096). There is no inner/outer image split: the one UFS is both what
Stage2 reads `/boot/kernel` from and what the kernel mounts live as `/` via
`iocs_blk`. The image is built by `scripts/mkx68kimg.sh`, which stages the
romfs tree plus the kernel binary (as `/boot/kernel`) and runs `mkufs -B`.

m68k stage2 (`src/target/x68k/boot/stage2.c`) reads UFS metadata directly
from raw floppy sectors via IOCS `_B_READ` and expects:

- Superblock at the UFS's fragment 16 (byte 8192 from partition start)
- UFS magic 0x00011954 at superblock byte offset 1372
- Inode-table start fragment in `fs_iblkno` (superblock byte offset 16)
- Root directory at inode 2
- 128-byte inode records (4 per 512-byte sector)
- Variable-length BSD directory entries
- Files up to 12 direct blocks plus one single-indirect block (the kernel
  is ~120 KB; Stage2 does not parse double/triple indirect blocks)

Because all fields are big-endian, Stage2 reads them as native m68k values
with no byte-swapping.