/*
 * tmpfs.c — RAM-backed temporary filesystem
 *
 * Provides a volatile filesystem at /tmp.  File data is stored in pages
 * obtained from page_alloc(); metadata lives in a static inode table.
 *
 * Design:
 *   - Flat inode table (TMPFS_INODE_MAX entries) with parent_ino linkage
 *   - Inode 0 is always the root directory
 *   - File data: one page (4 KB) per file, allocated on first write
 *   - Total data pages bounded by TMPFS_DATA_MAX / PAGE_SIZE
 *   - Lookup: linear scan for inodes matching parent_ino
 *   - No symbolic links (not needed for /tmp use cases)
 */

#include "tmpfs.h"
#include "../vfs/vfs.h"
#include "../mm/page.h"
#include "../errno.h"
#include "config.h"
#include <stddef.h>

/* ── Inode structure ──────────────────────────────────────────────────── */

typedef struct {
    char      name[TMPFS_NAME_MAX + 1]; /* filename (NUL-terminated)     */
    uint8_t   type;                   /* VNODE_FILE or VNODE_DIR         */
    uint8_t   active;                 /* 1 = in use, 0 = free            */
    uint32_t  mode;                   /* S_IFREG|0644 or S_IFDIR|0755   */
    uint32_t  size;                   /* data bytes (files only)         */
    uint32_t  parent_ino;             /* inode index of parent directory */
    uint8_t  *data;                   /* page for file data, or NULL     */
} tmpfs_inode_t;

static tmpfs_inode_t inodes[TMPFS_INODE_MAX];
static uint32_t data_pages_used;    /* pages allocated for file data   */

#define TMPFS_MAX_PAGES   (TMPFS_DATA_MAX / PAGE_SIZE)
#define TMPFS_FILE_MAX    PAGE_SIZE  /* max bytes per file (one page)   */

/* ── String helpers (no libc) ─────────────────────────────────────────── */

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (*a == '\0' && *b == '\0');
}

static void str_copy(char *dst, const char *src, uint32_t max)
{
    uint32_t i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ── Inode management ─────────────────────────────────────────────────── */

static int inode_alloc(void)
{
    /* Skip inode 0 (root) */
    for (int i = 1; i < TMPFS_INODE_MAX; i++)
        if (!inodes[i].active)
            return i;
    return -1;
}

static void inode_free(int ino)
{
    if (inodes[ino].data) {
        page_free(inodes[ino].data);
        data_pages_used--;
    }
    inodes[ino].active = 0;
    inodes[ino].data   = (uint8_t *)0;
    inodes[ino].size   = 0;
}

/* ── VFS operations ───────────────────────────────────────────────────── */

static int tmpfs_mount(mount_entry_t *mnt, const void *dev_data)
{
    (void)dev_data;

    /* Initialise all inodes */
    for (int i = 0; i < TMPFS_INODE_MAX; i++) {
        inodes[i].active = 0;
        inodes[i].data   = (uint8_t *)0;
    }
    data_pages_used = 0;

    /* Create root directory (inode 0) */
    inodes[0].active     = 1;
    inodes[0].type       = VNODE_DIR;
    inodes[0].mode       = S_IFDIR | 0755u;
    inodes[0].size       = 0;
    inodes[0].parent_ino = 0;
    inodes[0].data       = (uint8_t *)0;
    inodes[0].name[0]    = '\0';

    /* Allocate root vnode */
    vnode_t *root = vnode_alloc();
    if (!root)
        return -ENOMEM;

    root->type    = VNODE_DIR;
    root->mode    = S_IFDIR | 0755u;
    root->ino     = 0;
    root->size    = 0;
    root->mount   = mnt;
    root->fs_priv = (void *)0;

    mnt->root = root;
    return 0;
}

static int tmpfs_lookup(vnode_t *dir, const char *name, vnode_t **result)
{
    uint32_t dir_ino = dir->ino;

    for (int i = 0; i < TMPFS_INODE_MAX; i++) {
        if (!inodes[i].active)
            continue;
        if (inodes[i].parent_ino != dir_ino)
            continue;
        if (!str_eq(inodes[i].name, name))
            continue;

        /* Found — create a vnode */
        vnode_t *vn = vnode_alloc();
        if (!vn)
            return -ENOMEM;

        vn->ino     = (uint32_t)i;
        vn->type    = (vnode_type_t)inodes[i].type;
        vn->mode    = inodes[i].mode;
        vn->size    = inodes[i].size;
        vn->mount   = dir->mount;
        vn->fs_priv = (void *)0;

        *result = vn;
        return 0;
    }

    return -ENOENT;
}

static long tmpfs_read(vnode_t *vn, void *buf, size_t n, uint32_t off)
{
    if (vn->type == VNODE_DIR)
        return -(long)EISDIR;

    tmpfs_inode_t *ti = &inodes[vn->ino];

    if (off >= ti->size)
        return 0;
    if (off + n > ti->size)
        n = ti->size - off;

    if (ti->data)
        __builtin_memcpy(buf, ti->data + off, n);
    else
        __builtin_memset(buf, 0, n);

    return (long)n;
}

static long tmpfs_write(vnode_t *vn, const void *buf, size_t n, uint32_t off)
{
    if (vn->type == VNODE_DIR)
        return -(long)EISDIR;

    tmpfs_inode_t *ti = &inodes[vn->ino];

    /* Enforce per-file limit (one page) */
    if (off + n > TMPFS_FILE_MAX) {
        if (off >= TMPFS_FILE_MAX)
            return -(long)ENOSPC;
        n = TMPFS_FILE_MAX - off;
    }

    /* Allocate data page on first write */
    if (!ti->data) {
        if (data_pages_used >= TMPFS_MAX_PAGES)
            return -(long)ENOSPC;
        ti->data = (uint8_t *)page_alloc();
        if (!ti->data)
            return -(long)ENOMEM;
        __builtin_memset(ti->data, 0, PAGE_SIZE);
        data_pages_used++;
    }

    __builtin_memcpy(ti->data + off, buf, n);

    /* Extend file size if needed */
    if (off + (uint32_t)n > ti->size) {
        ti->size = off + (uint32_t)n;
        vn->size = ti->size;
    }

    return (long)n;
}

static int tmpfs_readdir(vnode_t *dir, struct dirent *entries,
                         size_t max_entries, uint32_t *cookie)
{
    uint32_t dir_ino = dir->ino;
    int count = 0;
    uint32_t skip = *cookie;
    uint32_t seen = 0;

    for (int i = 0; i < TMPFS_INODE_MAX && (size_t)count < max_entries; i++) {
        if (!inodes[i].active)
            continue;
        if (inodes[i].parent_ino != dir_ino)
            continue;
        /* Don't list root as its own child */
        if ((uint32_t)i == dir_ino)
            continue;

        if (seen < skip) {
            seen++;
            continue;
        }

        entries[count].d_ino  = (uint32_t)i;
        entries[count].d_type = (inodes[i].type == VNODE_DIR) ? DT_DIR : DT_REG;
        str_copy(entries[count].d_name, inodes[i].name, VFS_NAME_MAX + 1);
        count++;
        seen++;
    }

    *cookie = seen;
    return count;
}

static int tmpfs_stat(vnode_t *vn, struct stat *st)
{
    tmpfs_inode_t *ti = &inodes[vn->ino];

    st->st_ino   = vn->ino;
    st->st_mode  = ti->mode;
    st->st_nlink = 1;
    st->st_size  = ti->size;
    return 0;
}

static int tmpfs_create(vnode_t *dir, const char *name, uint32_t mode,
                        vnode_t **result)
{
    /* Check for duplicate */
    for (int i = 0; i < TMPFS_INODE_MAX; i++) {
        if (inodes[i].active &&
            inodes[i].parent_ino == dir->ino &&
            str_eq(inodes[i].name, name))
            return -EEXIST;
    }

    int idx = inode_alloc();
    if (idx < 0)
        return -ENOSPC;

    tmpfs_inode_t *ti = &inodes[idx];
    ti->active     = 1;
    ti->type       = VNODE_FILE;
    ti->mode       = S_IFREG | (mode & 0777u);
    ti->size       = 0;
    ti->parent_ino = dir->ino;
    ti->data       = (uint8_t *)0;
    str_copy(ti->name, name, TMPFS_NAME_MAX + 1);

    /* Return vnode for the new file */
    vnode_t *vn = vnode_alloc();
    if (!vn) {
        ti->active = 0;
        return -ENOMEM;
    }

    vn->ino     = (uint32_t)idx;
    vn->type    = VNODE_FILE;
    vn->mode    = ti->mode;
    vn->size    = 0;
    vn->mount   = dir->mount;
    vn->fs_priv = (void *)0;

    *result = vn;
    return 0;
}

static int tmpfs_mkdir(vnode_t *dir, const char *name, uint32_t mode)
{
    /* Check for duplicate */
    for (int i = 0; i < TMPFS_INODE_MAX; i++) {
        if (inodes[i].active &&
            inodes[i].parent_ino == dir->ino &&
            str_eq(inodes[i].name, name))
            return -EEXIST;
    }

    int idx = inode_alloc();
    if (idx < 0)
        return -ENOSPC;

    tmpfs_inode_t *ti = &inodes[idx];
    ti->active     = 1;
    ti->type       = VNODE_DIR;
    ti->mode       = S_IFDIR | (mode & 0777u);
    ti->size       = 0;
    ti->parent_ino = dir->ino;
    ti->data       = (uint8_t *)0;
    str_copy(ti->name, name, TMPFS_NAME_MAX + 1);

    return 0;
}

static int tmpfs_unlink(vnode_t *dir, const char *name)
{
    for (int i = 0; i < TMPFS_INODE_MAX; i++) {
        if (!inodes[i].active)
            continue;
        if (inodes[i].parent_ino != dir->ino)
            continue;
        if (!str_eq(inodes[i].name, name))
            continue;

        /* If directory, check it's empty */
        if (inodes[i].type == VNODE_DIR) {
            for (int j = 0; j < TMPFS_INODE_MAX; j++) {
                if (j == i) continue;
                if (inodes[j].active && inodes[j].parent_ino == (uint32_t)i)
                    return -ENOTEMPTY;
            }
        }

        inode_free(i);
        return 0;
    }

    return -ENOENT;
}

static int tmpfs_truncate(vnode_t *vn, uint32_t length)
{
    tmpfs_inode_t *ti = &inodes[vn->ino];

    if (ti->type == VNODE_DIR)
        return -EISDIR;

    if (length == 0 && ti->data) {
        page_free(ti->data);
        ti->data = (uint8_t *)0;
        data_pages_used--;
    }

    ti->size = length;
    vn->size = length;
    return 0;
}

/* ── Operations table ─────────────────────────────────────────────────── */

const vfs_ops_t tmpfs_ops = {
    .mount    = tmpfs_mount,
    .lookup   = tmpfs_lookup,
    .read     = tmpfs_read,
    .write    = tmpfs_write,
    .readdir  = tmpfs_readdir,
    .stat     = tmpfs_stat,
    .readlink = (void *)0,
    .create   = tmpfs_create,
    .mkdir    = tmpfs_mkdir,
    .unlink   = tmpfs_unlink,
    .truncate = tmpfs_truncate,
};
