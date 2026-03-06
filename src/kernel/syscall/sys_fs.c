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
#include "../fd/tty.h"
#include "../proc/proc.h"
#include "../mm/kmem.h"
#include "../fs/devfs.h"
#include "../fs/procfs.h"
#include "../fs/tmpfs.h"
#ifdef PPAP_HAS_BLKDEV
#include "../fs/vfat.h"
#include "../blkdev/blkdev.h"
#include "../fs/ufs.h"
#endif
#include "../errno.h"
#include "config.h"
#include <stddef.h>
#include <string.h>

/* ── File object pool ──────────────────────────────────────────────────────── */

static struct file   file_storage[FILE_MAX];
static kmem_pool_t   file_pool;

void file_pool_init(void)
{
    kmem_pool_init(&file_pool, file_storage,
                   sizeof(struct file), FILE_MAX);
}

struct file *file_alloc(void)
{
    return kmem_alloc(&file_pool);
}

void file_free(struct file *f)
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

    /* O_APPEND: always write at end of file */
    if (f->flags & O_APPEND)
        f->offset = f->vnode->size;

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
    NULL,   /* ioctl — regular files don't support ioctl */
};

/* ── sys_open ──────────────────────────────────────────────────────────────── */

long sys_open(const char *path, long flags, long mode)
{
    if (!path)
        return -(long)EINVAL;

    vnode_t *vn = NULL;
    int err = vfs_lookup(path, &vn);

    /* O_CREAT: if file doesn't exist, create it via the FS driver */
    if (err == -ENOENT && ((uint32_t)flags & O_CREAT)) {
        vnode_t *parent = NULL;
        char namebuf[VFS_NAME_MAX + 1];
        err = vfs_lookup_parent(path, &parent, namebuf,
                                (int)sizeof(namebuf));
        if (err) return (long)err;

        if (parent->type != VNODE_DIR) {
            vnode_put(parent);
            return -(long)ENOTDIR;
        }

        /* Check that the FS supports create */
        if (!parent->mount || !parent->mount->ops ||
            !parent->mount->ops->create) {
            vnode_put(parent);
            return -(long)ENOSYS;
        }

        /* Check read-only mount */
        if (parent->mount->flags & MNT_RDONLY) {
            vnode_put(parent);
            return -(long)EROFS;
        }

        err = parent->mount->ops->create(parent, namebuf,
                                          (uint32_t)mode, &vn);
        vnode_put(parent);
        if (err) return (long)err;
    } else if (err) {
        return (long)err;
    }

    /* O_TRUNC: truncate existing file to zero length */
    if (((uint32_t)flags & O_TRUNC) && vn->type == VNODE_FILE) {
        if (vn->mount && vn->mount->ops && vn->mount->ops->truncate) {
            int terr = vn->mount->ops->truncate(vn, 0);
            if (terr) {
                vnode_put(vn);
                return (long)terr;
            }
        } else {
            vn->size = 0;  /* simple fallback */
        }
    }

    /* TTY device detection: redirect /dev/ttyS0, /dev/console, /dev/tty
     * opens to the tty driver so they get line discipline processing.
     * devfs vnodes have fs_priv → devfs_node_t whose first field is name. */
    if (vn->type == VNODE_DEV && vn->fs_priv) {
        const char *devname = *(const char **)vn->fs_priv;
        if (devname &&
            (strcmp(devname, "ttyS0") == 0 ||
             strcmp(devname, "tty1") == 0 ||
             strcmp(devname, "console") == 0 ||
             strcmp(devname, "tty") == 0)) {
            /* Use the tty driver file objects with line discipline */
            struct file *ttyf;
            if ((uint32_t)flags & O_WRONLY)
                ttyf = &tty_stdout;
            else
                ttyf = &tty_stdin;   /* O_RDONLY or O_RDWR → stdin ops */
            int fd = fd_alloc(current, ttyf);
            vnode_put(vn);
            if (fd < 0)
                return (long)fd;
            return (long)fd;
        }
    }

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
    f->offset = ((uint32_t)flags & O_APPEND) ? vn->size : 0;

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

/* Sentinel: directory fully read.  Prevents romfs (which uses byte-offset
 * cookies where 0 means "start from first child") from restarting when the
 * last child's next_off is 0. */
#define GETDENTS_EOF  0xFFFFFFFFu

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

    /* Already reached end of directory */
    if (f->offset == GETDENTS_EOF)
        return 0;

    /* Use the file offset as the readdir cookie */
    uint32_t cookie = f->offset;
    int n = f->vnode->mount->ops->readdir(
        f->vnode, buf, count, &cookie);
    if (n > 0)
        f->offset = (cookie == 0) ? GETDENTS_EOF : cookie;
    else if (n == 0)
        f->offset = GETDENTS_EOF;
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

/* ── sys_dup ──────────────────────────────────────────────────────────────── */

long sys_dup(long oldfd)
{
    if (oldfd < 0 || (uint32_t)oldfd >= FD_MAX)
        return -(long)EBADF;
    struct file *f = fd_get(current, (int)oldfd);
    if (!f)
        return -(long)EBADF;
    return (long)fd_alloc(current, f);
}

/* ── sys_dup2 ─────────────────────────────────────────────────────────────── */

long sys_dup2(long oldfd, long newfd)
{
    if (oldfd < 0 || (uint32_t)oldfd >= FD_MAX)
        return -(long)EBADF;
    if (newfd < 0 || (uint32_t)newfd >= FD_MAX)
        return -(long)EBADF;
    struct file *f = fd_get(current, (int)oldfd);
    if (!f)
        return -(long)EBADF;
    if (oldfd == newfd)
        return newfd;
    /* Close newfd if it is currently open */
    fd_free(current, (int)newfd);
    /* Point newfd at the same struct file, increment refcnt */
    current->fd_table[(int)newfd] = f;
    f->refcnt++;
    return newfd;
}

/* ── sys_mkdir ─────────────────────────────────────────────────────────────── */

long sys_mkdir(const char *path, long mode)
{
    if (!path)
        return -(long)EINVAL;

    vnode_t *parent = NULL;
    char namebuf[VFS_NAME_MAX + 1];
    int err = vfs_lookup_parent(path, &parent, namebuf,
                                (int)sizeof(namebuf));
    if (err) return (long)err;

    if (parent->type != VNODE_DIR) {
        vnode_put(parent);
        return -(long)ENOTDIR;
    }

    if (!parent->mount || !parent->mount->ops ||
        !parent->mount->ops->mkdir) {
        vnode_put(parent);
        return -(long)ENOSYS;
    }

    if (parent->mount->flags & MNT_RDONLY) {
        vnode_put(parent);
        return -(long)EROFS;
    }

    err = parent->mount->ops->mkdir(parent, namebuf, (uint32_t)mode);
    vnode_put(parent);
    return (long)err;
}

/* ── sys_unlink ────────────────────────────────────────────────────────────── */

long sys_unlink(const char *path)
{
    if (!path)
        return -(long)EINVAL;

    vnode_t *parent = NULL;
    char namebuf[VFS_NAME_MAX + 1];
    int err = vfs_lookup_parent(path, &parent, namebuf,
                                (int)sizeof(namebuf));
    if (err) return (long)err;

    if (parent->type != VNODE_DIR) {
        vnode_put(parent);
        return -(long)ENOTDIR;
    }

    if (!parent->mount || !parent->mount->ops ||
        !parent->mount->ops->unlink) {
        vnode_put(parent);
        return -(long)ENOSYS;
    }

    if (parent->mount->flags & MNT_RDONLY) {
        vnode_put(parent);
        return -(long)EROFS;
    }

    err = parent->mount->ops->unlink(parent, namebuf);
    vnode_put(parent);
    return (long)err;
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Phase 6 Step 7: Linux-compatible syscalls (musl libc ABI)
 * ══════════════════════════════════════════════════════════════════════════════ */

/* ── Linux ARM struct stat64 (88 bytes) ──────────────────────────────────── */
/*
 * Layout must exactly match musl's arch/arm/bits/stat.h.
 * We define it locally to avoid polluting the kernel VFS headers.
 */
struct linux_stat64 {
    uint64_t st_dev;             /* +0  */
    uint32_t __pad1;             /* +8  */
    uint32_t __st_ino_truncated; /* +12 */
    uint32_t st_mode;            /* +16 */
    uint32_t st_nlink;           /* +20 */
    uint32_t st_uid;             /* +24 */
    uint32_t st_gid;             /* +28 */
    uint64_t st_rdev;            /* +32 */
    uint32_t __pad2;             /* +40 */
    int64_t  st_size;            /* +44 */
    uint32_t st_blksize;         /* +52 */
    uint64_t st_blocks;          /* +56 */
    uint32_t st_atime;           /* +64 */
    uint32_t st_atime_nsec;      /* +68 */
    uint32_t st_mtime;           /* +72 */
    uint32_t st_mtime_nsec;      /* +76 */
    uint32_t st_ctime;           /* +80 */
    uint32_t st_ctime_nsec;      /* +84 */
    uint64_t st_ino;             /* +88 */
};
/* Note: actual size is 96 bytes including the trailing st_ino */

static void fill_stat64(const struct stat *src, struct linux_stat64 *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->st_ino = src->st_ino;
    dst->__st_ino_truncated = src->st_ino;
    dst->st_mode = src->st_mode;
    dst->st_nlink = src->st_nlink;
    dst->st_size = (int64_t)src->st_size;
    dst->st_blksize = 4096;
    dst->st_blocks = ((uint64_t)src->st_size + 511u) / 512u;
    /* uid, gid, dev, rdev, times left as zero */
}

/* ── sys_stat64 ────────────────────────────────────────────────────────────── */

long sys_stat64(const char *path, void *buf)
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

    struct stat st;
    err = vn->mount->ops->stat(vn, &st);
    vnode_put(vn);
    if (err)
        return (long)err;

    fill_stat64(&st, (struct linux_stat64 *)buf);
    return 0;
}

/* ── sys_fstat64 ───────────────────────────────────────────────────────────── */

long sys_fstat64(long fd, void *buf)
{
    if (fd < 0 || (uint32_t)fd >= FD_MAX || !buf)
        return -(long)EBADF;

    struct file *f = fd_get(current, (int)fd);
    if (!f)
        return -(long)EBADF;

    /* tty files (no vnode): synthesize a minimal char-device stat */
    if (!f->vnode) {
        struct linux_stat64 *dst = (struct linux_stat64 *)buf;
        memset(dst, 0, sizeof(*dst));
        dst->st_mode = S_IFCHR | 0666u;
        dst->st_nlink = 1;
        dst->st_blksize = 4096;
        return 0;
    }

    if (!f->vnode->mount || !f->vnode->mount->ops ||
        !f->vnode->mount->ops->stat)
        return -(long)ENOSYS;

    struct stat st;
    int err = f->vnode->mount->ops->stat(f->vnode, &st);
    if (err)
        return (long)err;

    fill_stat64(&st, (struct linux_stat64 *)buf);
    return 0;
}

/* ── sys_lstat64 ───────────────────────────────────────────────────────────── */

long sys_lstat64(const char *path, void *buf)
{
    if (!path || !buf)
        return -(long)EINVAL;

    vnode_t *vn = NULL;
    int err = vfs_lookup_flags(path, &vn, VFS_LOOKUP_NOFOLLOW);
    if (err)
        return (long)err;

    if (!vn->mount || !vn->mount->ops || !vn->mount->ops->stat) {
        vnode_put(vn);
        return -(long)ENOSYS;
    }

    struct stat st;
    err = vn->mount->ops->stat(vn, &st);
    vnode_put(vn);
    if (err)
        return (long)err;

    fill_stat64(&st, (struct linux_stat64 *)buf);
    return 0;
}

/* ── sys_getdents64 ────────────────────────────────────────────────────────── */
/*
 * Linux struct dirent64 (variable-length):
 *   uint64_t d_ino;     +0
 *   int64_t  d_off;     +8   (next cookie)
 *   uint16_t d_reclen;  +16  (total record length)
 *   uint8_t  d_type;    +18
 *   char     d_name[];  +19  (NUL-terminated, padded to 4-byte align)
 */
long sys_getdents64(long fd, void *buf, long count)
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

    /* Already reached end of directory */
    if (f->offset == GETDENTS_EOF)
        return 0;

    /* Read internal dirent entries */
    struct dirent entries[8];
    uint32_t cookie = f->offset;
    int n = f->vnode->mount->ops->readdir(f->vnode, entries, 8, &cookie);
    if (n < 0)
        return (long)n;
    if (n == 0) {
        f->offset = GETDENTS_EOF;
        return 0;
    }

    /* Pack into linux dirent64 format */
    uint8_t *out = (uint8_t *)buf;
    long total = 0;

    for (int i = 0; i < n; i++) {
        size_t name_len = strlen(entries[i].d_name);
        /* reclen = 19 (header) + name_len + 1 (NUL), aligned to 8 */
        uint16_t reclen = (uint16_t)((19 + name_len + 1 + 7) & ~7u);

        if (total + reclen > count)
            break;

        memset(out + total, 0, reclen);

        /* d_ino (uint64_t at offset 0) */
        uint64_t ino = entries[i].d_ino;
        memcpy(out + total, &ino, 8);

        /* d_off (int64_t at offset 8) — next cookie */
        int64_t d_off = (int64_t)(f->offset + (uint32_t)i + 1);
        memcpy(out + total + 8, &d_off, 8);

        /* d_reclen (uint16_t at offset 16) */
        memcpy(out + total + 16, &reclen, 2);

        /* d_type (uint8_t at offset 18) */
        out[total + 18] = entries[i].d_type;

        /* d_name (at offset 19) */
        memcpy(out + total + 19, entries[i].d_name, name_len + 1);

        total += reclen;
    }

    f->offset = (cookie == 0) ? GETDENTS_EOF : cookie;
    return total;
}

/* ── sys_llseek ────────────────────────────────────────────────────────────── */
/*
 * _llseek(fd, offset_hi, offset_lo, &result, whence)
 * result is a pointer to loff_t (int64_t).
 */
long sys_llseek(long fd, long off_hi, long off_lo, void *result, long whence)
{
    /* PPAP files are small — ignore off_hi */
    (void)off_hi;

    long pos = sys_lseek(fd, off_lo, whence);
    if (pos < 0)
        return pos;

    if (result) {
        int64_t res = (int64_t)pos;
        memcpy(result, &res, sizeof(res));
    }
    return 0;
}

/* ── sys_fcntl64 ───────────────────────────────────────────────────────────── */
/*
 * F_DUPFD(0), F_GETFD(1), F_SETFD(2), F_GETFL(3), F_SETFL(4)
 * F_DUPFD_CLOEXEC(1030)
 */
#define F_DUPFD       0
#define F_GETFD       1
#define F_SETFD       2
#define F_GETFL       3
#define F_SETFL       4
#define F_DUPFD_CLOEXEC 1030

long sys_fcntl64(long fd, long cmd, long arg)
{
    if (fd < 0 || (uint32_t)fd >= FD_MAX)
        return -(long)EBADF;

    struct file *f = fd_get(current, (int)fd);
    if (!f)
        return -(long)EBADF;

    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        /* Find lowest fd >= arg */
        for (long i = arg; i < FD_MAX; i++) {
            if (!current->fd_table[i]) {
                current->fd_table[i] = f;
                f->refcnt++;
                return i;
            }
        }
        return -(long)EMFILE;

    case F_GETFD:
        return 0;   /* no CLOEXEC tracking yet */

    case F_SETFD:
        return 0;   /* ignore CLOEXEC */

    case F_GETFL:
        return (long)f->flags;

    case F_SETFL:
        /* Only O_APPEND and O_NONBLOCK can be changed; we support O_APPEND */
        f->flags = (f->flags & O_ACCMODE) | ((uint32_t)arg & ~O_ACCMODE);
        return 0;

    default:
        return -(long)EINVAL;
    }
}

/* ── sys_access ────────────────────────────────────────────────────────────── */

long sys_access(const char *path, long mode)
{
    (void)mode;   /* always root — all access checks pass */

    if (!path)
        return -(long)EINVAL;

    vnode_t *vn = NULL;
    int err = vfs_lookup(path, &vn);
    if (err)
        return (long)err;

    vnode_put(vn);
    return 0;
}

/* ── sys_readlink ──────────────────────────────────────────────────────────── */

long sys_readlink(const char *path, char *buf, long bufsiz)
{
    if (!path || !buf || bufsiz <= 0)
        return -(long)EINVAL;

    /* Special case: /proc/self/exe — busybox needs this */
    if (strcmp(path, "/proc/self/exe") == 0) {
        const char *exe = "/bin/busybox";
        size_t len = strlen(exe);
        if ((long)len > bufsiz) len = (size_t)bufsiz;
        memcpy(buf, exe, len);
        return (long)len;
    }

    vnode_t *vn = NULL;
    int err = vfs_lookup_flags(path, &vn, VFS_LOOKUP_NOFOLLOW);
    if (err)
        return (long)err;

    if (vn->type != VNODE_SYMLINK) {
        vnode_put(vn);
        return -(long)EINVAL;
    }

    if (!vn->mount || !vn->mount->ops || !vn->mount->ops->readlink) {
        vnode_put(vn);
        return -(long)EINVAL;
    }

    long ret = vn->mount->ops->readlink(vn, buf, (size_t)bufsiz);
    vnode_put(vn);
    return ret;
}

/* ── sys_rmdir ─────────────────────────────────────────────────────────────── */

long sys_rmdir(const char *path)
{
    return sys_unlink(path);   /* VFS unlink handles directories too */
}

/* ── sys_umask ─────────────────────────────────────────────────────────────── */

long sys_umask(long mask)
{
    uint32_t old = current->umask_val;
    current->umask_val = (uint32_t)mask & 0777u;
    return (long)old;
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Phase 6 Step 15: mount / umount / statfs syscalls
 * ══════════════════════════════════════════════════════════════════════════════ */

/* ── String comparison helper (local) ─────────────────────────────────────── */

static int fs_str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (*a == '\0' && *b == '\0');
}

/* ── sys_mount ─────────────────────────────────────────────────────────────── */

long sys_mount(const char *source, const char *target,
               const char *fstype, long flags, const void *data)
{
    (void)data;

    if (!target || !fstype)
        return -(long)EINVAL;

    /* Map fstype string to vfs_ops_t pointer */
    const vfs_ops_t *ops = NULL;
    const void *dev_data = NULL;

    if (fs_str_eq(fstype, "devfs"))
        ops = &devfs_ops;
    else if (fs_str_eq(fstype, "proc") || fs_str_eq(fstype, "procfs"))
        ops = &procfs_ops;
    else if (fs_str_eq(fstype, "tmpfs"))
        ops = &tmpfs_ops;
#ifdef PPAP_HAS_BLKDEV
    else if (fs_str_eq(fstype, "vfat")) {
        ops = &vfat_ops;
        if (source) {
            /* Strip leading "/dev/" from source to get blkdev name */
            const char *devname = source;
            if (devname[0] == '/' && devname[1] == 'd' && devname[2] == 'e' &&
                devname[3] == 'v' && devname[4] == '/')
                devname += 5;
            blkdev_t *bd = blkdev_find(devname);
            if (!bd)
                return -(long)ENODEV;
            dev_data = bd;
        }
    } else if (fs_str_eq(fstype, "ufs")) {
        ops = &ufs_ops;
        if (source) {
            const char *devname = source;
            if (devname[0] == '/' && devname[1] == 'd' && devname[2] == 'e' &&
                devname[3] == 'v' && devname[4] == '/')
                devname += 5;
            blkdev_t *bd = blkdev_find(devname);
            if (!bd)
                return -(long)ENODEV;
            dev_data = bd;
        }
    }
#endif
    else {
        return -(long)ENODEV;
    }

    /* Convert Linux mount flags: MS_RDONLY → MNT_RDONLY */
    uint8_t mnt_flags = 0;
    if ((uint32_t)flags & MS_RDONLY)
        mnt_flags |= MNT_RDONLY;

    return (long)vfs_mount(target, ops, mnt_flags, dev_data);
}

/* ── sys_umount2 ──────────────────────────────────────────────────────────── */

long sys_umount2(const char *target, long flags)
{
    (void)flags;

    if (!target)
        return -(long)EINVAL;

    /* Normalize path for comparison */
    char norm[VFS_PATH_MAX];
    int nlen = vfs_path_normalize(target, norm, (int)sizeof(norm));
    if (nlen < 0)
        return (long)nlen;

    return (long)vfs_umount(norm);
}

/* ── sys_statfs64 ─────────────────────────────────────────────────────────── */

long sys_statfs64(const char *path, long sz, void *buf)
{
    (void)sz;   /* struct size — fixed layout */

    if (!path || !buf)
        return -(long)EINVAL;

    /* Find the mount that covers this path */
    const char *remainder;
    mount_entry_t *mnt = vfs_find_mount(path, &remainder);
    if (!mnt)
        return -(long)ENOENT;

    struct kernel_statfs ksf;
    __builtin_memset(&ksf, 0, sizeof(ksf));

    if (mnt->ops && mnt->ops->statfs)
        mnt->ops->statfs(mnt, &ksf);

    __builtin_memcpy(buf, &ksf, sizeof(ksf));
    return 0;
}

/* ── sys_fstatfs64 ────────────────────────────────────────────────────────── */

long sys_fstatfs64(long fd, long sz, void *buf)
{
    (void)sz;

    if (fd < 0 || (uint32_t)fd >= FD_MAX || !buf)
        return -(long)EBADF;

    struct file *f = fd_get(current, (int)fd);
    if (!f)
        return -(long)EBADF;

    mount_entry_t *mnt = NULL;

    if (f->vnode && f->vnode->mount)
        mnt = f->vnode->mount;

    struct kernel_statfs ksf;
    __builtin_memset(&ksf, 0, sizeof(ksf));

    if (mnt && mnt->ops && mnt->ops->statfs)
        mnt->ops->statfs(mnt, &ksf);

    __builtin_memcpy(buf, &ksf, sizeof(ksf));
    return 0;
}
