/*
 * core_stubs.S — VFS-side caller stubs for core module functions
 *
 * These stubs live in the VFS module's code segment. When VFS code
 * calls a mod_core function (e.g. klog()), it calls these near stubs.
 * Each stub does an indirect far call to the core entry point.
 *
 * DS is shared (DS=0) — no DS switching needed.
 *
 * Linked into ppap_ibmpc_vfs (VFS binary).
 */

#ifdef __ia16__

  .code16
  .section .text

/*
 * Far pointer table for core entry points.
 * Initialized at runtime by the core module during VFS init.
 */
  .section .data
  .global core_fptrs
core_fptrs:
  .word 0, 0    /* [0]  klog_entry */
  .word 0, 0    /* [1]  klogf_entry */
  .word 0, 0    /* [2]  kmem_pool_init_entry */
  .word 0, 0    /* [3]  kmem_alloc_entry */
  .word 0, 0    /* [4]  kmem_free_entry */
  .word 0, 0    /* [5]  kmem_free_count_entry */

  .section .text

/*
 * Caller stub: lcall *fptr, ret.  DS=0 shared, no switching.
 */
.macro CORE_CALLER_STUB name, idx
  .global \name
\name:
  lcall *core_fptrs + \idx * 4
  ret
.endm

  CORE_CALLER_STUB klog,           0
  CORE_CALLER_STUB klogf,          1
  CORE_CALLER_STUB kmem_pool_init, 2
  CORE_CALLER_STUB kmem_alloc,     3
  CORE_CALLER_STUB kmem_free,      4
  CORE_CALLER_STUB kmem_free_count, 5

#endif /* __ia16__ */
