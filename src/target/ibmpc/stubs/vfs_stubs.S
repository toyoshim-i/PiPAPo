/*
 * vfs_stubs.S — Core-side caller stubs for VFS module functions
 *
 * These stubs live in the core module's code segment. When core code
 * calls a mod_vfs function (e.g. vfs_init()), it calls these near
 * stubs. Each stub does an indirect far call (lcall *fptr) to the
 * VFS entry point, then returns.
 *
 * DS is shared (DS=0) — no DS switching needed.
 *
 * The far pointer table vfs_fptrs[] is patched at boot by
 * target_ibmpc.c after reading the VFS module header.
 *
 * Linked into ppap_ibmpc (core binary).
 */

#ifdef __ia16__

  .code16
  .section .text

/*
 * Far pointer table: offset:segment pairs for each VFS entry point.
 * Each entry is 4 bytes: [offset 2B][segment 2B].
 */
  .section .data
  .global vfs_fptrs
  .global vfs_fptrs_end
vfs_fptrs:
  .word 0, 0    /* [0]  vfs_init_entry */
  .word 0, 0    /* [1]  vfs_mount_entry */
  .word 0, 0    /* [2]  vfs_umount_entry */
  .word 0, 0    /* [3]  vfs_lookup_entry */
  .word 0, 0    /* [4]  vfs_lookup_flags_entry */
  .word 0, 0    /* [5]  vfs_lookup_parent_entry */
  .word 0, 0    /* [6]  vfs_path_normalize_entry */
  .word 0, 0    /* [7]  vfs_find_mount_entry */
  .word 0, 0    /* [8]  vfs_mount_ufs_entry */
  .word 0, 0    /* [9]  vfs_alloc_vnode_entry */
  .word 0, 0    /* [10] vfs_ref_vnode_entry */
  .word 0, 0    /* [11] vfs_rel_vnode_entry */
  .word 0, 0    /* [12] vfs_fd_stdio_init_entry */
  .word 0, 0    /* [13] vfs_file_read_entry */
vfs_fptrs_end:

  .section .text

/*
 * Caller stub: convert near call to far call.
 *
 * Compiler does `call *mod_vfs.field` which pushes a near ret IP.
 * We pop it, construct a far frame (push CS, push IP), then ljmp
 * to the VFS entry.  The entry does `call fn; lret` which returns
 * through the far frame back to the compiled call site.
 */
.macro VFS_CALLER_STUB name, idx
  .global vfs_\name
vfs_\name:
  pop  %ax                        /* near_ret_IP */
  push %cs                        /* save caller CS for lret */
  push %ax                        /* save near_ret_IP */
  ljmp *%ss:vfs_fptrs + \idx * 4   /* far jump via SS (DS is free) */
.endm

  VFS_CALLER_STUB init,           0
  VFS_CALLER_STUB mount,          1
  VFS_CALLER_STUB umount,         2
  VFS_CALLER_STUB lookup,         3
  VFS_CALLER_STUB lookup_flags,   4
  VFS_CALLER_STUB lookup_parent,  5
  VFS_CALLER_STUB path_normalize, 6
  VFS_CALLER_STUB find_mount,     7
  VFS_CALLER_STUB mount_ufs,     8
  VFS_CALLER_STUB alloc_vnode,    9
  VFS_CALLER_STUB ref_vnode,      10
  VFS_CALLER_STUB rel_vnode,      11
  VFS_CALLER_STUB fd_stdio_init,  12
  VFS_CALLER_STUB file_read,      13

#endif /* __ia16__ */
