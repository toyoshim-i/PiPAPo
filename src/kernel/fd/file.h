/*
 * file.h — Kernel file abstraction
 *
 * struct file and struct file_ops are the permanent VFS interface that all
 * subsequent phases build on (devfs, romfs, UFS).  In Phase 1 the only
 * driver is the UART tty (tty.c, Step 10).
 *
 * Design:
 *   struct file_ops  — driver vtable (read / write / close)
 *   struct file      — open-file instance: vtable pointer + driver state
 *
 * Separate stdin/stdout/stderr struct file objects (not a single shared one)
 * so that dup2() can later replace individual fds independently — e.g., a
 * shell pipeline that redirects stdout to a pipe while stdin stays on tty.
 */

#ifndef PPAP_FD_FILE_H
#define PPAP_FD_FILE_H

#include <stddef.h>
#include <stdint.h>

/* Open-mode flags stored in struct file.flags */
#define O_RDONLY  0u
#define O_WRONLY  1u
#define O_RDWR    2u

/* Seek origin constants (for sys_lseek) */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* Forward declarations */
struct file;
struct vnode;

/*
 * struct file_ops — driver vtable.
 *
 * All three functions take a pointer to the owning struct file so that a
 * single set of file_ops can serve multiple open instances via the priv
 * pointer (e.g., different baud rates, different device nodes).
 *
 * Return values follow the POSIX convention:
 *   read/write: number of bytes transferred, or negative errno on error
 *   close: 0 on success, negative errno on error
 */
struct file_ops {
    long (*read) (struct file *f, char *buf,       size_t n);
    long (*write)(struct file *f, const char *buf, size_t n);
    int  (*close)(struct file *f);
};

/*
 * struct file — open-file instance.
 *
 * Allocated statically for the three stdio files (tty.c).
 * refcnt tracks sharing across dup() / fork() — not used in Phase 1.
 */
struct file {
    const struct file_ops *ops;   /* driver vtable                     */
    void                  *priv;  /* driver-private state (NULL = none) */
    uint32_t               flags; /* O_RDONLY / O_WRONLY / O_RDWR       */
    uint32_t               refcnt;/* reference count (dup/fork sharing) */
    struct vnode          *vnode;  /* backing vnode (NULL for tty files) */
    uint32_t               offset;/* current file position              */
};

/*
 * Allocate / free a struct file from the kernel file pool.
 * file_pool_init() must be called from kmain() before any allocations.
 */
struct file *file_alloc(void);
void         file_free(struct file *f);

#endif /* PPAP_FD_FILE_H */
