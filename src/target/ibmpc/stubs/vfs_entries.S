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
  /* Stack: [args] [far_IP 2B] [far_CS 2B] ← SP */
  pop  %ax
  pop  %bx
  mov  %ax, vfs_saved_ip
  mov  %bx, vfs_saved_cs
  /* Stack: [args] ← SP — exactly what the function expects */
  call vfs_\name
  /* Restore far return and lret */
  push vfs_saved_cs
  push vfs_saved_ip
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

#endif /* __ia16__ */
