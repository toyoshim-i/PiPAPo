# PC/XT HDD Boot Support (R-2)

Add hard-disk boot support to the PC/XT target while keeping floppy boot
working.  The bootstrap code is shared between both media; only the
sector-0 header differs.

---

## 1. Disk Layout

Both floppy and HDD use the same logical layout: stage1 at sector 0,
stage2 at sectors 1–8, UFS partition at sector 9+.

```
Floppy (1.44 MB, 2880 sectors):
┌──────────────────┐ sector 0
│ stage1 + BPB     │  (512 B, floppy BIOS Parameter Block)
├──────────────────┤ sector 1
│ stage2           │  (8 sectors = 4 KB, shared binary)
├──────────────────┤ sector 9
│ UFS partition    │  (2871 sectors ≈ 1.4 MB)
└──────────────────┘ sector 2879

HDD (16 MB, 32768 sectors):
┌──────────────────┐ sector 0
│ MBR + part table │  (512 B, partition table at offset 446)
├──────────────────┤ sector 1
│ stage2           │  (8 sectors = 4 KB, same binary as floppy)
├──────────────────┤ sector 9
│ UFS partition    │  (32759 sectors ≈ 16 MB)
└──────────────────┘ sector 32767
```

The UFS start at sector 9 is identical in both cases.  The MBR
partition table entry records the start LBA (9) and size explicitly,
so standard tools can identify the partition.

### Partition Type

MBR partition type **0xA9** (BSD UFS).  The UFS format migration in
Step 7 makes PPAP images compatible with Linux's UFS driver, so the
standard BSD partition type is appropriate (see §7).

---

## 2. Unified Stage1 with Shared Include

### 2.1 stage1_common.inc

Factor the shared boot logic out of `stage1.S` into a common include:

- `print_char` — write AL to both BIOS teletype (INT 10h) and COM1
- `.Lstart` — CLI, set up segments (DS=ES=SS=0), stack at 0x7C00
- Save boot drive (DL) to `drive_no`
- Print "Pi" banner
- Load 8 sectors (stage2) from sectors 1–8 to 0x0000:0xC000 via
  INT 13h AH=02h, using `drive_no`
- Pass DL to stage2, far-jump to 0x0000:0xC000
- `.Ldisk_err` — print 'E', halt

Both stage1 variants include this file after their headers.

### 2.2 stage1.S (floppy, unchanged logic)

```
Offset 0–2:    jmp .Lstart; nop
Offset 3–61:   BPB (1.44 MB floppy parameters)
Offset 62–509: boot code (.include "stage1_common.inc")
Offset 510:    0xAA55
```

### 2.3 stage1_hdd.S (new, MBR variant)

```
Offset 0–2:    jmp .Lstart; nop
Offset 3–445:  boot code (.include "stage1_common.inc")
Offset 446–461: partition 1 (type=0xBB, start_lba=9, size=32759)
Offset 462–509: partitions 2–4 (empty)
Offset 510:    0xAA55
```

The partition table is assembled statically by the image builder
(`mkpcimg.sh --hdd`), not embedded in the assembly source.  stage1_hdd.S
pads to 446 bytes and leaves the remaining 66 bytes for the script to
fill in.

---

## 3. Stage2 Dynamic Geometry

### 3.1 Current State

`stage2.c` hardcodes floppy geometry:

```c
#define SECS_PER_TRACK  18u
#define NUM_HEADS       2u
#define UFS_FLOPPY_BASE 9u
```

### 3.2 Changes

Replace with globals, initialised based on boot drive:

```c
static uint16_t secs_per_track;
static uint16_t num_heads;
static uint16_t ufs_base_sector;

void stage2_main(void) {
    if (boot_drive >= 0x80) {
        /* HDD: query geometry via INT 13h AH=08h */
        query_disk_geometry(&secs_per_track, &num_heads);
        /* Read MBR partition table, find type 0xBB */
        ufs_base_sector = find_ufs_partition();
    } else {
        /* Floppy: known geometry */
        secs_per_track = 18;
        num_heads = 2;
        ufs_base_sector = 9;
    }
    /* ... rest unchanged, using variables instead of macros ... */
}
```

**INT 13h AH=08h** (Get Drive Parameters):

```nasm
mov  ah, 0x08
mov  dl, boot_drive
int  0x13
; DH = max head number (0-based) → num_heads = DH + 1
; CL bits 0-5 = max sector number (1-based) → secs_per_track = CL & 0x3F
```

**MBR partition scan**: Read sector 0 into BUF, check 0xAA55 signature
at offset 510, scan 4 entries at offset 446 for type 0xBB.  The entry's
`start_lba` (uint32_t at offset +8) becomes `ufs_base_sector`.

### 3.3 Size Impact

The geometry query + MBR parser adds ~100–150 bytes to stage2.
Current stage2 is well under the 4 KB limit.

---

## 4. Boot Parameters in mod_info

Extend the existing `mod_info_t` at 0x0500 so the kernel receives
boot device information from stage2:

```c
#define MOD_MAX 4u

typedef struct {
    uint16_t count;
    struct {
        uint16_t segment;
        uint16_t size;
    } mod[MOD_MAX];
    /* Boot device info (new fields, after mod[]) */
    uint8_t  boot_drive;        /* 0x00 = floppy, 0x80 = HDD */
    uint8_t  reserved;
    uint16_t ufs_base_sector;   /* LBA of first UFS sector */
    uint16_t secs_per_track;    /* CHS geometry */
    uint16_t num_heads;
    uint32_t partition_sectors; /* UFS partition size in sectors */
} mod_info_t;
```

Stage2 fills in all fields.  `target_early_init()` reads them and
passes the boot device parameters to the block driver init.

The new fields start at offset 18 within the struct (after `count` +
4 × `mod` entries).  The existing kernel reads `count`, `mod[0..2]` —
the new fields occupy previously unused bytes in the 0x0500–0x05FF
range.

---

## 5. Unified Block Driver (bios_blk)

### 5.1 Rename

`floppy_blk.c` / `floppy_blk.h` → `bios_blk.c` / `bios_blk.h`

### 5.2 Interface

```c
void bios_blk_init(uint8_t drive, uint16_t ufs_base,
                   uint16_t spt, uint16_t heads,
                   uint32_t partition_sectors);
```

- **Drive number**: passed through to INT 13h DL
- **UFS base sector**: added to every LBA before the INT 13h call
- **Geometry**: used for LBA→CHS conversion
- **Device name**: `"fd0"` if `drive < 0x80`, `"hd0"` if `drive >= 0x80`
- **Sector count**: `partition_sectors` (floppy: 2871, HDD: from MBR)

The `read_sector_bios` function is already device-agnostic.  Only the
LBA→CHS conversion and the device registration change.

### 5.3 Kernel Init Flow

```
target_early_init():
    read mod_info at 0x0500
    → boot_drive, ufs_base_sector, spt, heads, partition_sectors

target_late_init():
    bios_blk_init(boot_drive, ufs_base_sector, spt, heads, partition_sectors)
    mount_ufs("/", device_name)
```

---

## 6. Build Flow and run.sh

### 6.1 CMakeLists.txt

Add `stage1_hdd` target alongside existing `stage1`:

```cmake
add_executable(stage1_hdd boot/stage1_hdd.S)
target_link_options(stage1_hdd PRIVATE -T ${STAGE1_LD} ...)
add_custom_command(... stage1_hdd.bin ...)
```

Both stage1 variants share the same stage2, core, VFS, and user
binaries.

### 6.2 mkpcimg.sh

Add `--hdd` mode.  The script always builds both images:

```
Output:
  build/pcxt/ppap_pcxt.img       (1.44 MB floppy)
  build/pcxt/ppap_pcxt_hdd.img   (16 MB HDD)
```

Floppy image assembly is unchanged.  HDD image:

1. Create 16 MB zero-filled image
2. Write stage1_hdd.bin to sector 0 (first 446 bytes)
3. Write MBR partition table at offset 446:
   - Entry 1: status=0x80 (active), type=0xBB, start_lba=9,
     size=(total_sectors − 9)
   - Entries 2–4: zero
4. Write 0xAA55 at offset 510
5. Write stage2 to sectors 1–8
6. Write UFS to sectors 9+

The MBR CHS fields in the partition entry use the LBA-to-CHS
conversion for the 16 MB geometry (heads=16, spt=63 is standard for
small disks, giving 32 cylinders for 16 MB).

### 6.3 run.sh

Add `--hdd` flag:

```bash
# Argument parsing
--hdd)  DO_HDD=1 ;;

# QEMU launch (pcxt target)
if [[ $DO_HDD -eq 1 ]]; then
    IMG="$BUILD_DIR/ppap_pcxt_hdd.img"
    QEMU_ARGS=(-machine pc -cpu 486 -m 1M -serial mon:stdio
               -drive "file=$IMG,format=raw,if=ide")
else
    IMG="$BUILD_DIR/ppap_pcxt.img"
    QEMU_ARGS=(-machine pc -cpu 486 -m 1M -serial mon:stdio
               -drive "file=$IMG,format=raw,if=floppy")
fi
```

Usage:

```sh
./scripts/run.sh --build pcxt           # build both images, boot from floppy
./scripts/run.sh --build --hdd pcxt     # build both images, boot from HDD
./scripts/run.sh --hdd pcxt             # boot existing HDD image
```

---

## 7. UFS Compatibility: Making PPAP Images Mountable on Linux

The goal is to make PPAP UFS images mountable on Linux via
`mount -t ufs -o ufstype=44bsd`, so that developers can edit image
contents directly from the host without a custom tool.  If achievable,
the MBR partition type can use 0xA9 (BSD UFS) instead of a custom
byte.

This section analyses what the Linux UFS driver requires and how far
PPAP UFS is from meeting those requirements.

### 7.1 What Linux's UFS Driver Expects

Linux mounts UFS1 (4.4BSD FFS) with `mount -t ufs -o ufstype=44bsd`.
The driver checks:

| Requirement | Expected | PPAP Current | Gap |
|-------------|----------|-------------|-----|
| **Superblock location** | Partition offset 8192 (0x2000) | Partition offset 0 | Move SB, leave 8 KB boot area |
| **Magic number** | 0x00011954 at SB+1372 | 0x55465331 at SB+0 | Different value, different offset within SB |
| **Superblock size** | 1536 bytes (3 × 512B sub-structs) | 128 bytes | Expand to full BSD layout |
| **Root inode** | Inode 2 | Inode 1 | Change root inode number |
| **Inode size** | 128 bytes | 64 bytes | Grow inode, add fields |
| **Inode fields** | + atime, blocks, flags, gen, 12 direct + 3 indirect | 10 direct + 1 indirect | Add missing fields |
| **Directory entries** | Variable-length (ino, reclen, type, namlen, name) | Fixed 32 bytes (ino, name) | Rewrite dir format |
| **Cylinder groups** | CG metadata required for inode-to-block mapping | None (flat layout) | Add CG structure |
| **Fragments** | fsize (512–4096), frag = bsize/fsize | None (block = allocation unit) | Set frag=1 (fsize=bsize) |
| **Block size** | ≥ 4096, power of 2 | 4096 | Compatible as-is |
| **fs_postblformat** | Must be 1 (UFS_DYNAMICPOSTBLFMT) | N/A | Add field |
| **fs_clean** | 0x01 (clean) or 0x02 (stable) | N/A | Add field |

### 7.2 The Cylinder Group Problem

Even for **read-only** mounts, Linux's UFS driver uses cylinder group
geometry for inode-to-block mapping:

```
cg       = ino / fs_ipg              // which cylinder group
cg_start = cg * fs_fpg               // CG start fragment
inode_frag = cg_start + fs_iblkno + (ino % fs_ipg) / inodes_per_frag
```

This means the superblock must describe CG layout (fs_ncg, fs_ipg,
fs_fpg, fs_iblkno, etc.) and the inode table must be physically
located where the formula expects it.

**The workaround**: use **1 cylinder group** for the entire filesystem.
With `fs_ncg=1`, all inodes and data blocks belong to CG 0.  The
on-disk layout becomes effectively flat — identical to what PPAP
already does — but wrapped in the CG metadata that Linux expects:

```
Partition layout with 1 CG:
  Offset 0:        Boot block area (8 KB, zeros)
  Offset 8192:     Superblock (1536 bytes, BSD format)
  Offset ~16384:   CG 0 descriptor (magic 0x090255, bitmaps)
  Offset ~N:       Inode table (128-byte inodes)
  Offset ~M:       Data blocks
```

This is not a hack — small BSD filesystems genuinely have 1 CG.
The Linux driver handles this correctly.  For read-write mount,
Linux additionally validates the CG descriptor (CG_MAGIC, bitmaps,
summary), so mkufs must emit fully correct CG metadata — but with
1 CG and no fragments this is straightforward.

### 7.3 Variable-Length Directory Entries

BSD directory entries use:

```c
struct direct {
    uint32_t d_ino;       /* inode number */
    uint16_t d_reclen;    /* record length (4-byte aligned) */
    uint8_t  d_type;      /* file type (DT_REG, DT_DIR, ...) */
    uint8_t  d_namlen;    /* name length */
    char     d_name[];    /* name (NUL-terminated) */
};
```

PPAP currently uses fixed 32-byte entries `(uint32_t d_ino, char
d_name[28])`.  Switching to variable-length entries requires:

- **Kernel directory scan**: iterate by `d_reclen` instead of fixed
  stride.  Moderate change — the scan loop in `ufs_lookup` and
  `ufs_readdir` changes from `for (i = 0; i < N; i++)` to
  `while (offset < blocksize)`.

- **Directory create/unlink**: find free space by walking `d_reclen`
  gaps.  On unlink, merge with adjacent free entry.  This is more
  complex than fixed-slot free (currently `d_ino = 0`), but standard
  BSD code is well-documented.

- **mkufs**: generate variable-length entries.  Straightforward —
  the host tool just needs to emit the right format.

This is the largest single change, but it is a contained rewrite of
the directory layer (3 functions: lookup, readdir, create/unlink).

### 7.4 Fragment Simplification

BSD FFS supports sub-block fragments (e.g., 8 KB blocks with 1 KB
fragments) for space efficiency.  However, Linux's UFS driver accepts
`fs_fsize = fs_bsize` (fragment size = block size, i.e., `fs_frag=1`).
This means **no fragment support is needed** — just set the fragment
fields correctly in the superblock and allocate whole blocks.

### 7.5 Summary of Required Changes

Grouped by where the work falls:

**mkufs (host tool) — most of the work:**

| Change | Effort |
|--------|--------|
| Write 8 KB boot area + BSD superblock at offset 8192 | Small |
| Fill 1536-byte superblock with all required fields | Medium (many fields, but mostly constants for 1-CG case) |
| Write CG 0 descriptor with block/inode bitmaps | Medium |
| Emit 128-byte inodes (add atime, blocks, flags, gen, 3 indirect ptrs) | Small |
| Emit variable-length directory entries | Small |
| Root inode = 2 instead of 1 | Trivial |

**PPAP kernel ufs.c — moderate changes:**

| Change | Effort |
|--------|--------|
| Mount: read SB at offset 8192, parse BSD superblock fields | Small |
| Mount: compute inode locations using CG formula (1 CG = simple) | Small |
| Inode: read 128-byte inodes, handle new fields | Small |
| Directory: variable-length entry scan in lookup/readdir | Medium |
| Directory: variable-length create/unlink | Medium |
| Root inode = 2 | Trivial |

**stage2 boot loader:**

| Change | Effort |
|--------|--------|
| Read SB at partition offset 8192 instead of 0 | Trivial |
| Parse BSD superblock to find inode table | Small |
| Read 128-byte inodes | Small |
| Scan variable-length directory entries | Small |

### 7.6 Assessment

Making PPAP UFS images mountable on Linux is **feasible** and does not
require a full FFS implementation.  The key insights:

- **1 cylinder group** eliminates most CG complexity while satisfying
  the Linux driver's inode mapping formula.
- **fsize = bsize** (no fragments) is valid and avoids the hardest
  part of FFS allocation.
- **Variable-length directories** are the largest code change but
  are a contained rewrite of ~3 functions.
- **128-byte inodes** just add fields; the existing 10 direct + 1
  indirect layout is a subset of BSD's 12 direct + 3 indirect.

The total effort is roughly:

- mkufs rewrite: ~400–600 lines (new superblock/CG/inode/dir format)
- ufs.c changes: ~200–300 lines (SB parse, inode read, dir scan)
- stage2 changes: ~50 lines (SB offset, inode size, dir format)

This is significant but tractable work.  If done, the MBR partition
type can be **0xA9** (BSD UFS) and Linux users can:

```sh
# Mount PPAP HDD image partition on Linux
sudo losetup -o $((9*512)) /dev/loop0 ppap_pcxt_hdd.img
sudo mount -t ufs -o ufstype=44bsd /dev/loop0 /mnt
ls /mnt/boot/kernel
```

### 7.7 Migration Plan

The UFS format migration is Step 7 of the R-2 implementation.  It
proceeds in sub-steps that can each be verified independently:

1. **mkufs rewrite** — emit BSD-compatible images (1 CG, 128-byte
   inodes, variable-length directories, superblock at offset 8192).
   Verify on Linux: `mount -t ufs -o ufstype=44bsd`.
2. **PPAP ufs.c update** — read the new format (SB at 8192, CG
   inode mapping, 128-byte inodes, variable-length dir scan).
   Verify: all targets boot and pass tests with new images.
3. **stage2 loader update** — parse new SB location, inode size,
   directory format.  Verify: pcxt + x68k boot from new images.
4. **MBR partition type → 0xA9**.

The format change is architecture-independent and applies to all
targets (ARM, m68k, RISC-V, Xtensa, i16).

---

## 8. Implementation Steps

| Step | Scope | Files | Risk |
|------|-------|-------|------|
| 1 | Factor stage1 shared code | `stage1_common.inc`, `stage1.S`, `stage1_hdd.S` | Low |
| 2 | Stage2 dynamic geometry + MBR parser | `stage2.c` | Low |
| 3 | Extend mod_info with boot params | `stage2.c`, `target_pcxt.c` | Low |
| 4 | Rename floppy_blk → bios_blk | `bios_blk.c`, `bios_blk.h`, CMakeLists | Medium |
| 5 | Build both images | `CMakeLists.txt`, `mkpcimg.sh` | Low |
| 6 | run.sh --hdd | `run.sh` | Low |
| 7a | mkufs: BSD UFS1-compatible format | `tools/mkufs/mkufs.c` | Medium |
| 7b | ufs.c: read new format | `src/kernel/vfs/ufs.c`, `ufs_format.h` | Medium |
| 7c | stage2: parse new format | `stage2.c` (pcxt), `stage2.S` (x68k) | Low |
| 7d | MBR partition type → 0xA9 | `mkpcimg.sh` | Trivial |

Steps 1–2 can be done together.  Step 4 is the riskiest for the HDD
bring-up — test floppy boot immediately after to verify no regression.
Steps 5–6 are additive and complete HDD boot.

Step 7 (UFS format migration) can proceed independently after Step 6.
Step 7a can be verified on Linux without any kernel changes.  Steps
7b–7c make PPAP boot from the new images.  Step 7d is the final
switch.

### Verification

```sh
# Floppy (regression test — after each step)
./scripts/run.sh --build pcxt

# HDD (after step 6)
./scripts/run.sh --build --hdd pcxt

# Both should boot to shell with identical kernel + userland

# Linux mount (after step 7a)
sudo losetup -o $((9*512)) /dev/loop0 build/pcxt/ppap_pcxt_hdd.img
sudo mount -t ufs -o ufstype=44bsd /dev/loop0 /mnt
ls /mnt/boot/kernel    # should list kernel binary

# All-target regression (after step 7b)
./scripts/run.sh --test qemu_arm
./scripts/run.sh --test qemu_m68k
./scripts/run.sh --test qemu_rv32
./scripts/run.sh --build pcxt
./scripts/run.sh --build --hdd pcxt
```

---

## 9. Dependency Graph

```
Current: floppy boot works (P-1 through R-1.2)

Step 1–2: unified stage1/stage2 (floppy still works, HDD path added)
    │
Step 3–4: kernel reads boot params, bios_blk replaces floppy_blk
    │        (floppy verified, HDD tested)
    │
Step 5–6: build flow + run.sh --hdd
    │        (both images built and launchable)
    │
Step 7a: mkufs emits BSD UFS1 (verify on Linux)
    │
Step 7b–7c: PPAP kernel + stage2 read new format (all targets)
    │
Step 7d: partition type → 0xA9
    │
    ├──→ R-3 (V30 8080 eCPU — independent)
    ├──→ R-4 (DOS subsystem — independent)
    └──→ Future: native ATA/IDE PIO driver (replaces BIOS INT 13h)
```
