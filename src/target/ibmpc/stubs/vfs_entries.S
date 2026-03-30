/*
 * vfs_entries.S — VFS-side target entry stubs
 *
 * These stubs live in the VFS module's code segment. They are the
 * targets of ljmp from core caller stubs.
 *
 * The caller stub converts the compiler's near call into a far frame:
 *   Stack: [args] [caller_CS 2B] [near_ret_IP 2B] <- SP
 * Then does ljmp here (no push).
 *
 * The entry pops the far frame, calls the real function (which sees
 * [args] [ret_IP] correctly), then restores the far frame and lrets.
 *
 * Function list auto-generated from mod_vfs.inc.
 *
 * Linked into ppap_ibmpc_vfs (VFS binary).
 */

#ifdef __ia16__

  .code16

  .section .bss
vfs_saved_cs: .word 0
vfs_saved_ip: .word 0

  .section .text

.macro VFS_TARGET_STUB name
  .global vfs_\name\()_entry
vfs_\name\()_entry:
  cli
  pop  %ax
  pop  %bx
  mov  %ax, %ss:vfs_saved_ip
  mov  %bx, %ss:vfs_saved_cs
  sti
  /* Restore DS to shared data segment (SS=DS=0).
   * Core code may leave DS as scratch. */
  xor  %ax, %ax
  mov  %ax, %ds
  call vfs_\name
  cli
  /* Restore DS=0 before returning to core — VFS code uses DS
   * as a scratch register, and core expects DS=0.
   * Use BX to avoid clobbering AX (return value). */
  xor  %bx, %bx
  mov  %bx, %ds
  push %ss:vfs_saved_cs
  push %ss:vfs_saved_ip
  sti
  lret
.endm

/*
 * Aliases for functions not named vfs_<name>.
 * VFS_TARGET_STUB calls vfs_<name>, but some functions live in
 * fd.c / tty.c with their original names.  .set creates the alias.
 */
  .global vfs_fd_stdio_init
  .set vfs_fd_stdio_init, fd_stdio_init

  .global vfs_fd_pool_init
  .set vfs_fd_pool_init, fd_pool_init

  .global vfs_tty_rx_notify
  .set vfs_tty_rx_notify, tty_rx_notify

/* Generate all entry stubs from the .inc file */
#define MOD_VFS_ENTRY(name, idx)  VFS_TARGET_STUB name
#include "common/mod/mod_vfs.inc"
#undef MOD_VFS_ENTRY

#endif /* __ia16__ */
