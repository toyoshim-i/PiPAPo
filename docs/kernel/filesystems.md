# PPAP Filesystem Architecture

VFS layer and filesystem driver design.

---

## VFS Overview (`src/kernel/vfs/`)

The Virtual Filesystem Switch provides a unified interface over multiple
filesystem types. Key abstractions:

- **vnode** — in-memory representation of a file/directory (64-entry pool from kernel heap)
- **file_ops** — per-filesystem operations (open, read, write, readdir, stat, close)
- **mount table** — 8-entry fixed-size array; path resolution uses longest-prefix matching

### Path Resolution ("namei")

Component-by-component walk from root or CWD:
1. Split path by `/`
2. For each component, call the filesystem's `lookup` op
3. Check mount table at each directory — if mounted, switch to mounted root
4. Symlink depth limit: 8 (prevents infinite loops)

## romfs (`src/kernel/fs/romfs.c`)

Read-only filesystem. On ARM targets with QSPI flash, accessed directly via XIP.
On other targets (m68k), the romfs image is loaded into RAM.

### On-Flash Format

```
+-------------------+
| Superblock        |  magic "PPAP", total_size, file_count, root_offset
+-------------------+
| Entry 0           |  name, type, size, data_offset, sibling_offset, child_offset
| Entry 1           |
| ...               |
+-------------------+
| File data         |  4-byte aligned for XIP compatibility
+-------------------+
```

All offsets are relative to the romfs base address.
Entries use sibling/child offsets for flat directory traversal (no pointer chains).
File data is 4-byte aligned for XIP compatibility on ARM targets.

### mkromfs (`tools/mkromfs/`)

Host tool that generates `romfs.bin` from a directory tree:
1. First pass: compute sizes and assign offsets
2. Second pass: serialize entries and file data
3. Output: flat binary, appended to the kernel image in flash

## VFAT / FAT32 (`src/kernel/fs/vfat.c`)

SD card filesystem for PC/Mac interoperability.

- BPB (BIOS Parameter Block) parsing at mount time
- Cluster chain traversal with FAT sector caching
- VFAT long filename (LFN) support (13 chars per LFN entry)
- Read/write support for files and directories
- Mounted at `/mnt/sd` by fstab

## UFS (`src/kernel/vfs/ufs.c`)

UNIX-style filesystem driver with a 44BSD-compatible on-disk layout.

- UFS1-compatible superblock and inode layout
- 4 KB block size / 512 B fragments
- Variable-length BSD directory entries
- Standard UNIX file types (regular, directory, symlink)
- Used for PPAP image files and boot media UFS partitions

See [`ufs.md`](ufs.md) for layout details and invariants.

## Loopback Block Device (`src/kernel/blkdev/loop.c`)

Translates block I/O (512-byte sectors) to file operations on the
underlying VFAT filesystem:

```
loopback read(sector N) → lseek(image_fd, N*512) + read(512)
```

Up to 4 loopback devices (`/dev/loop0` – `/dev/loop3`).

## devfs (`src/kernel/fs/devfs.c`)

Static table of device nodes, mounted at `/dev`:

| Node | Type | Description |
|---|---|---|
| `/dev/null` | char | Discards writes, reads return EOF |
| `/dev/zero` | char | Reads return zeroes |
| `/dev/urandom` | char | Random bytes (hardware RNG or PRNG) |
| `/dev/ttyS0` | char | UART serial console |
| `/dev/tty1` | char | LCD + keyboard console (PicoCalc) |
| `/dev/backlight` | char | LCD backlight brightness (write 0–255) |
| `/dev/power` | char | System power control (write "off") |

## procfs (`src/kernel/fs/procfs.c`)

Virtual filesystem at `/proc` with system and per-process information.
See [procfs.md](/docs/kernel/procfs.md) for the complete file list and format.

## tmpfs (`src/kernel/fs/tmpfs.c`)

RAM-backed filesystem for `/tmp` and `/var/run`:
- File create, read, write, unlink
- Data stored in dynamically allocated pages
- Lost on reboot (no persistence)

## fstab (`romfs/etc/fstab`)

Parsed sequentially at boot by `kmain()`:

```
/dev/mmcblk0p1  /mnt/sd  vfat  defaults  0  0
/mnt/sd/ppap_usr.img  /usr  ufs  loop  0  0
/mnt/sd/ppap_home.img  /home  ufs  loop  0  0
/mnt/sd/ppap_var.img  /var  ufs  loop  0  0
none  /tmp  tmpfs  defaults  0  0
```

On targets without SD card (pico1, qemu_arm, qemu_m68k), fstab entries
that reference `/dev/mmcblk0` are silently skipped.

## Block Device Layer (`src/kernel/blkdev/`)

Sector-granular (512 B) abstraction:

```c
int blkdev_read(blkdev_t *dev, uint32_t sector, void *buf);
int blkdev_write(blkdev_t *dev, uint32_t sector, const void *buf);
```

Implementations: SD card (SPI), RAM disk (QEMU), loopback.

## Related Documentation

- [kernel.md](/docs/kernel/overview.md) — Kernel internals
- [procfs.md](/docs/kernel/procfs.md) — /proc filesystem details
- [syscall.md](/docs/kernel/syscall.md) — File I/O system calls
- [PicoCalc.md](/docs/reference/picocalc.md) — SD card hardware interface
