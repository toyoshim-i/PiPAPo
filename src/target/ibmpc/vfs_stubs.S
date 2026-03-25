/*
 * vfs_stubs.S — VFS module entry stubs for i16 segment split
 *
 * Target-side stubs: receive lcall from core, do near call to
 * the real VFS function, then lret back to core.
 *
 * These are placed at the START of the VFS binary so their offsets
 * are known and stable (0x0000, 0x0004, 0x0008, ...).
 * The core's caller-side stubs lcall to these fixed offsets.
 *
 * Pattern per stub:
 *   call real_function   ; 3 bytes (near call within VFS segment)
 *   lret                 ; 1 byte (far return to core segment)
 */

  .code16
  .section .text.stubs, "ax"

  /* Stub table — must match the order in mod_vfs.h */

  .global _vfs_stub_init
_vfs_stub_init:
  call  vfs_init
  lret

  .global _vfs_stub_mount
_vfs_stub_mount:
  call  vfs_mount
  lret

  .global _vfs_stub_umount
_vfs_stub_umount:
  call  vfs_umount
  lret

  .global _vfs_stub_lookup
_vfs_stub_lookup:
  call  vfs_lookup
  lret

  .global _vfs_stub_lookup_flags
_vfs_stub_lookup_flags:
  call  vfs_lookup_flags
  lret

  .global _vfs_stub_lookup_parent
_vfs_stub_lookup_parent:
  call  vfs_lookup_parent
  lret

  .global _vfs_stub_path_normalize
_vfs_stub_path_normalize:
  call  vfs_path_normalize
  lret

  .global _vfs_stub_find_mount
_vfs_stub_find_mount:
  call  vfs_find_mount
  lret

  .global _vfs_stub_alloc_vnode
_vfs_stub_alloc_vnode:
  call  vfs_alloc_vnode
  lret

  .global _vfs_stub_ref_vnode
_vfs_stub_ref_vnode:
  call  vfs_ref_vnode
  lret

  .global _vfs_stub_rel_vnode
_vfs_stub_rel_vnode:
  call  vfs_rel_vnode
  lret
