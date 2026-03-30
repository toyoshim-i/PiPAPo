/*
 * hello.S — Minimal i16 user program for testing exec + syscall
 *
 * Writes "Hello from user!\n" to stdout (fd 1) via INT 30h syscall,
 * then calls exit(0).
 *
 * Syscall ABI (INT 30h):
 *   AX = syscall number
 *   BX = arg0, CX = arg1, DX = arg2, SI = arg3, DI = arg4
 *   Return value in AX
 *
 * SYS_write = 0x0004 (fd, buf, len)
 * SYS_exit  = 0x0001 (status)
 */

  .code16
  .text
  .global _start

_start:
  /* Minimal test: just exit(42) via INT 30h */
  movw  $0x0001, %ax      /* SYS_exit */
  movw  $42, %bx           /* status = 42 */
  int   $0x30
  hlt
