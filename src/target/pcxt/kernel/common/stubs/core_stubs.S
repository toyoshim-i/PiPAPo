/*
 * core_stubs.S — VFS-side caller stubs for core module functions
 *
 * These stubs live in the VFS module's code segment. When VFS code
 * calls a mod_core function (e.g. klog()), it calls these near stubs.
 * Each stub does an indirect far call to the core entry point.
 *
 * DS is shared (DS=0) — no DS switching needed.
 *
 * Function list auto-generated from mod_core.inc.
 *
 * Linked into ppap_pcxt_vfs (VFS binary).
 */

#ifdef __ia16__

  .code16
  .section .text

/*
 * Far pointer table for core entry points.
 * Initialized at runtime by the core module during VFS init.
 * Auto-generated from mod_core.inc.
 */
  .section .data
  .global core_fptrs
core_fptrs:
#define MOD_CORE_ENTRY(name, idx)  .word 0, 0  /* idx */
#include "kernel/common/mod/mod_core.inc"
#undef MOD_CORE_ENTRY

  .section .text

/*
 * Caller stub: convert the compiler's near call into a far call.
 *
 * On entry (after compiler's `call *struct_field`):
 *   Stack: [args...] [near_ret_IP] <- SP
 *
 * We need the target function to see the SAME layout — [args] [ret_IP].
 * So: pop near_ret, push CS:near_ret (far frame), ljmp to target.
 * The entry stub just does `call fn; lret`.  Function sees correct args.
 * The lret pops CS:near_ret, returning to the compiled call site.
 */
.macro CORE_CALLER_STUB name, idx
  .global \name
\name:
  pop  %ax                      /* near_ret_IP */
  push %cs                      /* save caller CS for lret */
  push %ax                      /* save near_ret_IP */
  ljmp *%ss:core_fptrs + \idx * 4  /* far jump via SS (DS is free) */
.endm

#define MOD_CORE_ENTRY(name, idx)  CORE_CALLER_STUB name, idx
#include "kernel/common/mod/mod_core.inc"
#undef MOD_CORE_ENTRY

#endif /* __ia16__ */
