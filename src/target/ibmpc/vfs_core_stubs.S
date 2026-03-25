/*
 * vfs_core_stubs.S — Core module caller stubs inside VFS binary
 *
 * When VFS code calls mod_core.klogf(), it calls a near stub here
 * that does lcall to the core segment's target stub, which calls
 * the real core function.
 *
 * CORE_SEG = 0 (core is at segment 0, linear 0x0600).
 * Core target stubs are at fixed offsets in the core binary.
 */

  .code16
  .text

#define CORE_SEG 0x0000

/* Offsets of core target stubs — must match core_stubs.S */
#define CORE_STUB_KLOG           0x0000
#define CORE_STUB_KLOGF          0x0004
#define CORE_STUB_KMEM_POOL_INIT 0x0008
#define CORE_STUB_KMEM_ALLOC     0x000C
#define CORE_STUB_KMEM_FREE      0x0010
#define CORE_STUB_KMEM_FREE_CNT  0x0014

/* Stubs with the SAME names as the real core functions.
 * VFS .c code calls klogf(), kmem_alloc() etc. — the linker
 * resolves to these stubs (which do lcall to core). */

  .global klog
klog:
  lcall $CORE_SEG, $CORE_STUB_KLOG
  ret

  .global klogf
klogf:
  lcall $CORE_SEG, $CORE_STUB_KLOGF
  ret

  .global kmem_pool_init
kmem_pool_init:
  lcall $CORE_SEG, $CORE_STUB_KMEM_POOL_INIT
  ret

  .global kmem_alloc
kmem_alloc:
  lcall $CORE_SEG, $CORE_STUB_KMEM_ALLOC
  ret

  .global kmem_free
kmem_free:
  lcall $CORE_SEG, $CORE_STUB_KMEM_FREE
  ret

  .global kmem_free_count
kmem_free_count:
  lcall $CORE_SEG, $CORE_STUB_KMEM_FREE_CNT
  ret
