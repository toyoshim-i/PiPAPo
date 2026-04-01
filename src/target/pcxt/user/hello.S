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
 *
 * DS = CS = process segment (set by elf16_loader initial frame).
 * The ELF is linked at vaddr 0, so `msg` is a DS-relative offset.
 */

  .code16
  .text
  .global _start

_start:
  /* write(1, msg, 17) */
  movw  $0x0004, %ax      /* SYS_write */
  movw  $1, %bx            /* fd = stdout */
  movw  $msg, %cx          /* buf = msg (DS-relative) */
  movw  $17, %dx           /* len */
  int   $0x30

  /* exit(0) */
  movw  $0x0001, %ax      /* SYS_exit */
  movw  $0, %bx            /* status = 0 */
  int   $0x30
  hlt

  .section .rodata
msg:
  .ascii "Hello from user!\n"
