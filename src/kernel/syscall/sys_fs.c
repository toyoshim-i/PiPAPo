/*
 * sys_fs.c — VFS-routed filesystem syscalls
 *
 *   sys_open     open a file by path via VFS lookup
 *   sys_close    close an open fd (release vnode + file object)
 *   sys_lseek    reposition file offset (SEEK_SET/CUR/END)
 *   sys_stat     stat a path (lookup + FS stat)
 *   sys_fstat    stat an open fd
 *   sys_getdents read directory entries from an open directory fd
 *   sys_getcwd   return the current working directory
 *   sys_chdir    change the current working directory
 *
 * All operations route through the VFS layer — no FS-specific code here.
 *
 * The file pool (FILE_MAX objects) is managed by the kmem slab allocator.
 * file_pool_init() must be called from kmain() before any sys_open calls.
 *
 * VFS bridge: vfs_file_ops translates struct file_ops (read/write/close)
 * into vfs_ops calls on the backing vnode, maintaining the file offset.
 * Legacy tty files (fd 0/1/2) have vnode == NULL and use tty_fops
 * directly — no change from Phase 1.
 */

#include "syscall.h"
#include "../vfs/vfs.h"
#include "../fd/fd.h"
#include "../fd/file.h"
#include "../proc/proc.h"
#include "../mm/kmem.h"
#include "../errno.h"
#include "config.h"
#include <stddef.h>

/* ── File object pool ──────────────────────────────────────────────────────── */

static struct file   file_storage[FILE_MAX];
static kmem_pool_t   file_pool;

void file_pool_init(void)
{
    kmem_pool_init(&file_pool, file_storage,
                   sizeof(struct file), FILE_MAX);
}

static struct file *file_alloc(void)
{
    return kmem_alloc(&file_pool);
}

static void file_free(struct file *f)
{
    kmem_free(&file_pool, f);
}

/* ── VFS bridge file_ops ───────────────────────────────────────────────────── */

static long vfs_file_read(struct file *f, char *buf, size_t n)
{
    if (!f->vnode || !f->vnode->mount || !f->vnode->mount->ops ||
        !f->vnode->mount->ops->read)
        return -(long)EBADF;

    long ret = f->vnode->mount->ops->read(f->vnode, buf, n, f->offset);
    if (ret > 0)
        f->offset += (uint32_t)ret;
    return ret;
}

static long vfs_file_write(struct file *f, const char *buf, size_t n)
{
    if (!f->vnode || !f->vnode->mount || !f->vnode->mount->ops ||
        !f->vnode->mount->ops->write)
        return -(long)EBADF;

    long ret = f->vnode->mount->ops->write(
        f->vnode, (const void *)buf, n, f->offset);
    if (ret > 0)
        f->offset += (uint32_t)ret;
    return ret;
}

static int vfs_file_close(struct file *f)
{
    if (f->vnode)
        vnode_put(f->vnode);
    f->vnode = NULL;
    file_free(f);
    return 0;
}

static const struct file_ops vfs_file_ops = {
    vfs_file_read,
    vfs_file_write,
    vfs_file_close,
};

/* ── sys_open ──────────────────────────────────────────────────────────────── */

long sys_open(const char *path, long flags, long mode)
{
    (void)mode;   /* permissions not enforced in Phase 2 */

    if (!path)
        return -(long)EINVAL;

    vnode_t *vn = NULL;
    int err = vfs_lookup(path, &vn);
    if (err)
        return (long)err;

    /* Allocate a struct file from the pool */
    struct file *f = file_alloc();
    if (!f) {
        vnode_put(vn);
        return -(long)ENOMEM;
    }

    f->ops    = &vfs_file_ops;
    f->priv   = NULL;
    f->flags  = (uint32_t)flags;
    f->refcnt = 0;     /* fd_alloc will increment to 1 */
    f->vnode  = vn;
    f->offset = 0;

    int fd = fd_alloc(current, f);
    if (fd < 0) {
        vnode_put(vn);
        file_free(f);
        return (long)fd;
    }

    return (long)fd;
}

/* ── sys_close ─────────────────────────────────────────────────────────────── */

long sys_close(long fd)
{
    if (fd < 0 || (uint32_t)fd >= FD_MAX)
        return -(long)EBADF;

    struct file *f = fd_get(current, (int)fd);
    if (!f)
        return -(long)EBADF;

    fd_free(current, (int)fd);
    return 0;
}

/* ── sys_lseek ─────────────────────────────────────────────────────────────── */

long sys_lseek(long fd, long off, long whence)
{
    if (fd < 0 || (uint32_t)fd >= FD_MAX)
        return -(long)EBADF;

    struct file *f = fd_get(current, (int)fd);
    if (!f)
        return -(long)EBADF;

    /* lseek doesn't apply to tty or devices without a vnode */
    if (!f->vnode)
        return -(long)ESPIPE;

    long new_off;
    switch (whence) {
    case SEEK_SET:
        new_off = off;
        break;
    case SEEK_CUR:
        new_off = (long)f->offset + off;
        break;
    case SEEK_END:
        new_off = (long)f->vnode->size + off;
        break;
    default:
        return -(long)EINVAL;
    }

    if (new_off < 0)
        return -(long)EINVAL;

    f->offset = (uint32_t)new_off;
    return new_off;
}

/* ── sys_stat ──────────────────────────────────────────────────────────────── */

long sys_stat(const char *path, struct stat *buf)
{
    if (!path || !buf)
        return -(long)EINVAL;

    vnode_t *vn = NULL;
    int err = vfs_lookup(path, &vn);
    if (err)
        return (long)err;

    if (!vn->mount || !vn->mount->ops || !vn->mount->ops->stat) {
        vnode_put(vn);
        return -(long)ENOSYS;
    }

    err = vn->mount->ops->stat(vn, buf);
    vnode_put(vn);
    return (long)err;
}

/* ── sys_fstat ─────────────────────────────────────────────────────────────── */

long sys_fstat(long fd, struct stat *buf)
{
    if (fd < 0 || (uint32_t)fd >= FD_MAX || !buf)
        return -(long)EBADF;

    struct file *f = fd_get(current, (int)fd);
    if (!f)
        return -(long)EBADF;

    if (!f->vnode || !f->vnode->mount ||
        !f->vnode->mount->ops || !f->vnode->mount->ops->stat)
        return -(long)ENOSYS;

    return (long)f->vnode->mount->ops->stat(f->vnode, buf);
}

/* ── sys_getdents ──────────────────────────────────────────────────────────── */

long sys_getdents(long fd, struct dirent *buf, size_t count)
{
    if (fd < 0 || (uint32_t)fd >= FD_MAX || !buf)
        return -(long)EBADF;

    struct file *f = fd_get(current, (int)fd);
    if (!f)
        return -(long)EBADF;

    if (!f->vnode || f->vnode->type != VNODE_DIR)
        return -(long)ENOTDIR;

    if (!f->vnode->mount || !f->vnode->mount->ops ||
        !f->vnode->mount->ops->readdir)
        return -(long)ENOSYS;

    /* Use the file offset as the readdir cookie */
    uint32_t cookie = f->offset;
    int n = f->vnode->mount->ops->readdir(
        f->vnode, buf, count, &cookie);
    if (n >= 0)
        f->offset = cookie;
    return (long)n;
}

/* ── sys_getcwd ────────────────────────────────────────────────────────────── */

long sys_getcwd(char *buf, size_t size)
{
    if (!buf || size == 0)
        return -(long)EINVAL;

    const char *cwd = current->cwd;
    size_t len = __builtin_strlen(cwd);

    /* Default to "/" if cwd is empty (e.g. pid 0 before any chdir) */
    if (len == 0) {
        if (size < 2)
            return -(long)ERANGE;
        buf[0] = '/';
        buf[1] = '\0';
        return 2;
    }

    if (len + 1 > size)
        return -(long)ERANGE;

    __builtin_memcpy(buf, cwd, len + 1);
    return (long)(len + 1);
}

/* ── sys_chdir ─────────────────────────────────────────────────────────────── */

long sys_chdir(const char *path)
{
    if (!path)
        return -(long)EINVAL;

    /* Verify the path resolves to a directory */
    vnode_t *vn = NULL;
    int err = vfs_lookup(path, &vn);
    if (err)
        return (long)err;

    if (vn->type != VNODE_DIR) {
        vnode_put(vn);
        return -(long)ENOTDIR;
    }
    vnode_put(vn);

    /* Normalize the path and store in the PCB */
    char normalized[VFS_PATH_MAX];
    int nlen = vfs_path_normalize(path, normalized, (int)sizeof(normalized));
    if (nlen < 0)
        return (long)nlen;

    if ((size_t)nlen >= sizeof(current->cwd))
        return -(long)ENAMETOOLONG;

    __builtin_memcpy(current->cwd, normalized, (size_t)nlen + 1);
    return 0;
}
