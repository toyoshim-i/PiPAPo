/*
 * syscall.h — Syscall numbers and dispatch interface
 *
 * ARM EABI Linux convention (compatible with musl libc):
 *   r7  = syscall number
 *   r0–r3 = arguments (up to 4)
 *   r0  = return value (negative errno on error)
 *   svc 0 triggers the SVC exception
 *
 * SVC_Handler (svc.S) captures r7 before the compiler can clobber it,
 * reads the stacked r0–r3 exception frame from PSP, and calls
 * syscall_dispatch().  syscall_dispatch() writes the return value back
 * into the stacked r0 so the caller sees it in r0 after exception return.
 */

#ifndef PPAP_SYSCALL_H
#define PPAP_SYSCALL_H

#include <stdint.h>
#include <stddef.h>

/* ── Syscall numbers (ARM EABI Linux-compatible) ───────────────────────────── */

#define SYS_EXIT       1
#define SYS_READ       3
#define SYS_WRITE      4
#define SYS_OPEN       5
#define SYS_CLOSE      6
#define SYS_CHDIR     12
#define SYS_LSEEK     19
#define SYS_GETPID    20
#define SYS_STAT     106
#define SYS_FSTAT    108
#define SYS_GETDENTS 141
#define SYS_NANOSLEEP 162
#define SYS_GETCWD   183

/* ── Dispatch ──────────────────────────────────────────────────────────────── */

/*
 * Called from SVC_Handler (svc.S) with:
 *   frame[0..3] = stacked r0-r3 (syscall arguments a0-a3)
 *   nr          = syscall number (captured from r7)
 *
 * Dispatches to the appropriate sys_* implementation and writes the return
 * value into frame[0] (stacked r0) so the caller sees it after SVC return.
 */
void syscall_dispatch(uint32_t *frame, uint32_t nr);

/* ── Syscall implementations ───────────────────────────────────────────────── */
/*
 * Declared here; defined in syscall.c (stubs) until Steps 9-10 wire them
 * up to the process table, fd table, and sleep timer.
 */

/* sys_proc.c */
long sys_exit(long status);
long sys_getpid(void);

/* sys_io.c */
long sys_read(long fd, char *buf, size_t n);
long sys_write(long fd, const char *buf, size_t n);

/* sys_time.c */
long sys_nanosleep(void *req, void *rem);

/* sys_fs.c — VFS-routed file system calls */
struct stat;
struct dirent;
void file_pool_init(void);
long sys_open(const char *path, long flags, long mode);
long sys_close(long fd);
long sys_lseek(long fd, long off, long whence);
long sys_stat(const char *path, struct stat *buf);
long sys_fstat(long fd, struct stat *buf);
long sys_getdents(long fd, struct dirent *buf, size_t count);
long sys_getcwd(char *buf, size_t size);
long sys_chdir(const char *path);

#endif /* PPAP_SYSCALL_H */
