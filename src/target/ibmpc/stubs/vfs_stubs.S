/*
 * vfs_stubs.S — Core-side caller stubs for VFS module functions
 *
 * These stubs live in the core module's segment. When core code calls
 * a mod_vfs function (e.g. vfs_init()), it calls these near stubs.
 *
 * Each stub saves DS, loads the VFS segment into DS, performs an
 * indirect far call (lcall *mem) to the VFS entry point, then
 * restores DS and returns.
 *
 * The VFS segment base and entry offsets are stored in vfs_fptrs[],
 * initialized by seg_init_vfs() after stage2 loads the VFS binary.
 *
 * Linked into ppap_ibmpc (core binary).
 */

#ifdef __ia16__

  .code16
  .section .text

/*
 * Far pointer table: offset:segment pairs for each VFS entry point.
 * Initialized at runtime by seg_init_vfs() in target_ibmpc.c.
 * Each entry is 4 bytes: [offset 2B][segment 2B].
 */
  .section .data
  .global vfs_fptrs
vfs_fptrs:
  .word 0, 0    /* [0]  vfs_init_entry */
  .word 0, 0    /* [1]  vfs_mount_entry */
  .word 0, 0    /* [2]  vfs_umount_entry */
  .word 0, 0    /* [3]  vfs_lookup_entry */
  .word 0, 0    /* [4]  vfs_lookup_flags_entry */
  .word 0, 0    /* [5]  vfs_lookup_parent_entry */
  .word 0, 0    /* [6]  vfs_path_normalize_entry */
  .word 0, 0    /* [7]  vfs_find_mount_entry */
  .word 0, 0    /* [8]  vfs_alloc_vnode_entry */
  .word 0, 0    /* [9]  vfs_ref_vnode_entry */
  .word 0, 0    /* [10] vfs_rel_vnode_entry */

  .section .text

/*
 * Macro: generate a caller-side stub for one VFS function.
 *
 *   1. push ds             — save core's DS
 *   2. mov ax, [fptr+2]    — load VFS segment
 *   3. mov ds, ax          — switch to VFS data segment
 *   4. lcall *[fptr]       — far call (pushes core CS:IP on stack)
 *   5. pop ds              — restore core's DS
 *   6. ret                 — return to caller
 *
 * Arguments stay on the stack. The target-side stub in VFS does
 * a near call to the real function, then lret back here.
 */
.macro VFS_CALLER_STUB name, idx
  .global vfs_\name
vfs_\name:
  push %ds
  mov  vfs_fptrs + \idx * 4 + 2, %ax
  mov  %ax, %ds
  lcall *vfs_fptrs + \idx * 4
  pop  %ds
  ret
.endm

  VFS_CALLER_STUB init,           0
  VFS_CALLER_STUB mount,          1
  VFS_CALLER_STUB umount,         2
  VFS_CALLER_STUB lookup,         3
  VFS_CALLER_STUB lookup_flags,   4
  VFS_CALLER_STUB lookup_parent,  5
  VFS_CALLER_STUB path_normalize, 6
  VFS_CALLER_STUB find_mount,     7
  VFS_CALLER_STUB alloc_vnode,    8
  VFS_CALLER_STUB ref_vnode,      9
  VFS_CALLER_STUB rel_vnode,      10

#endif /* __ia16__ */
