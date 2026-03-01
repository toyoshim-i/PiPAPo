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

#ifndef PPAP_VFS_VFS_H
#define PPAP_VFS_VFS_H

#include <stddef.h>
#include <stdint.h>
#include "config.h"

/* ── Forward declarations ─────────────────────────────────────────────────── */

typedef struct vnode     vnode_t;
typedef struct vfs_ops   vfs_ops_t;
typedef struct mount_entry mount_entry_t;

/* ── File mode constants (POSIX-compatible) ───────────────────────────────── */

#define S_IFMT   0170000u  /* type-of-file mask                           */
#define S_IFDIR  0040000u  /* directory                                    */
#define S_IFCHR  0020000u  /* character special (device)                   */
#define S_IFREG  0100000u  /* regular file                                 */
#define S_IFLNK  0120000u  /* symbolic link                                */

#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)

/* ── Directory entry type constants (Linux d_type values) ─────────────────── */

#define DT_REG   8   /* regular file   */
#define DT_DIR   4   /* directory      */
#define DT_LNK  10   /* symbolic link  */
#define DT_CHR   2   /* character dev  */

/* ── struct stat — file metadata ──────────────────────────────────────────── */

struct stat {
    uint32_t st_ino;     /* inode number (FS-specific)                    */
    uint32_t st_mode;    /* file type + permissions (S_IF* | 0755)        */
    uint32_t st_nlink;   /* number of hard links (always 1 for romfs)     */
    uint32_t st_size;    /* file size in bytes                            */
};

/* ── struct dirent — directory entry ──────────────────────────────────────── */

struct dirent {
    uint32_t d_ino;                    /* inode number                    */
    uint8_t  d_type;                   /* DT_REG / DT_DIR / DT_LNK / …  */
    char     d_name[VFS_NAME_MAX + 1]; /* NUL-terminated filename        */
};

/* ── Mount flags ──────────────────────────────────────────────────────────── */

#define MNT_RDONLY  0x01u   /* read-only mount (romfs, procfs)             */

/* ── vnode — in-memory file/directory node ─────────────────────────────────── */

typedef enum {
    VNODE_FILE,     /* regular file                                       */
    VNODE_DIR,      /* directory                                          */
    VNODE_SYMLINK,  /* symbolic link                                      */
    VNODE_DEV,      /* device file (character special)                    */
} vnode_type_t;

struct vnode {
    vnode_type_t   type;      /* file type                                 */
    uint32_t       size;      /* file size in bytes                        */
    uint32_t       mode;      /* permissions (0755 / 0644)                 */
    uint32_t       ino;       /* FS-specific inode number / offset         */
    uint32_t       refcnt;    /* open reference count (0 = free)           */
    void          *fs_priv;   /* FS-specific data pointer                  */
    mount_entry_t *mount;     /* owning mount entry                        */
    const void    *xip_addr;  /* XIP flash address for direct exec (or NULL) */
};

/* ── vfs_ops — per-FS driver operation table ──────────────────────────────── */
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
 */

struct vfs_ops {
    int  (*mount)   (mount_entry_t *mnt, const void *dev_data);
    int  (*lookup)  (vnode_t *dir, const char *name, vnode_t **result);
    long (*read)    (vnode_t *vn, void *buf, size_t n, uint32_t off);
    long (*write)   (vnode_t *vn, const void *buf, size_t n, uint32_t off);
    int  (*readdir) (vnode_t *dir, struct dirent *entries, size_t max_entries,
                     uint32_t *cookie);
    int  (*stat)    (vnode_t *vn, struct stat *st);
    long (*readlink)(vnode_t *vn, char *buf, size_t bufsiz);
};

/* ── mount_entry — one entry in the kernel mount table ────────────────────── */

struct mount_entry {
    char             path[VFS_PATH_MAX]; /* mount point (e.g., "/", "/dev")  */
    uint8_t          path_len;           /* strlen(path) — cached            */
    uint8_t          flags;              /* MNT_RDONLY, etc.                  */
    uint8_t          active;             /* 1 = in use, 0 = free slot        */
    uint8_t          _pad;
    const vfs_ops_t *ops;                /* FS driver operations             */
    vnode_t         *root;               /* root vnode of this mount         */
    void            *sb_priv;            /* superblock / FS-private data     */
};

/* ── VFS API ──────────────────────────────────────────────────────────────── */

/*
 * Initialise the VFS layer: zero the mount table, initialise the vnode pool.
 * Must be called once from kmain() after mm_init().
 */
void vfs_init(void);

/*
 * Mount a filesystem at `path`.  Calls ops->mount() to let the FS driver
 * initialise its superblock and root vnode.
 *
 * `dev_data` is passed through to the FS mount function — its meaning
 * depends on the FS type (e.g., flash base address for romfs, NULL for
 * pseudo-FS like devfs/procfs).
 *
 * Returns 0 on success, negative errno on failure (-ENOMEM if mount table
 * is full, or whatever the FS driver returns).
 */
int vfs_mount(const char *path, const vfs_ops_t *ops, uint8_t flags,
              const void *dev_data);

/*
 * Allocate a fresh vnode from the pool.
 * Returns NULL if the pool is exhausted.
 * The returned vnode has refcnt = 1.
 */
vnode_t *vnode_alloc(void);

/*
 * Increment a vnode's reference count.
 */
void vnode_ref(vnode_t *vn);

/*
 * Decrement a vnode's reference count.  When refcnt reaches 0 the vnode
 * is returned to the free pool.
 */
void vnode_put(vnode_t *vn);

/*
 * Resolve an absolute path to a vnode.
 *
 * Walks each path component through the VFS layer, crossing mount
 * boundaries and following symlinks as needed.  "." and ".." are
 * resolved lexically (no parent pointers on vnodes).
 *
 * The returned vnode has its refcnt incremented — the caller must call
 * vnode_put() when done.
 *
 * Returns 0 on success, negative errno on failure:
 *   -EINVAL         path is NULL or does not start with '/'
 *   -ENOENT         a component was not found
 *   -ENOTDIR        a non-final component is not a directory
 *   -ELOOP          too many symlink levels (> VFS_SYMLOOP_MAX)
 *   -ENAMETOOLONG   a component or the total path exceeds limits
 */
int vfs_lookup(const char *path, vnode_t **result);

/*
 * Normalize an absolute path: resolve "." and ".." lexically, collapse
 * consecutive '/' characters.  buf receives the result.
 *
 * Returns length of normalized path (≥ 1), or negative errno.
 */
int vfs_path_normalize(const char *path, char *buf, int bufsiz);

/*
 * Look up the mount entry whose path is the longest prefix of `path`.
 * Sets *remainder to point into `path` past the mount point prefix
 * (skipping any leading '/').
 *
 * Returns the mount entry, or NULL if no mount covers the path.
 */
mount_entry_t *vfs_find_mount(const char *path, const char **remainder);

/*
 * Return the number of free vnodes in the pool (for diagnostics).
 */
uint32_t vnode_free_count(void);

/*
 * Read-only access to the mount table (for procfs /proc/mounts, etc.).
 */
extern mount_entry_t vfs_mount_table[VFS_MOUNT_MAX];

#endif /* PPAP_VFS_VFS_H */
