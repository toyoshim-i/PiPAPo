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
 * Entry list auto-generated from mod_vfs.inc.
 *
 * Linked FIRST into ppap_pcxt_vfs so it appears at offset 0.
 */

#ifdef __ia16__

  .code16
  .section .text.header, "ax"

  .global vfs_module_header
vfs_module_header:
  .word 0x5646                    /* magic: "VF" */
/* Get MOD_VFS_FUNC_COUNT from the .inc (discard entry macro) */
#define MOD_VFS_ENTRY(name, idx) /* nothing */
#include "common/mod/mod_vfs.inc"
#undef MOD_VFS_ENTRY
  .word MOD_VFS_FUNC_COUNT                          /* entry_count */
#define MOD_VFS_ENTRY(name, idx)  .word vfs_##name##_entry
#include "common/mod/mod_vfs.inc"
#undef MOD_VFS_ENTRY
  /* DS=0 address of core_fptrs table (for core to patch) */
  .word core_fptrs

#endif /* __ia16__ */
