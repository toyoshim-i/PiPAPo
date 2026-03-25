/*
 * core_entries.S — Core-side target entry stubs for core functions
 *
 * These stubs live in the core module's segment. They are the targets
 * of far calls from the VFS module (or other modules calling core).
 * Each entry does a near call to the real core function, then lret.
 *
 * DS is already set to the core segment by the caller-side stub.
 *
 * Linked into ppap_ibmpc (core binary).
 */

#ifdef __IA16__

  .code16
  .section .text

.macro CORE_TARGET_STUB name
  .global \name\()_entry
\name\()_entry:
  call \name
  lret
.endm

  CORE_TARGET_STUB klog
  CORE_TARGET_STUB klogf
  CORE_TARGET_STUB kmem_pool_init
  CORE_TARGET_STUB kmem_alloc
  CORE_TARGET_STUB kmem_free
  CORE_TARGET_STUB kmem_free_count

#endif /* __IA16__ */
