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
 * target_pcxt.c after reading the VFS module header.
 *
 * Function list auto-generated from mod_vfs.inc.
 *
 * Linked into ppap_pcxt (core binary).
 */

#ifdef __ia16__

  .code16
  .section .text

/*
 * Far pointer table: offset:segment pairs for each VFS entry point.
 * Each entry is 4 bytes: [offset 2B][segment 2B].
 * Auto-generated from mod_vfs.inc.
 */
  .section .data
  .global vfs_fptrs
  .global vfs_fptrs_end
vfs_fptrs:
#define MOD_VFS_ENTRY(name, idx)  .word 0, 0  /* idx */
#include "kernel/common/mod/mod_vfs.inc"
#undef MOD_VFS_ENTRY
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

#define MOD_VFS_ENTRY(name, idx)  VFS_CALLER_STUB name, idx
#include "kernel/common/mod/mod_vfs.inc"
#undef MOD_VFS_ENTRY

#endif /* __ia16__ */
