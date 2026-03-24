/*
 * mod_core.h — Core kernel module interface
 *
 * The core module provides common services used by ALL other modules:
 * logging, slab memory allocation, and (future) page allocation.
 *
 * Inline functions (spinlock, string, arch_irq) do NOT need module
 * thunks — they compile into each module's code segment directly.
 * Only functions defined in .c files are exported here.
 *
 * Usage:
 *   #include "mod/mod_core.h"
 *   mod_core.klogf("VFS: %s\n", msg);
 *   void *p = mod_core.kmem_alloc(&pool);
 *
 * Implementation: src/kernel/klog.c, src/kernel/mm/kmem.c
 */

#ifndef PPAP_KERNEL_MOD_MOD_CORE_H
#define PPAP_KERNEL_MOD_MOD_CORE_H

#include <stdint.h>
#include <stddef.h>

/* Forward declaration — full definition in mm/kmem.h */
struct kmem_pool;
typedef struct kmem_pool kmem_pool_t;

#include "module.h"

MOD_DECLARE_BEGIN(core)

  /* ── Logging ─────────────────────────────────────────────────────────── */

  /*
   * klog — Print a string to the kernel log (UART + optional mirror).
   *
   *   msg  NUL-terminated string to print.
   *
   * No formatting — use klogf for printf-style output.
   */
  MOD_FUNC(core, void, klog, const char *)

  /*
   * klogf — Printf-style kernel log output.
   *
   *   fmt  Format string (subset of printf: %s, %u, %d, %x, %c, %%).
   *   ...  Arguments matching the format specifiers.
   *
   * Note: variadic function pointers work in C but may need special
   * handling for far-call thunks on i16 when segments split.
   */
  MOD_FUNC(core, void, klogf, const char *, ...)

  /* ── Slab allocator ──────────────────────────────────────────────────── */

  /*
   * kmem_pool_init — Initialise a slab allocator pool.
   *
   *   pool      Pool descriptor to initialise.
   *   storage   Backing memory (statically allocated array).
   *   obj_size  Size of each object in bytes.
   *   count     Number of objects that fit in storage.
   */
  MOD_FUNC(core, void, kmem_pool_init, kmem_pool_t *, void *,
                                        size_t, uint32_t)

  /*
   * kmem_alloc — Allocate one object from a slab pool.
   *
   * Returns a pointer to a zeroed object, or NULL if pool is exhausted.
   */
  MOD_FUNC(core, void *, kmem_alloc, kmem_pool_t *)

  /*
   * kmem_free — Return an object to its slab pool.
   *
   *   pool  The pool the object was allocated from.
   *   ptr   Pointer to the object to free.
   */
  MOD_FUNC(core, void, kmem_free, kmem_pool_t *, void *)

  /*
   * kmem_free_count — Return the number of free objects in a pool.
   */
  MOD_FUNC(core, uint32_t, kmem_free_count, const kmem_pool_t *)

MOD_DECLARE_END(core)

#endif /* PPAP_KERNEL_MOD_MOD_CORE_H */
