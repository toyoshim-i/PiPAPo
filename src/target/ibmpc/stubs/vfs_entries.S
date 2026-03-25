/*
 * vfs_entries.S — VFS-side target entry stubs
 *
 * These stubs live in the VFS module's segment. They are the targets
 * of far calls from the core module. Each entry does a near call to
 * the real VFS function (in the same segment), then lret back to core.
 *
 * DS is already set to the VFS segment by the caller-side stub.
 *
 * Linked into ppap_ibmpc_vfs (VFS binary).
 */

#ifdef __IA16__

  .code16
  .section .text

.macro VFS_TARGET_STUB name
  .global vfs_\name\()_entry
vfs_\name\()_entry:
  call vfs_\name
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

#endif /* __IA16__ */
