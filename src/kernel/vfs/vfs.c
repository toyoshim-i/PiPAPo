/*
 * vfs.c — Virtual File System layer implementation
 *
 * Manages the mount table and vnode pool.  FS-specific drivers (romfs,
 * devfs, procfs) register via vfs_mount().  Path resolution (namei) and
 * VFS-routed syscalls are added in Phase 2 Steps 2-3.
 *
 * The vnode pool is a statically-allocated slab managed by the kmem
 * allocator — O(1) alloc/free with no per-vnode metadata overhead.
 *
 * The mount table is a simple fixed-size array.  vfs_find_mount() does a
 * longest-prefix match so that "/dev/ttyS0" resolves to the "/dev" mount
 * rather than "/".
 */

#include "vfs.h"
#include "../mm/kmem.h"
#include "../errno.h"
#include "drivers/uart.h"
#include <stddef.h>

/* ── Static storage ───────────────────────────────────────────────────────── */

/* Mount table — up to VFS_MOUNT_MAX entries.  Zero-initialised by BSS. */
mount_entry_t vfs_mount_table[VFS_MOUNT_MAX];

/* Vnode pool — VFS_VNODE_MAX objects managed by kmem. */
static vnode_t        vnode_storage[VFS_VNODE_MAX];
static kmem_pool_t    vnode_pool;

/* Number of active mounts (for diagnostics). */
static uint32_t mount_count;

/* ── vfs_init ─────────────────────────────────────────────────────────────── */

void vfs_init(void)
{
    /* Zero the mount table (BSS guarantees this, but be explicit) */
    for (int i = 0; i < VFS_MOUNT_MAX; i++)
        vfs_mount_table[i].active = 0;
    mount_count = 0;

    /* Initialise the vnode slab pool */
    kmem_pool_init(&vnode_pool, vnode_storage, sizeof(vnode_t), VFS_VNODE_MAX);

    uart_puts("VFS: initialised (");
    uart_print_dec(VFS_VNODE_MAX);
    uart_puts(" vnodes, ");
    uart_print_dec(VFS_MOUNT_MAX);
    uart_puts(" mount slots)\n");
}

/* ── vnode_alloc / vnode_ref / vnode_put ───────────────────────────────────── */

vnode_t *vnode_alloc(void)
{
    vnode_t *vn = kmem_alloc(&vnode_pool);
    if (!vn)
        return NULL;
    /* Zero the vnode and set initial refcnt */
    vn->type     = VNODE_FILE;
    vn->size     = 0;
    vn->mode     = 0;
    vn->ino      = 0;
    vn->refcnt   = 1;
    vn->fs_priv  = NULL;
    vn->mount    = NULL;
    vn->xip_addr = NULL;
    return vn;
}

void vnode_ref(vnode_t *vn)
{
    if (vn)
        vn->refcnt++;
}

void vnode_put(vnode_t *vn)
{
    if (!vn)
        return;
    if (vn->refcnt > 0)
        vn->refcnt--;
    if (vn->refcnt == 0)
        kmem_free(&vnode_pool, vn);
}

uint32_t vnode_free_count(void)
{
    return kmem_free_count(&vnode_pool);
}

/* ── vfs_mount ────────────────────────────────────────────────────────────── */

int vfs_mount(const char *path, const vfs_ops_t *ops, uint8_t flags,
              const void *dev_data)
{
    if (!path || !ops)
        return -EINVAL;

    /* Find a free slot */
    mount_entry_t *mnt = NULL;
    for (int i = 0; i < VFS_MOUNT_MAX; i++) {
        if (!vfs_mount_table[i].active) {
            mnt = &vfs_mount_table[i];
            break;
        }
    }
    if (!mnt)
        return -ENOMEM;

    /* Copy the mount point path */
    size_t plen = __builtin_strlen(path);
    if (plen >= VFS_PATH_MAX)
        return -ENAMETOOLONG;

    /* Strip trailing '/' except for the root mount */
    while (plen > 1 && path[plen - 1] == '/')
        plen--;

    __builtin_memcpy(mnt->path, path, plen);
    mnt->path[plen] = '\0';
    mnt->path_len = (uint8_t)plen;
    mnt->flags    = flags;
    mnt->ops      = ops;
    mnt->root     = NULL;
    mnt->sb_priv  = NULL;

    /* Let the FS driver initialise */
    int err = 0;
    if (ops->mount)
        err = ops->mount(mnt, dev_data);
    if (err) {
        mnt->active = 0;
        return err;
    }

    mnt->active = 1;
    mount_count++;

    uart_puts("VFS: mounted at ");
    uart_puts(mnt->path);
    uart_puts("\n");

    return 0;
}

/* ── vfs_umount ──────────────────────────────────────────────────────────── */

int vfs_umount(const char *path)
{
    if (!path)
        return -EINVAL;

    /* Cannot unmount root */
    if (path[0] == '/' && path[1] == '\0')
        return -EINVAL;

    /* Find the mount entry matching path exactly */
    mount_entry_t *mnt = NULL;
    for (int i = 0; i < VFS_MOUNT_MAX; i++) {
        mount_entry_t *m = &vfs_mount_table[i];
        if (!m->active)
            continue;
        if (__builtin_strcmp(m->path, path) == 0) {
            mnt = m;
            break;
        }
    }
    if (!mnt)
        return -ENOENT;

    /* Check no vnodes still reference this mount (scan vnode pool) */
    for (int i = 0; i < VFS_VNODE_MAX; i++) {
        if (vnode_storage[i].refcnt > 0 &&
            vnode_storage[i].mount == mnt &&
            &vnode_storage[i] != mnt->root)
            return -EBUSY;
    }

    /* Release root vnode and deactivate */
    if (mnt->root)
        vnode_put(mnt->root);
    mnt->root   = NULL;
    mnt->active = 0;
    mount_count--;

    uart_puts("VFS: unmounted ");
    uart_puts(path);
    uart_puts("\n");

    return 0;
}

/* ── vfs_find_mount ───────────────────────────────────────────────────────── */

mount_entry_t *vfs_find_mount(const char *path, const char **remainder)
{
    mount_entry_t *best = NULL;
    uint8_t best_len = 0;

    for (int i = 0; i < VFS_MOUNT_MAX; i++) {
        mount_entry_t *m = &vfs_mount_table[i];
        if (!m->active)
            continue;

        uint8_t mlen = m->path_len;

        /* Root mount "/" matches everything */
        if (mlen == 1 && m->path[0] == '/') {
            if (!best || mlen > best_len) {
                best = m;
                best_len = mlen;
            }
            continue;
        }

        /* Check if the mount path is a prefix of the lookup path.
         * The character after the prefix must be '/' or '\0' to avoid
         * matching "/dev" against "/device". */
        if (__builtin_strncmp(path, m->path, mlen) == 0) {
            char next = path[mlen];
            if (next == '/' || next == '\0') {
                if (mlen > best_len) {
                    best = m;
                    best_len = mlen;
                }
            }
        }
    }

    if (best && remainder) {
        const char *r = path + best_len;
        /* Skip leading '/' in the remainder */
        while (*r == '/')
            r++;
        *remainder = r;
    }

    return best;
}
