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
#define SYS_GETPID    20
#define SYS_NANOSLEEP 162

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

long sys_exit(long status);
long sys_read(long fd, char *buf, size_t n);
long sys_write(long fd, const char *buf, size_t n);
long sys_getpid(void);
long sys_nanosleep(void *req, void *rem);

#endif /* PPAP_SYSCALL_H */
