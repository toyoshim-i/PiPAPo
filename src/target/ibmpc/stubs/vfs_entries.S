/*
 * vfs_entries.S — VFS-side target entry stubs
 *
 * These stubs live in the VFS module's code segment. They are the
 * targets of far calls from core. Each entry adjusts the stack
 * (removes the far return address), calls the real function, then
 * restores the far return and does lret.
 *
 * This is needed because the call chain is:
 *   core: call *mod_vfs.field   → pushes near IP (2B)
 *   stub: lcall *vfs_fptrs[i]  → pushes far CS:IP (4B)
 *   entry: call vfs_fn          → pushes near IP (2B)
 *
 * Without adjustment, vfs_fn sees 6 extra bytes on the stack
 * and reads wrong argument values.
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
  /* Stack: [args] [near_IP 2B] [far_IP 2B] [far_CS 2B] ← SP */
  pop  %ax
  pop  %bx
  mov  %ax, vfs_saved_ip
  mov  %bx, vfs_saved_cs
  /* Stack: [args] [near_IP 2B] ← SP — correct for function */
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
  VFS_TARGET_STUB alloc_vnode
  VFS_TARGET_STUB ref_vnode
  VFS_TARGET_STUB rel_vnode

#endif /* __ia16__ */
