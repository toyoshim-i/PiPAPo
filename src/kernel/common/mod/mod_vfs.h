/*
 * mod_vfs.h — VFS kernel module interface
 *
 * Defines the module boundary for the Virtual File System layer.
 * This is the API surface through which the rest of the kernel
 * (syscalls, exec, main.c) accesses VFS functionality.
 *
 * The VFS layer manages mount points, path resolution, and vnode
 * lifecycle.  It does NOT implement filesystem-specific logic — that
 * lives in fs/ drivers which register via vfs_mount() and operate
 * through the vfs_ops_t vtable.
 *
 * Usage:
 *   Callers:          #include "kernel/common/mod/mod_vfs.h"
 *   Implementation:   #define MOD_IMPLEMENTATION
 *                     #include "kernel/common/mod/mod_vfs.h"
 *
 * Implementation: src/kernel/vfs/vfs.c, src/kernel/vfs/namei.c
 */

#ifndef PPAP_KERNEL_COMMON_MOD_MOD_VFS_H
#define PPAP_KERNEL_COMMON_MOD_MOD_VFS_H

/* Full VFS type definitions */
#include "kernel/common/core/page_types.h"
#include "kernel/common/vfs/vfs_types.h"

#include "kernel/common/mod/module.h"

/* VFS event IDs for mod_vfs.notify() */
#define VFS_EVENT_WILL_PLL_CHANGE  1  /* before clock_init_pll(): drain UART */
#define VFS_EVENT_PLL_CHANGED      2  /* after clock_init_pll(): reinit baud */
#define VFS_EVENT_LATE_INIT        3  /* from target_late_init(): TTY backends,
                                       * input polls, secondary logger, fbcon */

/* Forward declaration for fd_stdio_init parameter */
struct pcb;
typedef struct pcb pcb_t;

MOD_DECLARE_BEGIN(vfs)

  /* Fields are sorted alphabetically to match mod_vfs.inc indices. */

  /*
   * fd_acquire — Increment reference count on a file descriptor.
   *
   *   desc  System-wide descriptor ID (index into fd_pool).
   */
  MOD_FUNC(vfs, void, fd_acquire, int)

  /*
   * fd_fcntl — Query or set file descriptor flags.
   *
   *   desc  Descriptor ID.
   *   cmd   Command (F_GETFL=3, F_SETFL=4).
   *   arg   Flag bits for F_SETFL.
   *
   * Returns flags for F_GETFL, 0 for F_SETFL, or negative errno.
   */
  MOD_FUNC(vfs, long, fd_fcntl, int, int, long)

  /*
   * fd_fstat — Get file metadata (size, type, permissions).
   *
   *   desc  Descriptor ID.
   *   buf   Output struct stat pointer.
   *
   * Returns 0 on success, negative errno on failure.
   */
  MOD_FUNC(vfs, int, fd_fstat, int, void *)

  /*
   * fd_fstatfs — Get filesystem statistics for the mount containing a file.
   *
   *   desc  Descriptor ID.
   *   buf   Output struct kernel_statfs pointer.
   *
   * Returns 0 on success, negative errno on failure.
   */
  MOD_FUNC(vfs, int, fd_fstatfs, int, void *)

  /*
   * fd_getdents — Read directory entries (struct dirent format).
   *
   *   desc   Descriptor ID (must be a directory).
   *   buf    Output buffer.
   *   count  Buffer size in bytes.
   *
   * Returns number of entries filled (0 = EOF), or negative errno.
   */
  MOD_FUNC(vfs, long, fd_getdents, int, void *, size_t)

  /*
   * fd_getdents64 — Read directory entries (Linux dirent64 format).
   *
   *   desc   Descriptor ID (must be a directory).
   *   buf    Output buffer.
   *   count  Buffer size in bytes.
   *
   * Returns total bytes written (0 = EOF), or negative errno.
   */
  MOD_FUNC(vfs, long, fd_getdents64, int, void *, long)

  /*
   * fd_get_priv — Get the private data pointer of a file descriptor.
   *
   *   desc  Descriptor ID.
   *
   * Returns file->priv (tty_dev_t* for TTY, pipe_t* for pipes,
   * NULL for vfs files), or NULL if desc is invalid.
   */
  MOD_FUNC(vfs, void *, fd_get_priv, int)

  /*
   * fd_ioctl — Send a device-specific control command.
   *
   *   desc  Descriptor ID.
   *   cmd   Ioctl command (TCGETS, TIOCGWINSZ, etc.).
   *   arg   Command-specific argument pointer.
   *
   * Returns 0 on success, negative errno (ENOTTY if unsupported).
   */
  MOD_FUNC(vfs, int, fd_ioctl, int, uint32_t, void *)

  /*
   * fd_lseek — Change file read/write position.
   *
   *   desc    Descriptor ID.
   *   off     Offset (meaning depends on whence).
   *   whence  SEEK_SET, SEEK_CUR, or SEEK_END.
   *
   * Returns new absolute offset, or negative errno (ESPIPE for
   * TTY/pipe).
   */
  MOD_FUNC(vfs, long, fd_lseek, int, long, int)

  /*
   * fd_open — Open a file by pathname.
   *
   *   path   Absolute or relative pathname.
   *   flags  O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, O_TRUNC, O_APPEND.
   *   mode   Permission bits (used with O_CREAT).
   *
   * Returns non-negative descriptor ID, or negative errno.
   * Auto-detects TTY devices by name (/dev/ttyS0, /dev/console).
   */
  MOD_FUNC(vfs, int, fd_open, const char *, int, int)

  /*
   * fd_pipe_create — Create a unidirectional pipe.
   *
   *   rdesc  Output: read-end descriptor ID.
   *   wdesc  Output: write-end descriptor ID.
   *
   * Returns 0 on success, negative errno (ENOMEM if pool exhausted).
   * Pipe has a 512-byte ring buffer.
   */
  MOD_FUNC(vfs, int, fd_pipe_create, int *, int *)

  /*
   * fd_poll — Check if a descriptor is ready for I/O.
   *
   *   desc  Descriptor ID.
   *
   * Returns poll event mask (POLLIN, POLLOUT, etc.), or 0.
   */
  MOD_FUNC(vfs, int, fd_poll, int)

  /*
   * fd_pool_init — Initialize the system-wide fd pool and stdio.
   *
   * Pre-allocates descriptors 0-2 as TTY stdin/stdout/stderr.
   * Called once at boot before any process is forked.
   */
  MOD_FUNC(vfs, void, fd_pool_init, void)

  /*
   * fd_read — Read from a file descriptor.
   *
   *   desc  Descriptor ID.
   *   page  Destination buffer page.
   *   off   Destination buffer offset.
   *   n     Maximum bytes to read.
   *
   * Returns bytes read (0 = EOF), or negative errno.  May block
   * on TTY or pipe (uses svc_set_restart + sched_switch).
   */
  MOD_FUNC(vfs, long, fd_read, int, page_id_t, uint16_t, size_t)

  /*
   * fd_release — Decrement refcount; close when it reaches zero.
   *
   *   desc  Descriptor ID.
   *
   * Calls file->ops->close() and zeros the pool entry on last release.
   */
  MOD_FUNC(vfs, void, fd_release, int)

  /*
   * fd_stdio_desc — Get system descriptor ID for stdin/stdout/stderr.
   *
   *   which  0=stdin, 1=stdout, 2=stderr.
   *
   * Returns descriptor ID and increments its refcount.
   */
  MOD_FUNC(vfs, int, fd_stdio_desc, int)

  /*
   * fd_stdio_init — Set up fd_map[0-2] for a new process.
   *
   *   p  Target process PCB.
   *
   * Maps stdin/stdout/stderr to system descriptors and increments
   * refcounts.  Called by sys_vfork and sys_execve.
   */
  MOD_FUNC(vfs, void, fd_stdio_init, pcb_t *)

  /*
   * fd_write — Write to a file descriptor.
   *
   *   desc  Descriptor ID.
   *   page  Source buffer page.
   *   off   Source buffer offset.
   *   n     Bytes to write.
   *
   * Returns bytes written, or negative errno.  Respects O_APPEND.
   * May block on TTY or pipe.
   */
  MOD_FUNC(vfs, long, fd_write, int, page_id_t, uint16_t, size_t)

  /*
   * fstab_automount — Parse /etc/fstab and mount all entries.
   *
   * Reads fstab from romfs.  Supports devfs, procfs, tmpfs, romfs,
   * vfat, ufs.  Failures are non-fatal (next entry continues).
   */
  MOD_FUNC(vfs, void, fstab_automount, void)

  /*
   * init — Initialize the VFS subsystem.
   *
   * Sets up the vnode slab pool and mount table.  Called once from
   * kmain() before any filesystem is mounted.
   */
  MOD_FUNC(vfs, void, init, void)

  /*
   * klogf — Formatted kernel log output (printf-like).
   *
   *   fmt   Format string (%s, %u, %x, %lu, %lx, %%).
   *   ...   Arguments matching format specifiers.
   *
   * Converts '\n' to '\r\n'.  Holds SPIN_UART for atomicity.
   */
  MOD_FUNC(vfs, void, klogf, const char *, ...)

  /*
   * lookup — Resolve a pathname to a vnode.
   *
   *   path    Absolute or relative pathname.
   *   result  Output: vnode with refcnt incremented.
   *
   * Returns 0 on success, negative errno (ENOENT, ENOTDIR).
   * Follows symlinks.  Wrapper for lookup_flags(path, result, 0).
   */
  MOD_FUNC(vfs, int, lookup, const char *, vnode_t **)

  /*
   * lookup_flags — Resolve pathname with control flags.
   *
   *   path    Pathname.
   *   result  Output: vnode with refcnt incremented.
   *   flags   VFS_LOOKUP_NOFOLLOW to prevent symlink resolution.
   *
   * Returns 0 on success, negative errno on failure.
   */
  MOD_FUNC(vfs, int, lookup_flags, const char *, vnode_t **, int)

  /*
   * lookup_parent — Resolve to parent directory and extract final name.
   *
   *   path          Pathname (must not be "/").
   *   parent        Output: parent directory vnode (refcnt incremented).
   *   namebuf       Output: final path component (null-terminated).
   *   namebuf_size  Size of namebuf.
   *
   * Returns 0 on success, negative errno (EINVAL for "/").
   * Used by O_CREAT, unlink, rmdir to split dir from filename.
   */
  MOD_FUNC(vfs, int, lookup_parent, const char *, vnode_t **,
                                     char *, int)

  /*
   * mount — Register a filesystem at a mount point.
   *
   *   path      Mount point (e.g., "/dev").
   *   ops       Filesystem operations vtable.
   *   flags     MNT_RDONLY, etc.
   *   dev_data  FS-specific data (blkdev_t* for block FS, NULL otherwise).
   *
   * Returns 0 on success, negative errno (EINVAL, ENOMEM).
   */
  MOD_FUNC(vfs, int, mount, const char *, const vfs_ops_t *,
                             uint8_t, const void *)

  /*
   * mount_by_fstype — Mount by filesystem type name string.
   *
   *   source  Device path (e.g., "/dev/sda1").
   *   target  Mount point path.
   *   fstype  Type string ("vfat", "ufs", "devfs", "procfs", "tmpfs").
   *   flags   MS_RDONLY, etc.
   *
   * Returns 0 on success, negative errno on failure.
   */
  MOD_FUNC(vfs, int, mount_by_fstype, const char *, const char *,
                                       const char *, long)

  /*
   * mount_find — Find the mount enclosing a path (longest prefix match).
   *
   *   path       Pathname to locate.
   *   remainder  Output: path portion after mount point.
   *
   * Returns mount_entry_t pointer, or NULL (should not happen if
   * root "/" is mounted).
   */
  MOD_FUNC(vfs, mount_entry_t *, mount_find, const char *, const char **)

  /* mount_romfs — Convenience mount for romfs. */
  MOD_FUNC(vfs, int, mount_romfs, const char *, uint8_t, const void *)

  /* mount_ufs — Convenience mount for UFS (Unix File System). */
  MOD_FUNC(vfs, int, mount_ufs, const char *, uint8_t, const void *)

  /*
   * notify -- Deliver a boot-timing event to VFS-side drivers.
   *
   *   event  VFS_EVENT_WILL_PLL_CHANGE, VFS_EVENT_PLL_CHANGED,
   *          VFS_EVENT_LATE_INIT, etc.
   *
   * Target-provided weak override handles UART drain/reinit, TTY
   * backend registration, secondary loggers, and input poll setup.
   */
  MOD_FUNC(vfs, void, notify, int)

  /*
   * path_normalize — Normalize a path (resolve ".", "..", collapse "//").
   *
   *   path    Input pathname (must be absolute).
   *   buf     Output buffer.
   *   bufsiz  Buffer size.
   *
   * Returns length of normalized path, or negative errno.
   * Lexical only — no filesystem traversal or symlink resolution.
   */
  MOD_FUNC(vfs, int, path_normalize, const char *, char *, int)

  /*
   * tty_rx_notify — Wake processes blocked on TTY read.
   *
   *   idx  TTY index (0=serial, 1=display).
   *
   * Called by UART ISR when a byte is received.
   */
  MOD_FUNC(vfs, void, tty_rx_notify, int)

  /*
   * umount — Unmount a filesystem.
   *
   *   path  Mount point (exact match).
   *
   * Returns 0 on success, negative errno (EBUSY if vnodes
   * reference this mount, EINVAL for root "/").
   */
  MOD_FUNC(vfs, int, umount, const char *)

  /*
   * vnode_acquire — Increment vnode reference count.
   *
   *   vn  Vnode to acquire (NULL is a no-op).
   */
  MOD_FUNC(vfs, void, vnode_acquire, vnode_t *)

  /*
   * vnode_alloc — Allocate a fresh vnode from the slab pool.
   *
   * Returns vnode with refcnt=1, or NULL if pool exhausted.
   */
  MOD_FUNC(vfs, vnode_t *, vnode_alloc, void)

  /*
   * vnode_read — Read data from a vnode at a specified offset.
   *
   *   vn        Vnode to read from.
   *   page      Destination buffer page.
   *   page_off  Destination buffer offset.
   *   size      Bytes to read.
   *   off       Byte offset within file.
   *
   * Returns bytes read, or negative errno.  Executes in VFS code
   * segment to maintain module isolation.
   */
  MOD_FUNC(vfs, long, vnode_read, vnode_t *, page_id_t, uint16_t, uint32_t,
           uint32_t)

  /*
   * vnode_readlink — Read symlink target data from a vnode.
   *
   *   vn      Symlink vnode.
   *   buf     Output buffer in caller memory.
   *   bufsiz  Maximum bytes to write.
   *
   * Returns bytes written, or negative errno.  Executes in VFS code
   * segment to maintain module isolation.
   */
  MOD_FUNC(vfs, long, vnode_readlink, vnode_t *, char *, size_t)

  /*
   * vnode_release — Decrement refcnt; free vnode when it reaches zero.
   *
   *   vn  Vnode to release (NULL is a no-op).
   */
  MOD_FUNC(vfs, void, vnode_release, vnode_t *)

  /*
   * vnode_stat — Fetch metadata for a vnode.
   *
   *   vn  Vnode to inspect.
   *   st  Output stat structure.
   *
   * Returns 0 on success, negative errno on failure.  Executes in VFS
   * code segment to maintain module isolation.
   */
  MOD_FUNC(vfs, int, vnode_stat, vnode_t *, void *)

MOD_DECLARE_END(vfs)

/* MOD_VFS_FUNC_COUNT is defined in mod_vfs.inc — the single source
 * of truth shared by both C and assembly stubs.  The _Static_assert
 * below catches mismatches between this struct and the .inc file.
 * To add a new function: update mod_vfs.h (types) AND mod_vfs.inc
 * (name + index).  Assembly stubs auto-generate from the .inc file. */
#define MOD_VFS_ENTRY(name, idx) /* count only */
#include "kernel/common/mod/mod_vfs.inc"
#undef MOD_VFS_ENTRY
_Static_assert(sizeof(mod_vfs_t) == MOD_VFS_FUNC_COUNT * sizeof(void (*)(void)),
               "mod_vfs_t size mismatch — update MOD_VFS_FUNC_COUNT in "
               "mod_vfs.inc");

#endif /* PPAP_KERNEL_COMMON_MOD_MOD_VFS_H */
