/*
 * vfs.h — Virtual File System layer
 *
 * Central abstraction that lets multiple filesystem types (romfs, devfs,
 * procfs, and later UFS/tmpfs) coexist behind a uniform interface.
 *
 * Key types:
 *   vnode_t        — in-memory representation of a file or directory
 *   vfs_ops_t      — per-FS driver operation table
 *   mount_entry_t  — one entry in the kernel mount table
 *   struct stat    — file metadata returned by stat()
 *   struct dirent  — directory entry returned by readdir/getdents
 *
 * The vnode pool is a fixed-size slab (VFS_VNODE_MAX × 32 B) backed by
 * the kmem allocator.  Vnodes are allocated on open/lookup and freed
 * when refcnt drops to zero.
 *
 * The mount table is a static array of VFS_MOUNT_MAX entries.  Mounts
 * are never removed in Phase 2 (romfs, devfs, procfs are permanent).
 */

#ifndef PPAP_KERNEL_VFS_VFS_TYPES_H
#define PPAP_KERNEL_VFS_VFS_TYPES_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/common/core/page_types.h"
#include "kernel/common/config.h"

/* ── Forward declarations ───────────────────────────────────────────────────
 */

typedef struct vnode vnode_t;
typedef struct vfs_ops vfs_ops_t;
typedef struct mount_entry mount_entry_t;

/* ── Shared ABI types (common/) ──────────────────────────────────────────────
 */

#include "common/dirent.h"
#include "common/stat.h"

/* Verify that PPAP_NAME_MAX matches VFS_NAME_MAX */
_Static_assert(
    PPAP_NAME_MAX == VFS_NAME_MAX,
    "PPAP_NAME_MAX (common/dirent.h) must match VFS_NAME_MAX (config.h)");

/* ── Mount flags ────────────────────────────────────────────────────────────
 */

#define MNT_RDONLY 0x01u /* read-only mount (romfs, procfs)             */

/* Linux mount flags used by busybox mount(2) — only MS_RDONLY is honoured */
#define MS_RDONLY 1u

/* ── struct kernel_statfs — filesystem statistics ───────────────────────────
 */
/*
 * Matches the Linux ARM statfs64 layout that musl expects from
 * SYS_statfs64 / SYS_fstatfs64.  64-bit fields for block/inode counts.
 */
struct kernel_statfs {
  uint32_t f_type;     /* filesystem magic number                       */
  uint32_t f_bsize;    /* optimal transfer block size                   */
  uint64_t f_blocks;   /* total data blocks in filesystem               */
  uint64_t f_bfree;    /* free blocks in filesystem                     */
  uint64_t f_bavail;   /* free blocks available to non-root             */
  uint64_t f_files;    /* total file nodes in filesystem                */
  uint64_t f_ffree;    /* free file nodes in filesystem                 */
  uint32_t f_fsid[2];  /* filesystem ID                                 */
  uint32_t f_namelen;  /* maximum filename length                       */
  uint32_t f_frsize;   /* fragment size (same as f_bsize for us)        */
  uint32_t f_flags;    /* mount flags (ST_RDONLY, etc.)                 */
  uint32_t f_spare[4]; /* padding to match Linux layout                 */
};

/* ── vnode — in-memory file/directory node ───────────────────────────────────
 */

typedef enum {
  VNODE_FILE,    /* regular file                                       */
  VNODE_DIR,     /* directory                                          */
  VNODE_SYMLINK, /* symbolic link                                      */
  VNODE_DEV,     /* device file (character special)                    */
} vnode_type_t;

struct vnode {
  vnode_type_t type;    /* file type                                 */
  uint32_t size;        /* file size in bytes                        */
  uint32_t mode;        /* permissions (0755 / 0644)                 */
  uint32_t ino;         /* FS-specific inode number / offset         */
  uint32_t refcnt;      /* open reference count (0 = free)           */
  void *fs_priv;        /* FS-specific data pointer                  */
  mount_entry_t *mount; /* owning mount entry                        */
  const void *xip_addr; /* XIP flash address for direct exec (or NULL) */
};

/* ── vfs_ops — per-FS driver operation table ────────────────────────────────
 */
/*
 * Each filesystem driver (romfs, devfs, procfs, …) provides a static
 * vfs_ops_t.  Functions that a particular FS does not support should be
 * set to NULL — the VFS layer checks before calling.
 *
 * Return conventions:
 *   mount:    0 on success, negative errno on failure
 *   lookup:   0 on success, -ENOENT / -ENOTDIR / … on failure
 *   read:     bytes read (≥ 0), or negative errno
 *   write:    bytes written (≥ 0), or negative errno
 *   readdir:  number of entries filled (≥ 0), or negative errno
 *   stat:     0 on success, negative errno on failure
 *   readlink: bytes written to buf (≥ 0), or negative errno
 *   create:   0 on success (sets *result), negative errno on failure
 *   mkdir:    0 on success, negative errno on failure
 *   unlink:   0 on success, negative errno on failure
 *   truncate: 0 on success, negative errno on failure
 */

struct vfs_ops {
  int (*mount)(mount_entry_t *mnt, const void *dev_data);
  int (*lookup)(vnode_t *dir, const char *name, vnode_t **result);
  long (*read)(vnode_t *vn, page_id_t page, uint16_t page_off, size_t n,
               uint32_t off);
  long (*write)(vnode_t *vn, page_id_t page, uint16_t page_off, size_t n,
                uint32_t off);
  int (*readdir)(vnode_t *dir, struct dirent *entries, size_t max_entries,
                 uint32_t *cookie);
  int (*stat)(vnode_t *vn, struct stat *st);
  long (*readlink)(vnode_t *vn, char *buf, size_t bufsiz);
  int (*create)(vnode_t *dir, const char *name, uint32_t mode,
                vnode_t **result);
  int (*mkdir)(vnode_t *dir, const char *name, uint32_t mode);
  int (*unlink)(vnode_t *dir, const char *name);
  int (*rename)(vnode_t *old_dir, const char *old_name, vnode_t *new_dir,
                const char *new_name);
  int (*truncate)(vnode_t *vn, uint32_t length);
  int (*statfs)(mount_entry_t *mnt, struct kernel_statfs *buf);
};

/* ── mount_entry — one entry in the kernel mount table ──────────────────────
 */

struct mount_entry {
  char path[VFS_PATH_MAX]; /* mount point (e.g., "/", "/dev")  */
  uint8_t path_len;        /* strlen(path) — cached            */
  uint8_t flags;           /* MNT_RDONLY, etc.                  */
  uint8_t active;          /* 1 = in use, 0 = free slot        */
  uint8_t _pad;
  const vfs_ops_t *ops; /* FS driver operations             */
  vnode_t *root;        /* root vnode of this mount         */
  void *sb_priv;        /* superblock / FS-private data     */
};

/* Lookup flags for vfs_lookup_flags() */
#define VFS_LOOKUP_NOFOLLOW 0x01

/* Read-only access to the mount table */
extern mount_entry_t vfs_mount_table[VFS_MOUNT_MAX];

#endif /* PPAP_KERNEL_VFS_VFS_TYPES_H */
