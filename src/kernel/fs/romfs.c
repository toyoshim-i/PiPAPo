/*
 * romfs.c — Read-only romfs filesystem driver
 *
 * Implements vfs_ops_t for mounting a flash-resident romfs image.
 * The image base address is passed as dev_data to vfs_mount():
 *
 *   vfs_mount("/", &romfs_ops, MNT_RDONLY, (const void *)romfs_base);
 *
 * All data access is direct from XIP flash — no copying to SRAM for
 * reads.  File vnodes carry xip_addr for the ELF loader (Phase 3).
 *
 * Operations:
 *   mount    — verify magic, allocate root vnode
 *   lookup   — walk sibling chain to find a child by name
 *   read     — memcpy from flash (offset-aware)
 *   readdir  — iterate sibling chain with cookie-based resume
 *   stat     — return type, size, permissions
 *   readlink — copy symlink target from flash
 */

#include "romfs.h"
#include "romfs_format.h"
#include "../vfs/vfs.h"
#include "../errno.h"
#include "config.h"
#include <stddef.h>

/* ── Flash accessor helpers ────────────────────────────────────────────────── */

/* Return a pointer to the romfs_entry_t at byte offset `off` from the
 * image base.  No bounds check — the image is assumed valid (verified
 * at flash time). */
static inline const romfs_entry_t *get_entry(const uint8_t *base,
                                              uint32_t off)
{
    return (const romfs_entry_t *)(base + off);
}

/* Return the NUL-terminated name string for an entry. */
static inline const char *get_name(const romfs_entry_t *e)
{
    return (const char *)e + ROMFS_NAME_OFF;
}

/* Return a pointer to the file/symlink data for an entry. */
static inline const uint8_t *get_data(const romfs_entry_t *e)
{
    return (const uint8_t *)e + ROMFS_DATA_OFF(e);
}

/* ── vnode_from_entry ──────────────────────────────────────────────────────── */

static vnode_t *vnode_from_entry(mount_entry_t *mnt,
                                 const romfs_entry_t *e,
                                 uint32_t off)
{
    vnode_t *vn = vnode_alloc();
    if (!vn)
        return NULL;

    vn->ino   = off;
    vn->size  = e->size;
    vn->mount = mnt;

    switch (e->type) {
    case ROMFS_TYPE_DIR:
        vn->type = VNODE_DIR;
        vn->mode = S_IFDIR | 0755u;
        break;
    case ROMFS_TYPE_SYMLINK:
        vn->type = VNODE_SYMLINK;
        vn->mode = S_IFLNK | 0777u;
        break;
    default: /* ROMFS_TYPE_FILE */
        vn->type = VNODE_FILE;
        vn->mode = S_IFREG | 0644u;
        /* XIP address: pointer to the file data in flash */
        vn->xip_addr = get_data(e);
        break;
    }

    return vn;
}

/* ── romfs_mount ───────────────────────────────────────────────────────────── */

static int romfs_mount(mount_entry_t *mnt, const void *dev_data)
{
    if (!dev_data)
        return -EINVAL;

    const romfs_super_t *sb = (const romfs_super_t *)dev_data;
    if (sb->magic != ROMFS_MAGIC)
        return -EINVAL;

    mnt->sb_priv = (void *)(uintptr_t)dev_data;

    /* Allocate root vnode */
    const uint8_t *base = (const uint8_t *)dev_data;
    const romfs_entry_t *root_e = get_entry(base, sb->root_off);
    vnode_t *root = vnode_from_entry(mnt, root_e, sb->root_off);
    if (!root)
        return -ENOMEM;

    mnt->root = root;
    return 0;
}

/* ── romfs_lookup ──────────────────────────────────────────────────────────── */

static int romfs_lookup(vnode_t *dir, const char *name, vnode_t **result)
{
    const uint8_t *base = (const uint8_t *)dir->mount->sb_priv;
    const romfs_entry_t *dir_e = get_entry(base, dir->ino);

    uint32_t child_off = dir_e->child_off;
    while (child_off) {
        const romfs_entry_t *child = get_entry(base, child_off);
        const char *child_name = get_name(child);

        /* Compare names (NUL-terminated) */
        const char *a = child_name;
        const char *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') {
            vnode_t *vn = vnode_from_entry(dir->mount, child, child_off);
            if (!vn)
                return -ENOMEM;
            *result = vn;
            return 0;
        }

        child_off = child->next_off;
    }

    return -ENOENT;
}

/* ── romfs_read ────────────────────────────────────────────────────────────── */

static long romfs_read(vnode_t *vn, void *buf, size_t n, uint32_t off)
{
    if (vn->type == VNODE_DIR)
        return -(long)EISDIR;

    if (off >= vn->size)
        return 0;

    if (off + n > vn->size)
        n = vn->size - off;

    /* Direct flash read via XIP */
    const uint8_t *base = (const uint8_t *)vn->mount->sb_priv;
    const romfs_entry_t *e = get_entry(base, vn->ino);
    const uint8_t *data = get_data(e);

    __builtin_memcpy(buf, data + off, n);
    return (long)n;
}

/* ── romfs_readdir ─────────────────────────────────────────────────────────── */

static int romfs_readdir(vnode_t *dir, struct dirent *entries,
                          size_t max_entries, uint32_t *cookie)
{
    const uint8_t *base = (const uint8_t *)dir->mount->sb_priv;
    const romfs_entry_t *dir_e = get_entry(base, dir->ino);

    /* Walk the sibling chain; *cookie is the offset of the next child
     * to return (0 on first call = start from first child). */
    uint32_t child_off = (*cookie == 0) ? dir_e->child_off : *cookie;
    int count = 0;

    while (child_off && (size_t)count < max_entries) {
        const romfs_entry_t *child = get_entry(base, child_off);
        const char *name = get_name(child);

        entries[count].d_ino = child_off;
        entries[count].d_type =
            (child->type == ROMFS_TYPE_DIR) ? DT_DIR :
            (child->type == ROMFS_TYPE_SYMLINK) ? DT_LNK : DT_REG;

        /* Copy name (truncate if needed) */
        uint32_t nlen = child->name_len;
        if (nlen > VFS_NAME_MAX)
            nlen = VFS_NAME_MAX;
        __builtin_memcpy(entries[count].d_name, name, nlen);
        entries[count].d_name[nlen] = '\0';

        child_off = child->next_off;
        count++;
    }

    /* Update cookie for resume: next child offset (0 = end of dir) */
    *cookie = child_off;
    return count;
}

/* ── romfs_stat ────────────────────────────────────────────────────────────── */

static int romfs_stat(vnode_t *vn, struct stat *st)
{
    st->st_ino   = vn->ino;
    st->st_mode  = vn->mode;
    st->st_nlink = 1;
    st->st_size  = vn->size;
    return 0;
}

/* ── romfs_readlink ────────────────────────────────────────────────────────── */

static long romfs_readlink(vnode_t *vn, char *buf, size_t bufsiz)
{
    if (vn->type != VNODE_SYMLINK)
        return -(long)EINVAL;

    const uint8_t *base = (const uint8_t *)vn->mount->sb_priv;
    const romfs_entry_t *e = get_entry(base, vn->ino);
    const uint8_t *data = get_data(e);

    size_t len = vn->size;
    if (len > bufsiz)
        len = bufsiz;

    __builtin_memcpy(buf, data, len);
    return (long)len;
}

/* ── romfs_statfs ──────────────────────────────────────────────────────────── */

static int romfs_statfs(mount_entry_t *mnt, struct kernel_statfs *buf)
{
    __builtin_memset(buf, 0, sizeof(*buf));

    const romfs_super_t *sb = (const romfs_super_t *)mnt->sb_priv;

    buf->f_type    = 0x7275u;           /* Linux ROMFS_MAGIC */
    buf->f_bsize   = PAGE_SIZE;
    buf->f_frsize  = PAGE_SIZE;
    buf->f_blocks  = (sb->size + PAGE_SIZE - 1) / PAGE_SIZE;
    buf->f_bfree   = 0;                /* read-only */
    buf->f_bavail  = 0;
    buf->f_files   = sb->file_count;
    buf->f_ffree   = 0;
    buf->f_namelen = VFS_NAME_MAX;
    buf->f_flags   = 1;                /* ST_RDONLY */
    return 0;
}

/* ── Operations table ──────────────────────────────────────────────────────── */

const vfs_ops_t romfs_ops = {
    .mount    = romfs_mount,
    .lookup   = romfs_lookup,
    .read     = romfs_read,
    .write    = NULL,          /* read-only filesystem */
    .readdir  = romfs_readdir,
    .stat     = romfs_stat,
    .readlink = romfs_readlink,
    .statfs   = romfs_statfs,
};
