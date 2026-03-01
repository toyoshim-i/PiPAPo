/*
 * procfs.c — Process information pseudo-filesystem driver
 *
 * Implements vfs_ops_t for a RAM-resident pseudo-filesystem mounted at /proc.
 * All file content is generated dynamically on each read — there is no
 * cached state.
 *
 * Phase 2 entries:
 *   /proc/meminfo  — free pages, total pages, page size
 *   /proc/version  — kernel version string
 *
 * Future: /proc/<pid>/status, /proc/<pid>/maps, /proc/self symlink
 */

#include "procfs.h"
#include "../vfs/vfs.h"
#include "../mm/page.h"
#include "../errno.h"
#include "config.h"
#include <stddef.h>
#include <stdint.h>

/* ── Minimal integer-to-string formatter ──────────────────────────────────── */

/* Write unsigned decimal to buf, return number of chars written.
 * buf must have room for at least 10 digits + NUL. */
static int fmt_u32(char *buf, uint32_t v)
{
    char tmp[12];
    int len = 0;

    if (v == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }

    while (v > 0) {
        tmp[len++] = (char)('0' + (v % 10));
        v /= 10;
    }

    /* Reverse into buf */
    for (int i = 0; i < len; i++)
        buf[i] = tmp[len - 1 - i];
    buf[len] = '\0';
    return len;
}

/* Append src to dst, return new position in dst */
static int fmt_append(char *dst, int pos, int max, const char *src)
{
    while (*src && pos < max - 1)
        dst[pos++] = *src++;
    dst[pos] = '\0';
    return pos;
}

static int fmt_append_u32(char *dst, int pos, int max, uint32_t v)
{
    char tmp[12];
    fmt_u32(tmp, v);
    return fmt_append(dst, pos, max, tmp);
}

/* ── procfs node descriptor ───────────────────────────────────────────────── */

typedef struct {
    const char *name;
    int (*generate)(char *buf, int bufsiz);  /* fill buf, return length */
} procfs_node_t;

/* ── /proc/meminfo ────────────────────────────────────────────────────────── */

static int gen_meminfo(char *buf, int bufsiz)
{
    uint32_t free_pages = page_free_count();
    uint32_t total_kb = (PAGE_COUNT * PAGE_SIZE) / 1024u;
    uint32_t free_kb  = (free_pages * PAGE_SIZE) / 1024u;

    int pos = 0;
    pos = fmt_append(buf, pos, bufsiz, "MemTotal:    ");
    pos = fmt_append_u32(buf, pos, bufsiz, total_kb);
    pos = fmt_append(buf, pos, bufsiz, " kB\nMemFree:     ");
    pos = fmt_append_u32(buf, pos, bufsiz, free_kb);
    pos = fmt_append(buf, pos, bufsiz, " kB\nPageSize:   ");
    pos = fmt_append_u32(buf, pos, bufsiz, PAGE_SIZE);
    pos = fmt_append(buf, pos, bufsiz, " B\n");
    return pos;
}

/* ── /proc/version ────────────────────────────────────────────────────────── */

static const char version_str[] =
    "PicoPiAndPortable v0.3 (armv6m)\n";

static int gen_version(char *buf, int bufsiz)
{
    int len = 0;
    while (version_str[len]) len++;
    if (len > bufsiz - 1)
        len = bufsiz - 1;
    __builtin_memcpy(buf, version_str, (uint32_t)len);
    buf[len] = '\0';
    return len;
}

/* ── Node table ───────────────────────────────────────────────────────────── */

static const procfs_node_t procfs_nodes[] = {
    { "meminfo", gen_meminfo },
    { "version", gen_version },
};

#define PROCFS_NODE_COUNT \
    ((uint32_t)(sizeof(procfs_nodes) / sizeof(procfs_nodes[0])))

/* ── String helpers ───────────────────────────────────────────────────────── */

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (*a == '\0' && *b == '\0');
}

static uint32_t str_len(const char *s)
{
    uint32_t len = 0;
    while (s[len]) len++;
    return len;
}

/* ── procfs_mount ─────────────────────────────────────────────────────────── */

static int procfs_mount(mount_entry_t *mnt, const void *dev_data)
{
    (void)dev_data;

    vnode_t *root = vnode_alloc();
    if (!root)
        return -ENOMEM;

    root->type  = VNODE_DIR;
    root->mode  = S_IFDIR | 0555u;
    root->ino   = 0;
    root->size  = PROCFS_NODE_COUNT;
    root->mount = mnt;

    mnt->root = root;
    return 0;
}

/* ── procfs_lookup ────────────────────────────────────────────────────────── */

static int procfs_lookup(vnode_t *dir, const char *name, vnode_t **result)
{
    (void)dir;

    for (uint32_t i = 0; i < PROCFS_NODE_COUNT; i++) {
        if (str_eq(procfs_nodes[i].name, name)) {
            vnode_t *vn = vnode_alloc();
            if (!vn)
                return -ENOMEM;

            vn->type    = VNODE_FILE;
            vn->mode    = S_IFREG | 0444u;
            vn->ino     = i + 1;
            vn->size    = 0;   /* dynamic content — size is unknown until read */
            vn->mount   = dir->mount;
            vn->fs_priv = (void *)&procfs_nodes[i];

            *result = vn;
            return 0;
        }
    }

    return -ENOENT;
}

/* ── procfs_read ──────────────────────────────────────────────────────────── */

static long procfs_read(vnode_t *vn, void *buf, size_t n, uint32_t off)
{
    if (vn->type == VNODE_DIR)
        return -(long)EISDIR;

    const procfs_node_t *node = (const procfs_node_t *)vn->fs_priv;
    if (!node || !node->generate)
        return -(long)EIO;

    /* Generate full content into a stack buffer, then slice by offset */
    char tmp[256];
    int total = node->generate(tmp, (int)sizeof(tmp));
    if (total < 0)
        return -(long)EIO;

    if (off >= (uint32_t)total)
        return 0;

    uint32_t avail = (uint32_t)total - off;
    if (n > avail)
        n = avail;

    __builtin_memcpy(buf, tmp + off, n);
    return (long)n;
}

/* ── procfs_readdir ───────────────────────────────────────────────────────── */

static int procfs_readdir(vnode_t *dir, struct dirent *entries,
                           size_t max_entries, uint32_t *cookie)
{
    (void)dir;

    uint32_t idx = *cookie;
    int count = 0;

    while (idx < PROCFS_NODE_COUNT && (size_t)count < max_entries) {
        const procfs_node_t *node = &procfs_nodes[idx];

        entries[count].d_ino  = idx + 1;
        entries[count].d_type = DT_REG;

        uint32_t nlen = str_len(node->name);
        if (nlen > VFS_NAME_MAX)
            nlen = VFS_NAME_MAX;
        __builtin_memcpy(entries[count].d_name, node->name, nlen);
        entries[count].d_name[nlen] = '\0';

        idx++;
        count++;
    }

    *cookie = idx;
    return count;
}

/* ── procfs_stat ──────────────────────────────────────────────────────────── */

static int procfs_stat(vnode_t *vn, struct stat *st)
{
    st->st_ino   = vn->ino;
    st->st_mode  = vn->mode;
    st->st_nlink = 1;
    st->st_size  = vn->size;
    return 0;
}

/* ── Operations table ─────────────────────────────────────────────────────── */

const vfs_ops_t procfs_ops = {
    .mount    = procfs_mount,
    .lookup   = procfs_lookup,
    .read     = procfs_read,
    .write    = NULL,    /* read-only filesystem */
    .readdir  = procfs_readdir,
    .stat     = procfs_stat,
    .readlink = NULL,    /* no symlinks in procfs */
};
