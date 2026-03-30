/*
 * vfs_header.S — VFS module header (entry point table)
 *
 * This is linked at offset 0 in the VFS binary. It contains a
 * jump table of entry points that the core module reads at boot
 * to patch its far pointer table (vfs_fptrs[]).
 *
 * Layout at VFS segment offset 0:
 *   [0]  uint16_t magic (0x5646 = "VF")
 *   [2]  uint16_t entry_count
 *   [4]  uint16_t entry[0] — offset of vfs_init_entry
 *   [6]  uint16_t entry[1] — offset of vfs_mount_entry
 *   ...
 *
 * The core reads this table, verifies the magic, and patches
 * vfs_fptrs[] with (offset, vfs_segment) pairs.
 *
 * Linked FIRST into ppap_ibmpc_vfs so it appears at offset 0.
 */

#ifdef __ia16__

  .code16
  .section .text.header, "ax"

  .global vfs_module_header
vfs_module_header:
  .word 0x5646                    /* magic: "VF" */
  .word 14                        /* entry_count */
  .word vfs_init_entry
  .word vfs_mount_entry
  .word vfs_umount_entry
  .word vfs_lookup_entry
  .word vfs_lookup_flags_entry
  .word vfs_lookup_parent_entry
  .word vfs_path_normalize_entry
  .word vfs_find_mount_entry
  .word vfs_mount_ufs_entry
  .word vfs_alloc_vnode_entry
  .word vfs_ref_vnode_entry
  .word vfs_rel_vnode_entry
  .word vfs_fd_stdio_init_entry
  .word vfs_file_read_entry
  /* DS=0 address of core_fptrs table (for core to patch) */
  .word core_fptrs

#endif /* __ia16__ */
