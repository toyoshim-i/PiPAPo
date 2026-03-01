# Phase 2: romfs + VFS — Detailed Plan

**PicoPiAndPortable Development Roadmap**
Estimated Duration: 3 weeks

---

## Goals

Build the Virtual File System abstraction, a read-only romfs driver for
external flash, a host-side mkromfs tool, and pseudo-filesystems (devfs,
procfs).  After this phase the kernel can mount the flash root filesystem
and serve open/read/close/stat/readdir on files stored in romfs.

**Exit Criteria (all must pass before moving to Phase 3):**
- VFS layer routes open/close/read/write/lseek/stat/readdir through a mount table to the correct FS driver
- mkromfs host tool generates a romfs image from a directory tree; image linked into the flash binary
- romfs mounted as `/` at boot; files readable via `sys_open` + `sys_read`
- Symlink resolution works (essential for busybox multicall: `/bin/ls` → `/bin/busybox`)
- devfs mounted at `/dev`; `/dev/null`, `/dev/zero`, `/dev/ttyS0` functional
- procfs mounted at `/proc`; `/proc/meminfo` readable
- `sys_open`, `sys_close`, `sys_read`, `sys_write`, `sys_lseek`, `sys_stat`, `sys_getdents` all implemented and routed through VFS
- QEMU smoke test: open a romfs file, read its contents, print to stdout
- Flash image build: single `ninja` invocation produces kernel + romfs image

---

## Source Tree After Phase 2

```
src/
  boot/
    startup.S             (existing)
  kernel/
    main.c                (existing — add vfs_init, romfs_mount, devfs_mount,
                                       procfs_mount calls)
    main_qemu.c           (existing — same mount sequence)
    mm/
      page.c/h            (existing)
      kmem.c/h            (existing)
      mpu.c/h             (existing)
    proc/
      proc.c/h            (existing)
      sched.c/h           (existing)
      switch.S            (existing)
    fd/
      file.h              (existing — extend struct file with vnode pointer)
      fd.c/h              (existing)
      tty.c/h             (existing)
    vfs/
      vfs.c / vfs.h       # VFS layer: mount table, vnode cache, path lookup
      namei.c             # Path resolution: component-by-component walk
    fs/
      romfs.c / romfs.h   # romfs driver: mount, lookup, read, readdir, stat
      romfs_format.h      # On-flash romfs structures (shared with mkromfs)
      devfs.c / devfs.h   # devfs pseudo-FS: /dev/null, zero, ttyS0, urandom
      procfs.c / procfs.h # procfs pseudo-FS: /proc/meminfo, /proc/version
    syscall/
      syscall.c           (existing — add open, close, lseek, stat, getdents)
      sys_io.c            (existing — update read/write to use VFS)
      sys_fs.c            # New: open, close, lseek, stat, getdents, getcwd, chdir
      sys_proc.c          (existing)
      sys_time.c          (existing)
    smp.c/h               (existing)
    xip_test.c/h          (existing)
  drivers/
    uart.c/h              (existing)
    uart_qemu.c           (existing)
    clock.c/h             (existing)
tools/
  mkromfs/
    mkromfs.c             # Host tool: directory tree → romfs binary image
    Makefile              # Build with host gcc
romfs/                    # Test romfs content (built into image)
  etc/
    hostname              # "ppap"
    motd                  # Message of the day
  bin/                    # Empty for now; Phase 3 adds test ELF binaries
```

---

## Week 1: VFS Layer and Path Resolution

### Step 1 — VFS Data Structures (`src/kernel/vfs/vfs.h`)

The VFS layer is the central abstraction that allows multiple filesystem
types (romfs, devfs, procfs, and later UFS/tmpfs) to coexist behind a
uniform interface.

**Design choices:**

- **vnode** (virtual node): in-memory representation of a file or directory.
  Each mounted FS provides its own inode data; the vnode wraps it with a
  uniform interface.  vnode count is limited (fixed pool, ~64 entries from
  a kernel heap page) — vnodes are allocated on open and released on last
  close.
- **Mount table**: fixed-size array of mount entries.  Each entry maps a
  mount-point path to a FS driver + superblock.  Up to 8 mounts (romfs `/`,
  devfs `/dev`, procfs `/proc`, and later tmpfs `/tmp`, UFS `/usr`,
  `/home`, `/var`).
- **FS operations vector**: each FS driver registers a `struct vfs_ops`
  providing mount, lookup, read, write, readdir, stat, and readlink.
  Create/unlink/mkdir are added in Phase 4 for writable FS.

**Core types:**

```c
/* src/kernel/vfs/vfs.h */
#define VFS_MOUNT_MAX   8
#define VFS_VNODE_MAX   64
#define VFS_NAME_MAX    28    /* max filename component (romfs aligned to 32 B) */
#define VFS_PATH_MAX    128   /* max absolute path length */

typedef enum { VNODE_FILE, VNODE_DIR, VNODE_SYMLINK, VNODE_DEV } vnode_type_t;

typedef struct vnode {
    vnode_type_t  type;
    uint32_t      size;         /* file size in bytes */
    uint32_t      mode;         /* permissions (Phase 3+; 0755 default) */
    uint32_t      ino;          /* FS-specific inode number / offset */
    uint32_t      refcnt;       /* open reference count */
    void         *fs_priv;      /* FS-specific data (romfs_entry *, devfs node, …) */
    struct mount_entry *mount;  /* which FS owns this vnode */
    const void   *xip_addr;    /* XIP flash address for direct execution (NULL if N/A) */
} vnode_t;

typedef struct vfs_ops {
    int     (*mount)  (struct mount_entry *mnt, const void *dev_data);
    int     (*lookup) (vnode_t *dir, const char *name, vnode_t **result);
    ssize_t (*read)   (vnode_t *vn, void *buf, size_t n, uint32_t off);
    ssize_t (*write)  (vnode_t *vn, const void *buf, size_t n, uint32_t off);
    int     (*readdir)(vnode_t *dir, struct dirent *entries, size_t max_entries,
                       uint32_t *cookie);
    int     (*stat)   (vnode_t *vn, struct stat *st);
    ssize_t (*readlink)(vnode_t *vn, char *buf, size_t bufsiz);
} vfs_ops_t;

typedef struct mount_entry {
    char            path[VFS_PATH_MAX];  /* mount point (e.g., "/", "/dev") */
    uint8_t         path_len;
    const vfs_ops_t *ops;                /* FS driver operations */
    vnode_t         *root;               /* root vnode of this FS */
    void            *sb_priv;            /* superblock / FS-private data */
    uint8_t         flags;               /* MNT_RDONLY, etc. */
} mount_entry_t;

void      vfs_init(void);
int       vfs_mount(const char *path, const vfs_ops_t *ops, const void *dev_data);
vnode_t  *vnode_alloc(void);
void      vnode_free(vnode_t *vn);
```

**Key design rationale:**

- `xip_addr` on `vnode_t` is critical: when the ELF loader (Phase 3)
  opens a binary on romfs, it reads `xip_addr` to know where the .text
  section lives in flash — no copying to SRAM needed.
- `vfs_ops.readlink` is separate from `read` because symlink targets are
  metadata, not file data — they are stored differently in romfs.
- No `create/mkdir/unlink` in Phase 2 — these are added in Phase 4 for
  UFS.  romfs is read-only; devfs/procfs have fixed entries.

### Step 2 — Path Resolution (`src/kernel/vfs/namei.c`)

Path resolution ("namei") walks an absolute path component by component,
consulting the mount table at each step to cross FS boundaries.

**Algorithm:**

```
namei(path) → vnode
  1. Start at root mount (mount_table[0], where path = "/")
  2. Split path into components: "/dev/ttyS0" → ["dev", "ttyS0"]
  3. For each component:
     a. Call current_mount->ops->lookup(current_dir_vnode, component, &result)
     b. If result is a mount point → switch to that mount's root vnode
     c. If result is a symlink → readlink, restart from link target (depth limit = 8)
     d. current_dir_vnode = result
  4. Return final vnode
```

**API:**

```c
/* src/kernel/vfs/namei.c */
int vfs_lookup(const char *path, vnode_t **result);

/* Internal: find the mount entry that covers 'path' */
mount_entry_t *vfs_find_mount(const char *path, const char **remainder);
```

**Mount point matching:** `vfs_find_mount` finds the longest-prefix mount
entry.  For path `/dev/ttyS0`: mount `/` matches with remainder
`dev/ttyS0`, but mount `/dev` is longer and matches with remainder `ttyS0`.
The longest match wins.

**Symlink loop protection:** A depth counter limits symlink resolution to
8 levels.  If exceeded, return `-ELOOP`.

**Gotcha:** `.` and `..` must be handled during path walk.  `.` is a no-op
(stays at current directory).  `..` goes to the parent — but at a mount
point boundary, `..` must cross back to the parent mount's directory.
Simplification for Phase 2: track the full resolved path as a string during
the walk; `..` truncates the last component.  This avoids needing parent
pointers on vnodes.

### Step 3 — VFS-Routed Syscalls (`src/kernel/syscall/sys_fs.c`)

Extend the syscall table with file system operations.  All go through the
VFS layer — no FS-specific code in the syscall implementations.

**New syscalls:**

| Syscall | Number | Implementation |
|---|---|---|
| `open(path, flags, mode)` | 5 | `vfs_lookup` → allocate `struct file` → `fd_alloc` |
| `close(fd)` | 6 | `fd_get` → `vnode_free` (decrement refcnt) → `fd_free` |
| `lseek(fd, off, whence)` | 19 | Update `file->offset` (SEEK_SET/CUR/END) |
| `stat(path, buf)` | 106 | `vfs_lookup` → `ops->stat` → copy to user buf |
| `fstat(fd, buf)` | 108 | `fd_get` → vnode's `ops->stat` → copy to user buf |
| `getdents(fd, buf, n)` | 141 | `fd_get` → `ops->readdir` with cookie |
| `getcwd(buf, size)` | 183 | Copy `current->cwd` to user buf |
| `chdir(path)` | 12 | `vfs_lookup` → verify dir → update `current->cwd` |

**Integration with existing fd layer:**

`struct file` (from Phase 1) gains a `vnode` pointer and an `offset`:

```c
struct file {
    const struct file_ops *ops;
    void                  *priv;
    uint32_t               flags;
    uint32_t               refcnt;
    vnode_t               *vnode;    /* NEW: backing vnode (NULL for legacy tty files) */
    uint32_t               offset;   /* NEW: current file position */
};
```

A new `vfs_file_ops` bridges VFS calls to the `file_ops` interface:

```c
static ssize_t vfs_file_read(struct file *f, char *buf, size_t n) {
    ssize_t ret = f->vnode->mount->ops->read(f->vnode, buf, n, f->offset);
    if (ret > 0) f->offset += ret;
    return ret;
}
```

The existing tty files (fd 0/1/2) continue to work unchanged — their
`vnode` is NULL and they use `tty_fops` directly.

---

## Week 2: romfs Format, Host Tool, and Driver

### Step 4 — romfs On-Flash Format (`src/kernel/fs/romfs_format.h`)

The romfs format is shared between the host-side `mkromfs` tool and the
kernel driver.  All fields are little-endian (ARM native byte order).
All structures are 4-byte aligned for direct Thumb instruction fetch.

**Superblock (16 bytes at romfs image start):**

```c
#define ROMFS_MAGIC  0x50504653u   /* "PPFS" in little-endian */

typedef struct {
    uint32_t magic;       /* ROMFS_MAGIC */
    uint32_t size;        /* total image size in bytes */
    uint32_t file_count;  /* total number of entries (files + dirs + symlinks) */
    uint32_t root_off;    /* offset from image start to root directory entry */
} romfs_super_t;
```

**Directory/file entry (variable length, 4-byte aligned):**

```c
#define ROMFS_TYPE_FILE    0u
#define ROMFS_TYPE_DIR     1u
#define ROMFS_TYPE_SYMLINK 2u

typedef struct {
    uint32_t next_off;    /* offset to next sibling (0 = last); relative to image start */
    uint32_t type;        /* ROMFS_TYPE_FILE / DIR / SYMLINK */
    uint32_t size;        /* data size: file content / symlink target length */
    uint32_t child_off;   /* for DIR: offset to first child; for FILE/SYMLINK: 0 */
    uint32_t name_len;    /* length of name (excluding NUL, before padding) */
    /* char name[] follows — NUL-terminated, padded to 4-byte boundary */
    /* char data[] follows name — file content or symlink target, 4-byte aligned */
} romfs_entry_t;
```

**Layout example for a minimal romfs:**

```
Offset 0x0000: romfs_super { magic=PPFS, size=…, file_count=5, root_off=0x10 }
Offset 0x0010: entry "/" (DIR, child_off=0x0030, next=0)
Offset 0x0030: entry "etc" (DIR, child_off=0x004C, next=0x0080)
Offset 0x004C: entry "hostname" (FILE, size=5, data="ppap\n")
Offset 0x0068: entry "motd" (FILE, size=…, data="Welcome to PicoPiAndPortable\n")
Offset 0x0080: entry "bin" (DIR, child_off=0x009C, next=0)
Offset 0x009C: entry "ls" (SYMLINK, data="busybox")
…
```

**Design rationale:**

- **Flat offsets** (not pointers): the entire romfs image is position-
  independent relative to its base address.  The kernel adds the flash base
  (`0x10011000`) to each offset at runtime.
- **Separate `type` field** (not packed into `next_off`): clearer code, and
  we have 4 bytes to spare — romfs is read-only, so compactness matters
  less than clarity.
- **No per-entry checksums**: the entire flash image is verified at flash
  time.  Runtime CRC would be too slow for XIP.
- **4-byte alignment everywhere**: required for Thumb instruction fetch if
  a romfs file contains ELF .text executed directly via XIP.

### Step 5 — mkromfs Host Tool (`tools/mkromfs/`)

A standalone C program compiled with the host `gcc` (not cross-compiled).
It reads a directory tree from the host filesystem and produces a binary
romfs image suitable for concatenation with the kernel in the flash image.

**Usage:**

```sh
mkromfs <input_dir> <output_file>
# Example:
mkromfs romfs/ build/romfs.bin
```

**Algorithm:**

1. Recursively walk `input_dir`
2. For each directory: emit a `romfs_entry_t` with `type=DIR`
3. For each regular file: emit entry with `type=FILE`; copy file data
   (4-byte aligned)
4. For each symlink: emit entry with `type=SYMLINK`; store link target
5. Compute sibling and child offsets during a two-pass layout:
   - Pass 1: compute sizes and assign offsets
   - Pass 2: write entries with correct offsets
6. Prepend `romfs_super_t`
7. Pad total image to 4-byte alignment

**Build:**

```makefile
# tools/mkromfs/Makefile
mkromfs: mkromfs.c ../../src/kernel/fs/romfs_format.h
	$(CC) -o $@ $< -I../../src/kernel/fs
```

Integrated into CMake: a custom command runs `mkromfs` during the build to
produce `build/romfs.bin`.

**Verification:** `mkromfs --dump <file>` mode prints the generated image
in human-readable form (entry names, types, offsets, sizes) to catch
offset bugs before flashing.

### Step 6 — romfs Driver (`src/kernel/fs/romfs.c`)

The kernel-side romfs driver implements the `vfs_ops_t` interface for
read-only access to the flash-resident romfs image.

**Mount:**

```c
static int romfs_mount(mount_entry_t *mnt, const void *dev_data) {
    /* dev_data = flash base address of romfs image (0x10011000 on RP2040) */
    const romfs_super_t *sb = (const romfs_super_t *)dev_data;
    if (sb->magic != ROMFS_MAGIC) return -EINVAL;
    mnt->sb_priv = (void *)dev_data;   /* store image base address */
    /* Allocate root vnode */
    vnode_t *root = vnode_alloc();
    root->type = VNODE_DIR;
    root->ino  = sb->root_off;
    root->mount = mnt;
    mnt->root = root;
    return 0;
}
```

**Lookup:**

```c
static int romfs_lookup(vnode_t *dir, const char *name, vnode_t **result) {
    const uint8_t *base = (const uint8_t *)dir->mount->sb_priv;
    const romfs_entry_t *entry = get_entry(base, dir->ino);
    uint32_t child_off = entry->child_off;
    while (child_off) {
        const romfs_entry_t *child = get_entry(base, child_off);
        const char *child_name = get_name(child);
        if (strcmp(child_name, name) == 0) {
            *result = vnode_from_entry(dir->mount, child, child_off);
            return 0;
        }
        child_off = child->next_off;
    }
    return -ENOENT;
}
```

**Read (direct flash access via XIP):**

```c
static ssize_t romfs_read(vnode_t *vn, void *buf, size_t n, uint32_t off) {
    if (off >= vn->size) return 0;
    if (off + n > vn->size) n = vn->size - off;
    const uint8_t *data = get_file_data(vn);   /* returns flash XIP address */
    memcpy(buf, data + off, n);
    return (ssize_t)n;
}
```

**XIP address calculation:**

For ELF binaries, the `xip_addr` field on the vnode points to the start of
file data in flash.  The ELF loader (Phase 3) uses this to execute .text
directly without copying to SRAM.

```c
static vnode_t *vnode_from_entry(mount_entry_t *mnt, const romfs_entry_t *e,
                                  uint32_t off) {
    vnode_t *vn = vnode_alloc();
    vn->size = e->size;
    vn->ino  = off;
    vn->mount = mnt;
    vn->type = (e->type == ROMFS_TYPE_DIR) ? VNODE_DIR :
               (e->type == ROMFS_TYPE_SYMLINK) ? VNODE_SYMLINK : VNODE_FILE;
    if (vn->type == VNODE_FILE) {
        vn->xip_addr = get_file_data_ptr(mnt->sb_priv, e);
    }
    return vn;
}
```

**Readdir:**

Iterates the sibling chain starting from `dir->child_off`.  A `cookie`
(byte offset into the sibling chain) allows `getdents` to resume across
multiple calls.

**Stat:**

Returns file type, size, and default permissions (0755 for dirs/executables,
0644 for regular files).  Times are all set to 0 (romfs has no timestamps).

### Step 7 — Flash Image Build Pipeline

The final flash image contains three regions:

| Region | Flash Address | Contents |
|---|---|---|
| Stage 1 | `0x10000000` | SDK boot2 + stage1 extension (existing) |
| Kernel | `0x10001000` | Kernel .text / .rodata (existing) |
| romfs | `0x10011000` | romfs image (new) |

**Build integration (CMakeLists.txt):**

1. Build `mkromfs` from `tools/mkromfs/mkromfs.c` using host compiler
2. Run `mkromfs romfs/ build/romfs.bin` to generate the romfs image
3. Convert the binary to a linkable `.o` with a `.romfs` section:

```sh
arm-none-eabi-objcopy -I binary -O elf32-littlearm \
  --rename-section .data=.romfs,alloc,load,readonly \
  build/romfs.bin build/romfs.o
```

4. Link `romfs.o` into the final ELF

**Linker script update (`ppap.ld`):**

```ld
FLASH_ROMFS (rx) : ORIGIN = 0x10011000, LENGTH = 16M - 68K

.romfs : {
    _romfs_start = .;
    KEEP(*(.romfs))
    _romfs_end = .;
} > FLASH_ROMFS
```

**QEMU:** `qemu.ld` gets a matching `ROMFS` region in ROM.  The romfs
driver uses `_romfs_start` as the base address — works identically on
hardware and QEMU.

**Linker symbols used by the kernel:**

```c
extern const uint8_t _romfs_start[];
extern const uint8_t _romfs_end[];
```

The romfs driver passes `_romfs_start` to `vfs_mount` as `dev_data`.

---

## Week 3: Pseudo-Filesystems and Integration

### Step 8 — devfs (`src/kernel/fs/devfs.c`)

devfs is a RAM-resident pseudo-filesystem providing device files.  It
has no on-disk backing; all entries are created at mount time from a
static table.

**Devices in Phase 2:**

| Device File | Description |
|---|---|
| `/dev/null` | read → EOF; write → discard |
| `/dev/zero` | read → zero-filled buf; write → discard |
| `/dev/ttyS0` | read/write → existing UART tty driver |
| `/dev/urandom` | read → RP2040 ROSC-based random bytes |

**Implementation:**

devfs maintains a static array of device entries.  Lookup matches by name.
Read/write dispatch to device-specific handlers.

```c
typedef struct {
    const char     *name;
    vnode_type_t    type;
    ssize_t (*read)(void *buf, size_t n, uint32_t off);
    ssize_t (*write)(const void *buf, size_t n, uint32_t off);
} devfs_node_t;

static const devfs_node_t devfs_nodes[] = {
    { "null",    VNODE_DEV, devnull_read,  devnull_write },
    { "zero",    VNODE_DEV, devzero_read,  devnull_write },
    { "ttyS0",   VNODE_DEV, devtty_read,   devtty_write  },
    { "urandom", VNODE_DEV, devrandom_read, devnull_write },
};
```

**`/dev/ttyS0` integration:** The devfs tty entry delegates to the existing
`uart_putc` / `uart_getc` from Phase 1.  Once devfs is mounted, processes
opened via `open("/dev/ttyS0", ...)` get a VFS-backed file, while the
boot-time fd 0/1/2 still use the legacy `tty_fops` path.

**`/dev/urandom` implementation:**

The RP2040 has a ring oscillator (ROSC) whose random bit output is available
at `ROSC_RANDOMBIT` (`0x40060000 + 0x1C`).  Reading this register returns a
single random bit.  To fill a buffer, read 8 bits per byte:

```c
static ssize_t devrandom_read(void *buf, size_t n, uint32_t off) {
    (void)off;
    uint8_t *p = buf;
    for (size_t i = 0; i < n; i++) {
        uint8_t byte = 0;
        for (int b = 0; b < 8; b++)
            byte = (byte << 1) | (ROSC_RANDOMBIT & 1);
        p[i] = byte;
    }
    return (ssize_t)n;
}
```

**QEMU note:** ROSC does not exist on mps2-an500.  `devrandom_read` falls
back to a simple LFSR seeded with `SysTick->VAL` on QEMU.

### Step 9 — procfs Skeleton (`src/kernel/fs/procfs.c`)

procfs exposes kernel state as virtual files.  In Phase 2, two entries:

| Path | Content |
|---|---|
| `/proc/meminfo` | Free pages count, total pages, page size |
| `/proc/version` | `"PicoPiAndPortable v0.2 (armv6m)"` |

**Implementation:**

procfs uses the same pattern as devfs: a static table of entries, each with
a `generate` function that fills a buffer on read.  The data is produced
dynamically — there is no cached content.

```c
static ssize_t procfs_meminfo(void *buf, size_t n, uint32_t off) {
    char tmp[128];
    int len = fmt_meminfo(tmp, sizeof(tmp));   /* hand-rolled formatter */
    if (off >= (uint32_t)len) return 0;
    if (n > (size_t)(len - off)) n = len - off;
    memcpy(buf, tmp + off, n);
    return (ssize_t)n;
}
```

Output format:

```
MemTotal:    208 kB
MemFree:     192 kB
PageSize:   4096 B
```

**`snprintf` note:** The freestanding kernel has no libc `snprintf`.
A minimal `fmt_u32` / `fmt_str` helper set (integer-to-decimal, string
append) is sufficient — no floating point, no width specifiers needed.
These helpers live in a small `src/kernel/fmt.c` utility.

**Future expansion (Phase 3+):** `/proc/<pid>/status`,
`/proc/<pid>/maps`, `/proc/self` symlink.

### Step 10 — Boot-Time Mount Sequence and Integration Test

**Boot mount order in `kmain()` (after existing init calls):**

```c
void kmain(void) {
    /* … existing init: mm_init, proc_init, uart_init_irq, fd_stdio_init … */

    /* Phase 2: VFS and filesystem initialization */
    vfs_init();
    vfs_mount("/",     &romfs_ops,  (void *)_romfs_start);
    vfs_mount("/dev",  &devfs_ops,  NULL);
    vfs_mount("/proc", &procfs_ops, NULL);

    /* Integration test: open and read a file from romfs */
    int fd = sys_open("/etc/hostname", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[32];
        ssize_t n = sys_read(fd, buf, sizeof(buf) - 1);
        if (n > 0) { buf[n] = '\0'; uart_puts("hostname: "); uart_puts(buf); }
        sys_close(fd);
    }

    /* Test devfs */
    fd = sys_open("/dev/null", O_WRONLY, 0);
    if (fd >= 0) { sys_write(fd, "discarded", 9); sys_close(fd); }

    /* … mpu_init, sched_start … */
}
```

**Expected boot output (RP2040 and QEMU):**

```
MM: SRAM memory map
MM:   pages   0x20004000–0x20037fff 208 KB (52 × 4 KB, all free)
VFS: mounting romfs at / (image at 0x10011000, 1234 bytes)
VFS: mounting devfs at /dev (4 devices)
VFS: mounting procfs at /proc (2 entries)
hostname: ppap
```

**QEMU test script update (`scripts/qemu.sh`):**

The QEMU binary now includes the romfs image in its ROM section.  Expected
QEMU output matches the hardware output (different romfs base address).

**End-to-end integration tests (manual, via UART console or QEMU output):**

| Test | Expected Result |
|---|---|
| `open("/etc/hostname") + read` | Returns `"ppap\n"` |
| `open("/etc/motd") + read` | Returns welcome message |
| `open("/nonexistent")` | Returns `-ENOENT` |
| `open("/dev/null") + write` | Succeeds, data discarded |
| `open("/dev/zero") + read(4)` | Returns 4 zero bytes |
| `open("/proc/meminfo") + read` | Returns MemTotal/MemFree |
| `stat("/etc/hostname")` | Returns type=FILE, size=5 |
| `stat("/etc")` | Returns type=DIR |
| `getdents("/")` | Lists `etc`, `bin` |
| `getdents("/dev")` | Lists `null`, `zero`, `ttyS0`, `urandom` |
| `chdir("/etc") + getcwd` | Returns `"/etc"` |
| Symlink: `open("/bin/ls") + readlink` | Returns `"busybox"` |

---

## Deliverables

| File | Description |
|---|---|
| `src/kernel/vfs/vfs.c/h` | VFS core: mount table, vnode pool, `vfs_mount`, `vfs_init` |
| `src/kernel/vfs/namei.c` | Path resolution: `vfs_lookup`, `vfs_find_mount` |
| `src/kernel/fs/romfs_format.h` | On-flash romfs structures (shared with mkromfs) |
| `src/kernel/fs/romfs.c/h` | romfs driver: mount, lookup, read, readdir, stat, readlink |
| `src/kernel/fs/devfs.c/h` | devfs: `/dev/null`, `/dev/zero`, `/dev/ttyS0`, `/dev/urandom` |
| `src/kernel/fs/procfs.c/h` | procfs: `/proc/meminfo`, `/proc/version` |
| `src/kernel/syscall/sys_fs.c` | open, close, lseek, stat, fstat, getdents, getcwd, chdir |
| `src/kernel/syscall/syscall.c` | Updated dispatch table with new syscall entries |
| `src/kernel/fd/file.h` | Extended `struct file` with vnode pointer and offset |
| `src/kernel/fmt.c/h` | Minimal integer/string formatters for procfs output |
| `tools/mkromfs/mkromfs.c` | Host tool: directory tree → romfs binary image |
| `tools/mkromfs/Makefile` | Host build for mkromfs |
| `romfs/etc/hostname` | Test romfs content: `"ppap"` |
| `romfs/etc/motd` | Test romfs content: welcome message |
| `CMakeLists.txt` | Updated: mkromfs build + romfs image generation + linking |
| `ldscripts/ppap.ld` | Updated: `FLASH_ROMFS` region + `.romfs` section |
| `ldscripts/qemu.ld` | Updated: matching romfs region for QEMU |
| `src/kernel/main.c` | Updated: VFS init + mount sequence |
| `src/kernel/main_qemu.c` | Updated: same mount sequence for QEMU |

---

## Known Risks and Mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| romfs format bugs (offset miscalculation in mkromfs) | High | Add `mkromfs --dump` mode to print the generated image; verify offsets manually before kernel testing |
| Path resolution bugs (mount crossing, `..` at root) | High | Test namei with known paths on QEMU before hardware; add debug prints for each lookup step |
| vnode pool exhaustion (64 vnodes not enough) | Low | Phase 2 has few concurrent opens; free vnodes on close; monitor free count at boot |
| Flash image too large for `FLASH_ROMFS` region | Low | Phase 2 romfs is tiny (~1 KB); real risk starts in Phase 3 with busybox (200+ KB) |
| XIP alignment issues (unaligned file data causes HardFault) | Medium | mkromfs enforces 4-byte alignment on all data; assert in `romfs_mount` to verify |
| Symlink resolution loops | Low | Depth limit of 8; return `-ELOOP` on exceeded depth |
| QEMU romfs base address differs from hardware | Certain | Use `_romfs_start` linker symbol everywhere; never hardcode the address |
| `struct file` extension breaks Phase 1 tty code | Low | tty files have `vnode = NULL`; all VFS code checks for NULL before dereferencing |
| No `snprintf` in freestanding kernel | Medium | Hand-write minimal `fmt_u32` / `fmt_str` helpers in `fmt.c`; no libc dependency |
| `objcopy` binary-to-ELF flags vary across toolchain versions | Low | Test with project's arm-none-eabi toolchain; fallback to `.incbin` assembly if needed |

---

## References

- [RP2040 Datasheet](https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf) — §2.6 (SSI/XIP), §4.7 (ROSC random bit)
- [Linux romfs documentation](https://www.kernel.org/doc/html/latest/filesystems/romfs.html) — Inspiration for the on-flash format
- [ARMv6-M Architecture Reference Manual](https://developer.arm.com/documentation/ddi0419/) — §A3.7 (memory alignment requirements)
- [PicoPiAndPortable Design Spec v0.2](PicoPiAndPortable-spec-v02.md) — §3 (File System), §7 (Boot Sequence)
