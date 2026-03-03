# Phase 5: UFS + Loopback — Detailed Plan

**PicoPiAndPortable Development Roadmap**
Estimated Duration: 4 weeks
Target Board: ClockworkPi PicoCalc (RP2040 + full-size SD slot)

---

## Goals

Build the loopback block device, a simplified UFS (Unix File System)
driver, a tmpfs implementation, and the boot-time fstab mount sequence.
After this phase the kernel can open UFS image files on the VFAT
partition, create loopback block devices from them, mount UFS
filesystems at `/usr`, `/home`, and `/var`, and provide a RAM-backed
`/tmp`.  This completes the full filesystem hierarchy described in the
design spec and prepares the system for Phase 6 (musl + busybox).

**Exit Criteria (all must pass before moving to Phase 6):**
- Loopback block device translates sector read/write into file I/O on the underlying VFAT file
- `loopback_setup("/mnt/sd/ppap_usr.img")` creates `/dev/loop0` and registers a block device
- Up to 3 concurrent loopback devices (`/dev/loop0`, `/dev/loop1`, `/dev/loop2`)
- UFS on-disk format: superblock, inode table, block bitmap, data blocks (4KB block size)
- `mkufs` host tool creates empty formatted UFS image files of configurable size
- UFS driver mounts a loopback block device and serves lookup/read/readdir/stat
- UFS write support: create, write, truncate, unlink, mkdir, rmdir, link, symlink, rename
- UFS permissions (owner/group/other mode bits) and timestamps (mtime, ctime) stored in inodes
- tmpfs mounted at `/tmp` provides RAM-backed file create/read/write/unlink
- `/etc/fstab` parsed at boot; all mounts executed in order (VFAT → loopback → UFS → tmpfs)
- QEMU smoke test: embedded FAT32 image containing a UFS sub-image, full I/O round-trip
- Hardware test: real SD card (FAT32) with `ppap_usr.img`, mounted at `/usr` via loopback, file operations verified via UART

---

## Source Tree After Phase 5

```
src/
  board/
    picocalc.h            (existing)
  boot/
    startup.S             (existing)
    stage1.S              (existing)
  kernel/
    main.c                (existing — add fstab parsing, loopback + UFS mounts)
    main_qemu.c           (existing — add loopback + UFS mount test)
    blkdev/
      blkdev.c / blkdev.h (existing)
      ramblk.c / ramblk.h (existing)
      loopback.c / loopback.h  # Loopback block device: file → block device translation
    mm/
      page.c/h            (existing)
      kmem.c/h            (existing)
      mpu.c/h             (existing)
      xip.c/h             (existing)
    proc/
      proc.c/h            (existing)
      sched.c/h           (existing)
      switch.S            (existing)
    vfs/
      vfs.c/h             (existing)
      namei.c             (existing)
    fs/
      romfs.c/h           (existing)
      romfs_format.h      (existing)
      devfs.c/h           (existing — add /dev/loop0..2)
      procfs.c/h          (existing)
      vfat.c/h            (existing)
      vfat_format.h       (existing)
      ufs.c / ufs.h       # UFS driver: mount, lookup, read, write, readdir, stat,
                           #   create, mkdir, unlink, rmdir, link, symlink, rename
      ufs_format.h        # On-disk UFS structures (superblock, inode, dirent)
      tmpfs.c / tmpfs.h   # tmpfs: RAM-backed temp filesystem at /tmp
      fstab.c / fstab.h   # /etc/fstab parser + sequential mount executor
    fd/
      file.h              (existing)
      fd.c/h              (existing)
      tty.c/h             (existing)
      pipe.c              (existing)
    exec/
      elf.c/h             (existing)
      exec.c/h            (existing)
    signal/
      signal.c/h          (existing)
    syscall/
      syscall.c/h         (existing — add mount, umount, sync syscalls)
      svc.S               (existing)
      sys_proc.c          (existing)
      sys_io.c            (existing)
      sys_fs.c            (existing — add sys_mount, sys_umount, sys_sync)
      sys_mem.c           (existing)
      sys_time.c          (existing)
    smp.c/h               (existing)
    fmt.c/h               (existing)
  drivers/
    uart.c/h              (existing)
    uart_qemu.c           (existing)
    clock.c/h             (existing)
    spi.c/h               (existing)
    sd.c/h                (existing)
user/
  (existing)
  test_ufs.c             # On-target: UFS mount, file create/read/write/delete
  test_loopback.c        # On-target: loopback setup, block I/O round-trip
tests/
  (existing)
  test_ufs_unit.c        # Host-native: UFS format, inode read, block allocation
  test_loopback_unit.c   # Host-native: loopback sector ↔ file offset translation
tools/
  mkromfs/               (existing)
  mkfatimg/              (existing)
  mkufs/
    mkufs.c              # Host tool: create formatted UFS image files
    Makefile             # Build with host gcc
romfs/
  (existing)
  etc/
    fstab                # Mount table: /mnt/sd, /usr, /home, /var, /tmp, /dev, /proc
```

---

## Week 1: Loopback Block Device + tmpfs

### Step 1 — Loopback Block Device (`src/kernel/blkdev/loopback.c`)

The loopback block device is the key mechanism that bridges file-level
I/O (VFAT image files on SD) and block-level I/O (UFS filesystem
driver).  It implements the `blkdev_t` interface by translating sector
read/write operations into `lseek` + `read`/`write` on an underlying
open file.

**Design:**

```c
/* src/kernel/blkdev/loopback.h */

#define LOOP_MAX  3   /* /dev/loop0, loop1, loop2 */

typedef struct loop_dev {
    blkdev_t    blk;          /* base: registered block device */
    int         backing_fd;   /* fd of the underlying image file */
    uint32_t    file_size;    /* image file size in bytes */
    uint8_t     active;       /* 1 = in use, 0 = free */
} loop_dev_t;

/* Set up a loopback device from an image file path.
 * Opens the file, determines its size, computes sector_count,
 * and registers a block device named "loopN".
 *
 * Returns the loop device index (0–2) on success, negative errno
 * on failure (-ENOMEM if all slots are used, -ENOENT if file not
 * found, -EIO on read error). */
int loopback_setup(const char *image_path);

/* Tear down a loopback device: close the backing fd, unregister
 * the block device, mark the slot as free.
 * Returns 0 on success, -EINVAL if not active. */
int loopback_teardown(int loop_index);

/* Initialise the loopback subsystem.  Call once from kmain(). */
void loopback_init(void);
```

**I/O translation:**

```c
static int loop_read(blkdev_t *dev, void *buf,
                     uint32_t sector, uint32_t count) {
    loop_dev_t *loop = (loop_dev_t *)dev;
    uint32_t offset = sector * BLKDEV_SECTOR_SIZE;
    uint32_t nbytes = count * BLKDEV_SECTOR_SIZE;

    if (offset + nbytes > loop->file_size)
        return -EIO;

    /* Seek + read on the backing file via VFS */
    struct file *f = fd_get_file(loop->backing_fd);
    f->offset = offset;
    long n = f->vnode->mount->ops->read(f->vnode, buf, nbytes, offset);
    return (n == (long)nbytes) ? 0 : -EIO;
}

static int loop_write(blkdev_t *dev, const void *buf,
                      uint32_t sector, uint32_t count) {
    loop_dev_t *loop = (loop_dev_t *)dev;
    uint32_t offset = sector * BLKDEV_SECTOR_SIZE;
    uint32_t nbytes = count * BLKDEV_SECTOR_SIZE;

    if (offset + nbytes > loop->file_size)
        return -EIO;

    struct file *f = fd_get_file(loop->backing_fd);
    long n = f->vnode->mount->ops->write(f->vnode, buf, nbytes, offset);
    return (n == (long)nbytes) ? 0 : -EIO;
}
```

**Key design decisions:**

- The loopback device operates at the VFS/vnode level, not at the
  syscall level — it calls `ops->read` / `ops->write` directly on the
  backing vnode.  This avoids re-entering the syscall path and keeps
  the I/O stack shallow: UFS → loopback → VFAT `ops->read` → SD
  blkdev → SPI hardware.
- The backing fd is held open for the lifetime of the loopback device.
  This prevents the file from being deleted while a filesystem is
  mounted on it.
- `file_size` determines `sector_count` (= `file_size / 512`).  The
  image file size must be a multiple of 512 bytes; `mkufs` ensures this.
- The 3-slot limit (LOOP_MAX=3) matches the 3 UFS images
  (`ppap_usr.img`, `ppap_home.img`, `ppap_var.img`).

**devfs integration:**

`/dev/loop0`, `/dev/loop1`, `/dev/loop2` are added to devfs.
Unlike `/dev/mmcblk0`, loop device files only appear once their
corresponding loopback device is active.  Read/write on a loop device
file delegates to the loopback `blkdev_t`.

**QEMU note:** On QEMU, the loopback device translates block I/O into
file reads on the RAM-backed VFAT image.  The I/O path is:
UFS → loopback → VFAT `ops->read` → ramblk `blkdev_t` → ROM.
This exercises the same code path as hardware, minus the SPI/SD layer.


### Step 2 — tmpfs (`src/kernel/fs/tmpfs.c`)

tmpfs is a RAM-backed temporary filesystem mounted at `/tmp`.  It
stores files entirely in SRAM, with a configurable size limit (default
8KB from fstab).  Unlike romfs or UFS, tmpfs has no persistent backing
store — all contents are lost on reboot.

**Design:**

tmpfs uses a flat inode table and stores file data in dynamically
allocated kernel memory (kmem pages).

```c
/* src/kernel/fs/tmpfs.h */

extern const vfs_ops_t tmpfs_ops;

/* Maximum tmpfs entries (files + directories).
 * Each entry is ~48 bytes; 32 entries = ~1.5 KB overhead. */
#define TMPFS_INODE_MAX  32

/* Maximum total data across all tmpfs files.
 * Enforced at write time; sys_write returns -ENOSPC when exceeded. */
#define TMPFS_DATA_MAX   8192u   /* 8 KB default; overridable from fstab */
```

**On-memory inode:**

```c
typedef struct tmpfs_inode {
    char      name[VFS_NAME_MAX + 1]; /* filename */
    uint8_t   type;          /* VNODE_FILE or VNODE_DIR */
    uint32_t  mode;          /* permissions (0755, 0644, …) */
    uint32_t  size;          /* data bytes (files only) */
    uint8_t  *data;          /* pointer to data buffer (NULL for dirs) */
    uint32_t  parent_ino;    /* inode index of parent directory */
    uint32_t  ino;           /* self index (for stat) */
    uint8_t   active;        /* 1 = in use, 0 = free */
} tmpfs_inode_t;
```

**Supported operations:**

| Operation | Description |
|---|---|
| mount | Initialise inode table, create root directory (ino 0) |
| lookup | Linear scan of inodes where `parent_ino` matches directory |
| read | Copy from `data + offset` to user buffer |
| write | Extend `data` buffer (kmem realloc); enforce TMPFS_DATA_MAX |
| readdir | Iterate inodes with matching `parent_ino` |
| stat | Return type, size, mode from inode |
| create | Allocate inode, set type=FILE, name, parent_ino |
| mkdir | Allocate inode, set type=DIR, name, parent_ino |
| unlink | Free data buffer, mark inode inactive |
| truncate | Free data buffer, set size = 0 |

**Memory management:**

File data is allocated in small chunks from the kernel heap (kmem).
The total data budget (`TMPFS_DATA_MAX`) is tracked globally; when a
write would exceed it, `-ENOSPC` is returned.  This prevents tmpfs
from consuming unbounded SRAM.

**Key design rationale:**

- tmpfs is intentionally minimal — its primary purpose is providing
  `/tmp` for shell scripts and busybox applets that need temporary
  files.  8KB is sufficient for small temp files and pipes.
- No subdirectory nesting limit — the flat inode table with
  `parent_ino` linkage naturally supports nested directories.
- No symbolic links in tmpfs (not needed for `/tmp` use cases).


### Step 3 — Loopback Integration Test (QEMU)

Before building the UFS driver, verify that the loopback block device
correctly translates sector I/O into file reads/writes on a VFAT
image file.

**Test setup:**

The QEMU build's embedded FAT32 image (from Phase 4) is extended with
a small test file (`test_loop.bin`, 4096 bytes of known pattern data).
The test opens this file via VFAT, creates a loopback device from it,
and reads sectors through the block device interface.

**Test cases:**

| Test | Expected Result |
|---|---|
| `loopback_setup("/mnt/sd/test_loop.bin")` | Returns 0; `/dev/loop0` registered |
| `blkdev_find("loop0")` | Returns non-NULL `blkdev_t *` |
| `blkdev->sector_count` | Equals `4096 / 512 = 8` |
| Read sector 0 via `loop0` | Returns first 512 bytes of test pattern |
| Read sector 7 via `loop0` | Returns last 512 bytes of test pattern |
| Read sector 8 (out of range) | Returns `-EIO` |
| Write sector 0 via `loop0` + read back | Returns written data |
| `loopback_teardown(0)` | Returns 0; `blkdev_find("loop0")` returns NULL |
| `loopback_setup` × 3 + 4th call | 4th call returns `-ENOMEM` |

**tmpfs test (combined):**

| Test | Expected Result |
|---|---|
| `vfs_mount("/tmp", &tmpfs_ops, 0, NULL)` | Returns 0 |
| `open("/tmp/hello.txt", O_CREAT\|O_WRONLY)` | Returns valid fd |
| `write(fd, "hello", 5)` + `close` + re-open + `read` | Returns `"hello"` |
| `unlink("/tmp/hello.txt")` | Returns 0 |
| `open("/tmp/hello.txt")` after unlink | Returns `-ENOENT` |
| Write > TMPFS_DATA_MAX bytes | Returns `-ENOSPC` |
| `mkdir("/tmp/sub")` + `open("/tmp/sub/file")` | Works (nested dirs) |

---

## Week 2: UFS On-Disk Format, mkufs Tool, and Read-Only Driver

### Step 4 — UFS On-Disk Format (`src/kernel/fs/ufs_format.h`)

The UFS on-disk format is a simplified version of 4.4BSD's FFS (Fast
File System), designed for RP2040's resource constraints.  All fields
are little-endian (ARM native).

**Disk layout (for a UFS image of size S bytes):**

```
Block 0:     Superblock (4 KB)
Block 1:     Block bitmap (4 KB — covers up to 32768 blocks = 128 MB)
Block 2:     Inode bitmap (4 KB — covers up to 32768 inodes)
Block 3..N:  Inode table (N-2 blocks × 64 inodes/block)
Block N+1..: Data blocks
```

All blocks are 4KB (matching the page size, VFAT cluster alignment,
and SD card erase granularity).

**Superblock (128 bytes at offset 0 within block 0):**

```c
#define UFS_MAGIC  0x55465331u   /* "UFS1" in little-endian */
#define UFS_BLOCK_SIZE  4096u
#define UFS_INODE_SIZE    64u
#define UFS_INODES_PER_BLOCK  (UFS_BLOCK_SIZE / UFS_INODE_SIZE)  /* 64 */
#define UFS_DIRECT_BLOCKS    10
#define UFS_NAME_MAX         28  /* max filename in dir entry (matches VFS) */
#define UFS_ROOT_INO          1  /* inode 0 is reserved (unused / error sentinel) */

typedef struct {
    uint32_t s_magic;            /* UFS_MAGIC */
    uint32_t s_block_size;       /* always 4096 */
    uint32_t s_block_count;      /* total blocks in filesystem */
    uint32_t s_inode_count;      /* total inodes in filesystem */
    uint32_t s_free_blocks;      /* free block count */
    uint32_t s_free_inodes;      /* free inode count */
    uint32_t s_bmap_block;       /* block number of block bitmap (1) */
    uint32_t s_imap_block;       /* block number of inode bitmap (2) */
    uint32_t s_itable_block;     /* first block of inode table (3) */
    uint32_t s_data_block;       /* first data block */
    uint32_t s_inode_blocks;     /* number of blocks in inode table */
    uint8_t  s_pad[84];          /* pad to 128 bytes */
} ufs_super_t;
```

**Inode (64 bytes):**

```c
typedef struct {
    uint16_t i_mode;             /* file type + permissions (S_IFREG|0644, etc.) */
    uint16_t i_nlink;            /* hard link count */
    uint16_t i_uid;              /* owner (always 0 for now) */
    uint16_t i_gid;              /* group (always 0 for now) */
    uint32_t i_size;             /* file size in bytes */
    uint32_t i_mtime;            /* last modification time (UNIX epoch) */
    uint32_t i_ctime;            /* status change time */
    uint32_t i_direct[UFS_DIRECT_BLOCKS]; /* direct block pointers (10) */
    uint32_t i_indirect;         /* single-indirect block pointer */
    uint8_t  i_pad[4];           /* pad to 64 bytes */
} ufs_inode_t;
/* sizeof(ufs_inode_t) == 64 — 64 inodes per 4 KB block */
```

**Maximum file sizes:**

| Addressing | Blocks | Max Size |
|---|---|---|
| Direct only (10 blocks) | 10 | 40 KB |
| Direct + single indirect | 10 + 1024 | ~4 MB |
| With double indirect (future) | 10 + 1024 + 1M | ~4 GB |

Single-indirect is sufficient for Phase 5.  Double-indirect can be
added later if files > 4 MB are needed (unlikely for the initial
busybox-based system).

**Directory entry (32 bytes):**

```c
typedef struct {
    uint32_t d_ino;              /* inode number (0 = unused entry) */
    char     d_name[UFS_NAME_MAX]; /* filename (NUL-terminated) */
} ufs_dirent_t;
/* sizeof(ufs_dirent_t) == 32 — 128 entries per 4 KB block */
```

**Symbolic link storage:**

Symlink targets shorter than `UFS_DIRECT_BLOCKS × 4 = 40` bytes are
stored inline in the `i_direct[]` array (fast symlink).  Longer
targets are stored in a data block.

**Design rationale:**

- **4 KB blocks only (no fragments):** Simplifies allocation and
  avoids the complexity of BSD FFS fragment management.  The trade-off
  is wasted space for small files, but with image sizes of 32–128 MB
  this is acceptable.
- **Single cylinder group:** Image files are stored as contiguous
  extents on VFAT, so there is no physical geometry to optimise for.
  A single group eliminates cylinder group metadata overhead.
- **64-byte inodes:** The minimum needed for UNIX semantics.  The
  10 direct + 1 indirect scheme supports files up to ~4 MB, covering
  all typical command-line tool outputs and config files.
- **32-byte directory entries:** Fixed-size for simplicity.  The
  28-byte filename limit matches `VFS_NAME_MAX`.  Directory blocks
  hold 128 entries each — sufficient for typical `/usr/bin` with
  50–100 symlinks.


### Step 5 — mkufs Host Tool (`tools/mkufs/`)

A standalone C program compiled with the host `gcc` that creates
formatted UFS image files.

**Usage:**

```sh
mkufs [options] <output_file>
  -s SIZE     Image size (e.g., 64M, 32M, 1M).  Default: 1M.
  -i COUNT    Number of inodes.  Default: auto (1 per 4 KB of image).
  -p DIR      Populate from directory tree (optional).
  -v          Verbose: print layout and statistics.

# Examples:
mkufs -s 64M ppap_usr.img
mkufs -s 128M ppap_home.img
mkufs -s 32M ppap_var.img
mkufs -s 1M -p test_data/ test.img   # create + populate for testing
```

**Algorithm:**

1. Parse arguments, compute block count and inode count
2. Calculate layout:
   - Block 0: superblock
   - Block 1: block bitmap
   - Block 2: inode bitmap
   - Blocks 3..N: inode table (`ceil(inode_count / 64)` blocks)
   - Blocks N+1..: data blocks
3. Write superblock with computed offsets and counts
4. Clear block bitmap and inode bitmap
5. Mark metadata blocks as used in block bitmap
6. Allocate inode 1 (root directory):
   - `i_mode = S_IFDIR | 0755`
   - `i_nlink = 2` (self "." + parent "..")
7. Create root directory data block with "." and ".." entries
8. If `-p DIR` specified:
   - Recursively walk the host directory
   - Allocate inodes and data blocks for each file/dir/symlink
   - Copy file contents into data blocks
9. Write the complete image to the output file
10. If `-v`, print layout summary

**Verification mode:**

```sh
mkufs --dump image.img
```

Prints the superblock, inode table, and directory tree in human-readable
form.  Essential for debugging UFS format bugs before kernel testing.

**Build:**

```makefile
# tools/mkufs/Makefile
mkufs: mkufs.c ../../src/kernel/fs/ufs_format.h
	$(CC) -o $@ $< -I../../src/kernel/fs -Wall -Wextra
```

Integrated into CMake: a custom command runs `mkufs` during the build
to produce test UFS images for QEMU testing.

**QEMU build integration:**

For QEMU testing, `mkufs` creates a small UFS image (e.g., 64 KB)
populated with a few test files.  This image is embedded inside the
FAT32 test image (via `mkfatimg`), so the QEMU binary contains:
`ROM → FAT32 image → ppap_test.img (UFS) → test files`.


### Step 6 — UFS Read-Only Driver (`src/kernel/fs/ufs.c`)

The UFS driver implements the VFS operation table for read-only access
to a UFS filesystem on a loopback block device.  Write operations are
added in Steps 7–9.

**Mount:**

```c
static int ufs_mount(mount_entry_t *mnt, const void *dev_data) {
    const char *devname = (const char *)dev_data;  /* e.g., "loop0" */
    blkdev_t *bdev = blkdev_find(devname);
    if (!bdev) return -ENODEV;

    /* Read superblock (block 0) */
    uint8_t sb_buf[UFS_BLOCK_SIZE];
    if (bdev->read(bdev, sb_buf, 0, UFS_BLOCK_SIZE / BLKDEV_SECTOR_SIZE) < 0)
        return -EIO;
    const ufs_super_t *sb = (const ufs_super_t *)sb_buf;
    if (sb->s_magic != UFS_MAGIC) return -EINVAL;

    /* Allocate UFS-private mount data */
    ufs_mount_t *ufs = kmem_alloc(sizeof(ufs_mount_t));
    ufs->bdev = bdev;
    ufs->sb = *sb;  /* copy superblock to SRAM */
    mnt->sb_priv = ufs;

    /* Create root vnode (inode 1) */
    vnode_t *root = vnode_alloc();
    ufs_inode_t root_ino;
    ufs_read_inode(ufs, UFS_ROOT_INO, &root_ino);
    root->type = VNODE_DIR;
    root->size = root_ino.i_size;
    root->mode = root_ino.i_mode;
    root->ino  = UFS_ROOT_INO;
    root->mount = mnt;
    mnt->root = root;
    return 0;
}
```

**Inode reading:**

```c
/* Read inode `ino` from the inode table on the block device. */
static int ufs_read_inode(ufs_mount_t *ufs, uint32_t ino,
                          ufs_inode_t *out) {
    /* Compute which block of the inode table contains this inode */
    uint32_t inodes_per_block = UFS_BLOCK_SIZE / UFS_INODE_SIZE;
    uint32_t block = ufs->sb.s_itable_block + ino / inodes_per_block;
    uint32_t offset = (ino % inodes_per_block) * UFS_INODE_SIZE;

    /* Read the block, extract the inode */
    uint8_t blk_buf[UFS_BLOCK_SIZE];
    ufs_read_block(ufs, block, blk_buf);
    memcpy(out, blk_buf + offset, sizeof(ufs_inode_t));
    return 0;
}
```

**Block address resolution:**

```c
/* Return the physical block number for logical block `lblk` of a file.
 * Handles direct blocks (0–9) and single-indirect (10–1033). */
static uint32_t ufs_bmap(ufs_mount_t *ufs, const ufs_inode_t *ino,
                          uint32_t lblk) {
    if (lblk < UFS_DIRECT_BLOCKS) {
        return ino->i_direct[lblk];
    }
    /* Single indirect */
    uint32_t idx = lblk - UFS_DIRECT_BLOCKS;
    uint32_t ptrs_per_block = UFS_BLOCK_SIZE / sizeof(uint32_t);  /* 1024 */
    if (idx < ptrs_per_block && ino->i_indirect != 0) {
        uint32_t ind_buf[ptrs_per_block];
        ufs_read_block(ufs, ino->i_indirect, ind_buf);
        return ind_buf[idx];
    }
    return 0;  /* beyond file extent */
}
```

**Read:**

```c
static long ufs_read(vnode_t *vn, void *buf, size_t n, uint32_t off) {
    ufs_mount_t *ufs = (ufs_mount_t *)vn->mount->sb_priv;
    ufs_inode_t ino;
    ufs_read_inode(ufs, vn->ino, &ino);

    if (off >= ino.i_size) return 0;
    if (off + n > ino.i_size) n = ino.i_size - off;

    size_t total = 0;
    while (total < n) {
        uint32_t lblk = (off + total) / UFS_BLOCK_SIZE;
        uint32_t boff = (off + total) % UFS_BLOCK_SIZE;
        uint32_t pblk = ufs_bmap(ufs, &ino, lblk);
        if (pblk == 0) break;   /* sparse block → zeros */

        uint8_t blk_buf[UFS_BLOCK_SIZE];
        ufs_read_block(ufs, pblk, blk_buf);

        uint32_t chunk = UFS_BLOCK_SIZE - boff;
        if (chunk > n - total) chunk = n - total;
        memcpy((uint8_t *)buf + total, blk_buf + boff, chunk);
        total += chunk;
    }
    return (long)total;
}
```

**Lookup (directory search):**

```c
static int ufs_lookup(vnode_t *dir, const char *name, vnode_t **result) {
    ufs_mount_t *ufs = (ufs_mount_t *)dir->mount->sb_priv;
    ufs_inode_t dir_ino;
    ufs_read_inode(ufs, dir->ino, &dir_ino);

    /* Scan directory data blocks for matching entry */
    uint32_t nblocks = (dir_ino.i_size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t pblk = ufs_bmap(ufs, &dir_ino, b);
        if (pblk == 0) continue;

        uint8_t blk_buf[UFS_BLOCK_SIZE];
        ufs_read_block(ufs, pblk, blk_buf);
        ufs_dirent_t *entries = (ufs_dirent_t *)blk_buf;
        uint32_t nent = UFS_BLOCK_SIZE / sizeof(ufs_dirent_t);  /* 128 */

        for (uint32_t i = 0; i < nent; i++) {
            if (entries[i].d_ino == 0) continue;
            if (strncmp(entries[i].d_name, name, UFS_NAME_MAX) == 0) {
                /* Found — create vnode */
                ufs_inode_t file_ino;
                ufs_read_inode(ufs, entries[i].d_ino, &file_ino);
                vnode_t *vn = vnode_alloc();
                vn->ino   = entries[i].d_ino;
                vn->size  = file_ino.i_size;
                vn->mode  = file_ino.i_mode;
                vn->type  = S_ISDIR(file_ino.i_mode) ? VNODE_DIR :
                            S_ISLNK(file_ino.i_mode) ? VNODE_SYMLINK :
                            VNODE_FILE;
                vn->mount = dir->mount;
                *result = vn;
                return 0;
            }
        }
    }
    return -ENOENT;
}
```

**Readdir, stat, readlink:**

These follow the same pattern — read inode, traverse blocks, return
data.  Readlink for fast symlinks reads directly from `i_direct[]`;
for regular symlinks, reads the data block.

**UFS-private mount data:**

```c
typedef struct {
    blkdev_t   *bdev;    /* backing block device (loop0, etc.) */
    ufs_super_t sb;      /* in-memory copy of superblock */
} ufs_mount_t;
```

**Block-level I/O helper:**

```c
/* Read one 4 KB UFS block from the backing block device.
 * Translates UFS block number → sector number (× 8 sectors per block). */
static int ufs_read_block(ufs_mount_t *ufs, uint32_t block, void *buf) {
    uint32_t sector = block * (UFS_BLOCK_SIZE / BLKDEV_SECTOR_SIZE);
    uint32_t count  = UFS_BLOCK_SIZE / BLKDEV_SECTOR_SIZE;  /* 8 */
    return ufs->bdev->read(ufs->bdev, buf, sector, count);
}
```

**SRAM usage consideration:**

Each `ufs_read` / `ufs_lookup` call uses a 4 KB stack buffer for
the block read.  This is significant on RP2040 (process stacks are
4–8 KB).  Mitigation: use the I/O buffer region (24 KB at
0x20038000–0x2003DFFF) instead of stack buffers for UFS block reads.
The I/O buffer is shared with VFAT; access is serialised (single-core
kernel operation, no preemption during I/O).

```c
/* Shared I/O buffer in the reserved SRAM region */
extern uint8_t __io_buf_start[];  /* defined in linker script */
#define UFS_IO_BUF  ((uint8_t *)__io_buf_start)
/* Use UFS_IO_BUF instead of stack-allocated blk_buf */
```

---

## Week 3: UFS Write Support

### Step 7 — Block and Inode Allocation

Add block bitmap and inode bitmap management to the UFS driver.

**Block bitmap operations:**

```c
/* Allocate a free block.  Scans the block bitmap, marks the first
 * free bit as used, decrements s_free_blocks.
 * Returns the block number, or 0 if no free blocks. */
static uint32_t ufs_alloc_block(ufs_mount_t *ufs);

/* Free a block: clear its bit in the bitmap, increment s_free_blocks. */
static void ufs_free_block(ufs_mount_t *ufs, uint32_t block);
```

**Inode bitmap operations:**

```c
/* Allocate a free inode.  Returns the inode number (≥ 1), or 0. */
static uint32_t ufs_alloc_inode(ufs_mount_t *ufs);

/* Free an inode: clear its bit, increment s_free_inodes. */
static void ufs_free_inode(ufs_mount_t *ufs, uint32_t ino);
```

**Implementation:**

The block bitmap is a single 4 KB block (block 1), covering up to
32768 blocks.  For a 64 MB image (16384 blocks), only the first 2048
bytes of the bitmap are used.  The bitmap is read into the I/O buffer,
scanned for a zero bit, the bit is set, and the bitmap is written back.

Similarly, the inode bitmap (block 2) covers up to 32768 inodes.  For
typical images, 256–1024 inodes are provisioned.

**Superblock flush:**

After any allocation/deallocation, the superblock's `s_free_blocks` /
`s_free_inodes` counters are updated and the superblock block is
rewritten.  This ensures consistency after a single-write crash (the
bitmaps and superblock always agree on free counts).

**Inode write-back:**

```c
/* Write inode `ino` back to the inode table on disk. */
static int ufs_write_inode(ufs_mount_t *ufs, uint32_t ino,
                           const ufs_inode_t *inode);
```

This reads the inode table block, patches the specific inode entry,
and writes the block back.  A full read-modify-write cycle is needed
because multiple inodes share a 4 KB block.


### Step 8 — File Write, Create, and Truncate

**Write (`ufs_write`):**

```c
static long ufs_write(vnode_t *vn, const void *buf, size_t n,
                       uint32_t off) {
    ufs_mount_t *ufs = (ufs_mount_t *)vn->mount->sb_priv;
    ufs_inode_t ino;
    ufs_read_inode(ufs, vn->ino, &ino);

    size_t total = 0;
    while (total < n) {
        uint32_t lblk = (off + total) / UFS_BLOCK_SIZE;
        uint32_t boff = (off + total) % UFS_BLOCK_SIZE;

        uint32_t pblk = ufs_bmap(ufs, &ino, lblk);
        if (pblk == 0) {
            /* Allocate a new block */
            pblk = ufs_alloc_block(ufs);
            if (pblk == 0) return (total > 0) ? (long)total : -ENOSPC;
            ufs_set_block(ufs, &ino, lblk, pblk);
        }

        /* Read-modify-write */
        uint8_t *blk_buf = UFS_IO_BUF;
        ufs_read_block(ufs, pblk, blk_buf);
        uint32_t chunk = UFS_BLOCK_SIZE - boff;
        if (chunk > n - total) chunk = n - total;
        memcpy(blk_buf + boff, (const uint8_t *)buf + total, chunk);
        ufs_write_block(ufs, pblk, blk_buf);
        total += chunk;
    }

    /* Update inode size if file grew */
    if (off + total > ino.i_size) {
        ino.i_size = off + total;
    }
    /* Update timestamps */
    ino.i_mtime = current_time();
    ino.i_ctime = current_time();
    ufs_write_inode(ufs, vn->ino, &ino);
    vn->size = ino.i_size;
    return (long)total;
}
```

**Block pointer management (`ufs_set_block`):**

```c
/* Set the physical block pointer for logical block `lblk`.
 * For lblk < 10: update i_direct[lblk].
 * For lblk ≥ 10: allocate indirect block if needed, update entry. */
static void ufs_set_block(ufs_mount_t *ufs, ufs_inode_t *ino,
                           uint32_t lblk, uint32_t pblk);
```

When the first indirect block is needed (`lblk == 10` and
`ino->i_indirect == 0`), a new block is allocated for the indirect
table, zeroed, and its address stored in `i_indirect`.

**Create (`ufs_create`):**

```c
static int ufs_create(vnode_t *dir, const char *name, uint32_t mode,
                       vnode_t **result) {
    ufs_mount_t *ufs = (ufs_mount_t *)dir->mount->sb_priv;

    /* 1. Allocate a new inode */
    uint32_t new_ino = ufs_alloc_inode(ufs);
    if (new_ino == 0) return -ENOSPC;

    /* 2. Initialise the inode */
    ufs_inode_t inode = {0};
    inode.i_mode  = S_IFREG | (mode & 0777);
    inode.i_nlink = 1;
    inode.i_mtime = current_time();
    inode.i_ctime = current_time();
    ufs_write_inode(ufs, new_ino, &inode);

    /* 3. Add directory entry in parent */
    ufs_add_dirent(ufs, dir->ino, new_ino, name);

    /* 4. Return vnode */
    vnode_t *vn = vnode_alloc();
    vn->ino   = new_ino;
    vn->type  = VNODE_FILE;
    vn->size  = 0;
    vn->mode  = inode.i_mode;
    vn->mount = dir->mount;
    *result = vn;
    return 0;
}
```

**Truncate (`ufs_truncate`):**

Walks the block pointers and frees all data blocks beyond the new
size.  If the new size is 0, frees all blocks and the indirect block
(if any).

**Metadata write ordering:**

To maximise crash resilience without a journal, UFS follows the
traditional BSD write ordering:
1. Write data blocks
2. Update and write the inode (new size, block pointers)
3. Update and write the directory entry (for create)
4. Update and write the bitmaps and superblock

This ordering ensures that a crash at any point leaves the filesystem
in a state recoverable by fsck (at worst, some blocks are marked used
but unreferenced — leaked space, not data corruption).


### Step 9 — Directory Operations and Links

**mkdir (`ufs_mkdir`):**

1. Allocate new inode (type `S_IFDIR | 0755`, nlink = 2)
2. Allocate data block for new directory
3. Write "." entry (self) and ".." entry (parent) in new dir block
4. Add entry for new directory in parent
5. Increment parent's `i_nlink` (for ".." backlink)

**rmdir (`ufs_rmdir`):**

1. Verify directory is empty (only "." and ".." entries)
2. Remove entry from parent directory
3. Free the directory's data blocks
4. Free the inode
5. Decrement parent's `i_nlink`

**unlink (`ufs_unlink`):**

1. Look up the file's inode from the directory
2. Decrement `i_nlink`
3. If `i_nlink` reaches 0 and no open references: free all data
   blocks and the inode
4. Remove the directory entry (set `d_ino = 0`)

**link (`ufs_link` — hard links):**

1. Look up source inode
2. Verify source is not a directory (hard links to directories
   are not allowed)
3. Add directory entry in target parent with source inode number
4. Increment source inode's `i_nlink`

**symlink (`ufs_symlink`):**

1. Allocate new inode (type `S_IFLNK | 0777`)
2. If target length ≤ 40 bytes: store inline in `i_direct[]`
3. Otherwise: allocate data block, write target string
4. Set `i_size` = target length
5. Add directory entry in parent

**rename (`ufs_rename` — same FS only):**

1. Look up source entry in old parent directory
2. Add entry in new parent directory (or same directory for simple
   rename) with the same inode number
3. Remove entry from old parent directory
4. If moving to a different directory and source is a dir: update
   the source's ".." entry and adjust nlink counts

**Directory entry management helpers:**

```c
/* Add a directory entry: find a free slot (d_ino == 0) or append.
 * If the directory's last data block is full, allocate a new block. */
static int ufs_add_dirent(ufs_mount_t *ufs, uint32_t dir_ino,
                           uint32_t file_ino, const char *name);

/* Remove a directory entry: set d_ino = 0 in the matching slot. */
static int ufs_del_dirent(ufs_mount_t *ufs, uint32_t dir_ino,
                           const char *name);
```

---

## Week 4: fstab, Boot Integration, and Testing

### Step 10 — fstab Parser and Boot-Time Mount Sequence

**fstab file (`romfs/etc/fstab`):**

```
# device                      mountpoint  fstype  options
/dev/mmcblk0p1                /mnt/sd     vfat    rw
/mnt/sd/ppap_usr.img          /usr        ufs     loop,ro
/mnt/sd/ppap_home.img         /home       ufs     loop,rw
/mnt/sd/ppap_var.img          /var        ufs     loop,rw
none                          /tmp        tmpfs   rw,size=8k
none                          /dev        devfs   rw
none                          /proc       procfs  ro
```

**Parser (`src/kernel/fs/fstab.c`):**

```c
/* src/kernel/fs/fstab.h */

#define FSTAB_MAX_ENTRIES  8

typedef struct {
    char device[VFS_PATH_MAX];     /* device path or image path */
    char mountpoint[VFS_PATH_MAX]; /* mount point path */
    char fstype[16];               /* "vfat", "ufs", "tmpfs", "devfs", "procfs" */
    uint8_t flags;                 /* MNT_RDONLY, etc. */
    uint8_t loop;                  /* 1 if "loop" option present */
} fstab_entry_t;

/* Parse /etc/fstab from romfs into an array of entries.
 * Returns the number of entries parsed, or negative errno. */
int fstab_parse(fstab_entry_t *entries, int max_entries);

/* Execute all mount entries in order.  For "loop" entries, calls
 * loopback_setup() first, then mounts the UFS on the loop device.
 * Returns 0 on success, negative errno on first failure. */
int fstab_mount_all(const fstab_entry_t *entries, int count);
```

**Mount execution logic:**

```c
int fstab_mount_all(const fstab_entry_t *entries, int count) {
    for (int i = 0; i < count; i++) {
        const fstab_entry_t *e = &entries[i];

        if (strcmp(e->fstype, "devfs") == 0) {
            vfs_mount(e->mountpoint, &devfs_ops, e->flags, NULL);
        } else if (strcmp(e->fstype, "procfs") == 0) {
            vfs_mount(e->mountpoint, &procfs_ops, e->flags, NULL);
        } else if (strcmp(e->fstype, "tmpfs") == 0) {
            vfs_mount(e->mountpoint, &tmpfs_ops, e->flags, NULL);
        } else if (strcmp(e->fstype, "vfat") == 0) {
            /* Mount VFAT on block device */
            vfs_mount(e->mountpoint, &vfat_ops, e->flags, e->device);
        } else if (strcmp(e->fstype, "ufs") == 0 && e->loop) {
            /* Loopback mount: setup loop device, then mount UFS */
            int loop_idx = loopback_setup(e->device);
            if (loop_idx < 0) {
                uart_puts("fstab: loopback_setup failed for ");
                uart_puts(e->device);
                uart_puts("\n");
                continue;  /* skip this mount, try next */
            }
            char devname[8];
            fmt_loop_name(devname, loop_idx);  /* "loop0", "loop1", … */
            vfs_mount(e->mountpoint, &ufs_ops, e->flags, devname);
        }
    }
    return 0;
}
```

**Boot sequence update (`kmain()`):**

```c
void kmain(void) {
    /* … existing init: UART, PLL, mm_init, proc_init … */

    /* VFS + base mounts (romfs at /, devfs at /dev, procfs at /proc) */
    vfs_init();
    file_pool_init();
    blkdev_init();
    loopback_init();
    vfs_mount("/", &romfs_ops, MNT_RDONLY, (void *)__romfs_start);

    /* Parse fstab and mount everything */
    fstab_entry_t fstab[FSTAB_MAX_ENTRIES];
    int nent = fstab_parse(fstab, FSTAB_MAX_ENTRIES);
    if (nent > 0) {
        fstab_mount_all(fstab, nent);
    }

    /* fd stdio, exec /sbin/init … */
}
```

**Error handling:**

If the SD card is absent or a UFS image is missing, the corresponding
mount is skipped with a warning on UART.  The system boots with romfs
only, providing a limited shell.  This graceful degradation matches
the spec's requirement for SD-card-optional boot.

**Mount dependency ordering:**

The fstab entries must be ordered so that `/mnt/sd` is mounted before
any loopback mount that references files on `/mnt/sd`.  The parser
does not reorder entries — it trusts the fstab author (the build
system generates the correct order).


### Step 11 — QEMU Full Integration Test

**Test image construction:**

The build system creates a nested image structure for QEMU:

```
1. mkufs -s 64K -p test_ufs_data/ build/test_ufs.img
   → small UFS image with test files: hello.txt, subdir/world.txt
2. mkfatimg builds a FAT32 image containing:
   → test_ufs.img (the UFS image from step 1)
   → test_loop.bin (loopback test data from Step 3)
   → other VFAT test files
3. FAT32 image is linked into QEMU binary ROM
```

**Test sequence (in `main_qemu.c`):**

```
1. Boot with romfs at /
2. Mount ramblk as VFAT at /mnt/sd
3. Verify /mnt/sd/test_ufs.img exists via stat()
4. loopback_setup("/mnt/sd/test_ufs.img") → /dev/loop0
5. vfs_mount("/usr", &ufs_ops, 0, "loop0")
6. Read /usr/hello.txt → verify contents
7. Read /usr/subdir/world.txt → verify contents
8. stat("/usr/hello.txt") → verify type=FILE, permissions
9. getdents("/usr") → list entries, verify hello.txt + subdir present
10. Create /usr/newfile.txt, write data, read back → verify
11. mkdir("/usr/newdir") → verify
12. symlink("/usr/link", "hello.txt") → readlink → verify
13. unlink("/usr/newfile.txt") → verify removed
14. Mount tmpfs at /tmp, create + read + delete test files
15. Verify /proc/mounts lists all active mounts
16. Print "Phase 5 QEMU tests: PASS"
```

**Expected output:**

```
MM: SRAM memory map
MM:   pages   0x20005000–0x20037fff 204 KB (51 × 4 KB, all free)
VFS: mounting romfs at / (read-only)
VFS: mounting devfs at /dev (5 devices)
VFS: mounting procfs at /proc
BLK: registered mmcblk0 (XXX sectors)
VFS: mounting vfat at /mnt/sd
LOOP: setup loop0 → /mnt/sd/test_ufs.img (XXX sectors)
VFS: mounting ufs at /usr (loop0)
VFS: mounting tmpfs at /tmp (8 KB max)
TEST: /usr/hello.txt → "Hello from UFS!\n" OK
TEST: /usr/subdir/world.txt → "World!\n" OK
TEST: stat /usr/hello.txt → FILE 0644 OK
TEST: getdents /usr → [., .., hello.txt, subdir] OK
TEST: create + write + read /usr/newfile.txt OK
TEST: mkdir /usr/newdir OK
TEST: symlink + readlink /usr/link OK
TEST: unlink /usr/newfile.txt OK
TEST: tmpfs create + read + delete OK
TEST: /proc/mounts → 6 entries OK
Phase 5 QEMU tests: PASS
```


### Step 12 — Hardware Integration Test (PicoCalc)

**Prerequisites:**

- SD card formatted as FAT32
- `ppap_usr.img` created by mkufs and copied to SD card root
- Build: `cd build && ninja`
- Flash via SWD: `gdb-multiarch -x ppap.gdb build/ppap.elf`

**Test procedure (via UART console):**

1. Boot → verify kernel banner, clock speed, memory map
2. SD card detected and initialised (CMD0 → CMD8 → ACMD41 sequence)
3. VFAT mounted at `/mnt/sd`
4. `ppap_usr.img` found on VFAT, loopback device created
5. UFS mounted at `/usr` via loopback
6. Read test files from `/usr`
7. Create new files on `/usr`, read back, verify
8. tmpfs at `/tmp` functional
9. Full mount table displayed (all 6+ mounts active)

**Expected UART output:**

```
PicoPiAndPortable booting...
UART: 115200 bps @ 12 MHz XOSC
System clock: 133 MHz
MM: SRAM memory map
MM:   pages   0x20005000–0x20037fff 204 KB (51 × 4 KB, all free)
VFS: mounting romfs at /
SD: card detected, SDHC, XXX MB
VFS: mounting vfat at /mnt/sd
LOOP: setup loop0 → /mnt/sd/ppap_usr.img
VFS: mounting ufs at /usr (loop0)
VFS: mounting tmpfs at /tmp
hostname: ppap
Phase 5 hardware tests: PASS
```

**SD card preparation script (`scripts/prepare_sd.sh`):**

```sh
#!/bin/bash
# Format SD card and create UFS image files
# Usage: ./scripts/prepare_sd.sh /dev/sdX

DEVICE=$1
MOUNT=/tmp/ppap_sd

mkfs.vfat -F 32 ${DEVICE}1
mkdir -p $MOUNT
mount ${DEVICE}1 $MOUNT

# Create UFS images
./build/tools/mkufs -s 64M $MOUNT/ppap_usr.img
./build/tools/mkufs -s 128M $MOUNT/ppap_home.img
./build/tools/mkufs -s 32M $MOUNT/ppap_var.img

# Optional: populate /usr with test files
./build/tools/mkufs -s 64M -p test_usr/ $MOUNT/ppap_usr.img

sync
umount $MOUNT
echo "SD card ready"
```

---

## Deliverables

| File | Description |
|---|---|
| `src/kernel/blkdev/loopback.c` | Loopback block device: file → sector translation |
| `src/kernel/blkdev/loopback.h` | Loopback API: `loopback_setup`, `loopback_teardown`, `loopback_init` |
| `src/kernel/fs/ufs_format.h` | On-disk UFS structures: superblock, inode, dirent |
| `src/kernel/fs/ufs.c` | UFS driver: mount, lookup, read, write, readdir, stat, readlink, create, mkdir, unlink, rmdir, link, symlink, rename, truncate |
| `src/kernel/fs/ufs.h` | UFS driver API + `ufs_ops` extern |
| `src/kernel/fs/tmpfs.c` | tmpfs: RAM-backed filesystem for /tmp |
| `src/kernel/fs/tmpfs.h` | tmpfs API + `tmpfs_ops` extern |
| `src/kernel/fs/fstab.c` | fstab parser: read /etc/fstab, parse entries |
| `src/kernel/fs/fstab.h` | fstab API: `fstab_parse`, `fstab_mount_all` |
| `src/kernel/fs/devfs.c` | Extended: `/dev/loop0`, `/dev/loop1`, `/dev/loop2` |
| `src/kernel/syscall/sys_fs.c` | Extended: `sys_mount`, `sys_umount`, `sys_sync` |
| `src/kernel/syscall/syscall.c` | Updated dispatch table: mount (21), umount (22), sync (36) |
| `src/kernel/vfs/vfs.c` | Updated: `vfs_umount()` support |
| `src/kernel/main.c` | Updated: fstab-driven mount sequence, loopback_init |
| `src/kernel/main_qemu.c` | Updated: loopback + UFS integration tests |
| `tools/mkufs/mkufs.c` | Host tool: create formatted UFS image files |
| `tools/mkufs/Makefile` | Host build for mkufs |
| `romfs/etc/fstab` | Mount configuration file |
| `scripts/prepare_sd.sh` | SD card setup script for hardware testing |
| `user/test_ufs.c` | On-target test: UFS file operations |
| `user/test_loopback.c` | On-target test: loopback block I/O |
| `tests/test_ufs_unit.c` | Host-native unit tests for UFS format + driver |
| `tests/test_loopback_unit.c` | Host-native unit tests for loopback device |
| `CMakeLists.txt` | Updated: mkufs build, UFS test image generation |

---

## Known Risks and Mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| UFS format bugs (inode offset miscalculation in mkufs) | High | `mkufs --dump` mode prints layout; verify offsets before kernel testing |
| Loopback I/O stack depth (UFS → loop → VFAT → SD) causes stack overflow | High | Use shared I/O buffer region instead of stack-allocated 4 KB buffers; measure actual stack depth in QEMU |
| SRAM pressure from concurrent UFS + VFAT metadata caching | Medium | I/O buffer (24 KB) is shared; serialise access; VFAT FAT cache and UFS bitmap reads alternate, not overlap |
| Block bitmap corruption on power loss during write | Medium | Write-ordering (data → inode → bitmap → superblock) limits damage; mkufs creates a recoverable state; fsck planned for Phase 8 |
| UFS indirect block allocation exhausts free blocks silently | Low | Check `s_free_blocks` before allocation; return `-ENOSPC` early |
| fstab parsing bug skips or misordes mounts | Medium | Manual verification of fstab order; defensive `if (vfs_lookup(mnt_point))` check before each mount |
| Loopback write-through to VFAT is slow (each UFS block write → VFAT cluster write → SD sector write) | High | Acceptable for Phase 5 (correctness first); Phase 8 adds write buffering / delayed allocation |
| QEMU nested image (ROM → FAT32 → UFS) too large for ROM section | Low | Keep test UFS image small (64 KB); FAT32 image total ≤ 256 KB; fits in 16 MB ROM |
| tmpfs exhausts SRAM if user writes large temp files | Low | Enforce `TMPFS_DATA_MAX` (8 KB); return `-ENOSPC` when exceeded |
| UFS directory search is O(n) per lookup (linear scan) | Low | Acceptable for typical directory sizes (< 128 entries per block); optimisation deferred to Phase 8 |
| Hard links create inode refcount bugs (premature free) | Medium | Track `i_nlink` carefully; only free inode when `i_nlink == 0` AND no open vnodes reference it |
| Symlink fast path (inline in i_direct[]) collides with block pointer interpretation | Low | Fast symlinks are identified by `S_ISLNK(i_mode)`; code never interprets i_direct[] as block pointers for symlink inodes |

---

## References

- [RP2040 Datasheet](https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf) — §2.6 (SSI/XIP), §4.7.1 (SRAM banks)
- [4.4BSD FFS / UFS](https://docs.freebsd.org/en/books/design-44bsd/) — Chapter 7: File Systems (inode structure, allocation policies)
- [Linux loop device](https://www.kernel.org/doc/html/latest/admin-guide/blockdev/loop.html) — Loopback block device concept
- [PicoPiAndPortable Design Spec v0.4](PicoPiAndPortable-spec-v04.md) — §3.5 (Loopback), §3.8 (UFS), §3.9 (FS Layout), §3.10 (fstab)
- [Phase 4 Plan](phase4-plan.md) — Block device abstraction, VFS write extensions, VFAT driver
