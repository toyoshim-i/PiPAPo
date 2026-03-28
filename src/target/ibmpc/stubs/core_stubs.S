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
  .word 0, 0    /* [6]  mem_region_alloc_entry */
  .word 0, 0    /* [7]  mem_region_free_entry */
  .word 0, 0    /* [8]  mem_region_total_bytes_entry */
  .word 0, 0    /* [9]  mem_region_free_bytes_entry */
  .word 0, 0    /* [10] core_blkdev_read_entry */
  .word 0, 0    /* [11] core_blkdev_write_entry */

  .section .text

/*
 * Caller stub: convert the compiler's near call into a far call.
 *
 * On entry (after compiler's `call *struct_field`):
 *   Stack: [args...] [near_ret_IP] ← SP
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

  CORE_CALLER_STUB klog,                0
  CORE_CALLER_STUB klogf,               1
  CORE_CALLER_STUB kmem_pool_init,      2
  CORE_CALLER_STUB kmem_alloc,          3
  CORE_CALLER_STUB kmem_free,           4
  CORE_CALLER_STUB kmem_free_count,     5
  CORE_CALLER_STUB mem_region_alloc,    6
  CORE_CALLER_STUB mem_region_free,     7
  CORE_CALLER_STUB mem_region_total_bytes, 8
  CORE_CALLER_STUB mem_region_free_bytes,  9
  CORE_CALLER_STUB core_blkdev_read,   10
  CORE_CALLER_STUB core_blkdev_write,  11

#endif /* __ia16__ */
