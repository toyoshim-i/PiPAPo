/*
 * vfs_entries.S — VFS-side target entry stubs
 *
 * These stubs live in the VFS module's code segment. They are the
 * targets of ljmp from core caller stubs.
 *
 * The caller stub converts the compiler's near call into a far frame:
 *   Stack: [args] [caller_CS 2B] [near_ret_IP 2B] ← SP
 * Then does ljmp here (no push).
 *
 * The entry pops the far frame, calls the real function (which sees
 * [args] [ret_IP] correctly), then restores the far frame and lrets.
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

  VFS_TARGET_STUB init
  VFS_TARGET_STUB mount
  VFS_TARGET_STUB umount
  VFS_TARGET_STUB lookup
  VFS_TARGET_STUB lookup_flags
  VFS_TARGET_STUB lookup_parent
  VFS_TARGET_STUB path_normalize
  VFS_TARGET_STUB find_mount
  VFS_TARGET_STUB mount_ufs
  VFS_TARGET_STUB alloc_vnode
  VFS_TARGET_STUB ref_vnode
  VFS_TARGET_STUB rel_vnode
  /* fd_stdio_init lives in fd.c, not vfs.c — alias for the stub macro */
  .global vfs_fd_stdio_init
  .set vfs_fd_stdio_init, fd_stdio_init
  VFS_TARGET_STUB fd_stdio_init
  VFS_TARGET_STUB file_read
  /* file_pool_init lives in sys_fs.c — alias */
  .global vfs_file_pool_init
  .set vfs_file_pool_init, file_pool_init
  VFS_TARGET_STUB file_pool_init
  /* tty_rx_notify lives in tty.c — alias */
  .global vfs_tty_rx_notify
  .set vfs_tty_rx_notify, tty_rx_notify
  VFS_TARGET_STUB tty_rx_notify
  VFS_TARGET_STUB fstab_automount
  VFS_TARGET_STUB fd_open
  VFS_TARGET_STUB fd_release
  VFS_TARGET_STUB fd_ref
  VFS_TARGET_STUB fd_pipe_create
  VFS_TARGET_STUB fd_stdio_desc
  VFS_TARGET_STUB fd_read
  VFS_TARGET_STUB fd_write
  VFS_TARGET_STUB fd_ioctl
  VFS_TARGET_STUB fd_poll
  VFS_TARGET_STUB fd_lseek
  VFS_TARGET_STUB fd_fstat
  VFS_TARGET_STUB fd_getdents
  VFS_TARGET_STUB fd_getdents64
  VFS_TARGET_STUB fd_fstatfs
  VFS_TARGET_STUB fd_fcntl
  VFS_TARGET_STUB fd_get_priv

#endif /* __ia16__ */
