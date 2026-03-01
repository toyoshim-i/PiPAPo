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
#define SYS_WAITPID    7
#define SYS_EXECVE    11
#define SYS_CHDIR     12
#define SYS_LSEEK     19
#define SYS_GETPID    20
#define SYS_STAT     106
#define SYS_FSTAT    108
#define SYS_GETDENTS 141
#define SYS_NANOSLEEP 162
#define SYS_GETCWD   183
#define SYS_VFORK    190

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

/* Set by sys_execve after do_execve succeeds.  Checked by SVC_Handler
 * to perform a PendSV-like context restore from the new process image
 * before exception return (so r9/GOT base is correct). */
extern volatile int exec_pending;

/* Syscall restart mechanism for blocking syscalls (waitpid, read, etc.).
 *
 * Problem: sched_yield() from SVC context only pends PendSV — it cannot
 * actually context-switch because PendSV has lower priority than SVC.
 * After SVC returns, PendSV tail-chains and performs the switch.  But the
 * syscall has already returned a (wrong) value to frame[0].
 *
 * Solution: blocking syscalls set svc_restart = 1.  SVC_Handler detects
 * this, restores frame[0] from svc_saved_a0, and adjusts the stacked PC
 * back by 2 bytes (size of "svc 0").  When the process is rescheduled,
 * PendSV exception-returns to the SVC instruction, which re-executes the
 * syscall with the original arguments. */
extern volatile int      svc_restart;
extern volatile uint32_t svc_saved_a0;

/* ── Syscall implementations ───────────────────────────────────────────────── */
/*
 * Declared here; defined in syscall.c (stubs) until Steps 9-10 wire them
 * up to the process table, fd table, and sleep timer.
 */

/* sys_proc.c */
long sys_exit(long status);
long sys_getpid(void);
long sys_execve(const char *path);
long sys_vfork(uint32_t *frame);
long sys_waitpid(long pid, long status_ptr, long options);

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
