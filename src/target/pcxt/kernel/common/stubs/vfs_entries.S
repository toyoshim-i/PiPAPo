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
 * Linked into ppap_pcxt_vfs (VFS binary).
 */

  .code16

  .section .bss
vfs_saved_cs: .word 0
vfs_saved_ip: .word 0

  .section .text

/*
 * Entry stub ABI note:
 *   - AX is the return-value register, so post-call stub code must
 *     leave it untouched.
 *   - BX, SI, DI, and BP are callee-saved, so the stub must not use
 *     them as scratch around the call.
 *   - DX is caller-saved, so it is the right scratch register for
 *     handling the far-return frame and restoring DS=0.
 *   - DS is compiler scratch in ia16 small model; shared kernel data
 *     is addressed via SS, so the stub restores DS only for callers
 *     that expect the conventional DS=0 state on return.
 */
.macro VFS_TARGET_STUB name
  .global vfs_\name\()_entry
vfs_\name\()_entry:
  cli
  pop  %ax
  pop  %dx
  mov  %ax, %ss:vfs_saved_ip
  mov  %dx, %ss:vfs_saved_cs
  sti
  /* Restore DS to shared data segment (SS=DS=0).
   * Core code may leave DS as scratch. */
  xor  %dx, %dx
  mov  %dx, %ds
  call vfs_\name
  cli
  /* Restore DS=0 before returning to core — VFS code uses DS
   * as a scratch register, and core expects DS=0.
   * Save DX first: 32-bit return values travel in DX:AX.
   * Use CX (also caller-saved) as scratch for the DS restore. */
  push %dx
  xor  %cx, %cx
  mov  %cx, %ds
  pop  %dx
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

  /* klog — klog.c is in VFS, alias for MOD_IMPL naming */
  .global vfs_klogf
  .set vfs_klogf, klogf

/* Generate all entry stubs from the .inc file */
#define MOD_VFS_ENTRY(name, idx)  VFS_TARGET_STUB name
#include "kernel/common/mod/mod_vfs.inc"
#undef MOD_VFS_ENTRY
