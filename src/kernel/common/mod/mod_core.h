/*
 * mod_core.h — Core kernel module interface
 *
 * The core module provides common services used by ALL other modules:
 * logging, slab memory allocation, and region-based page allocation.
 *
 * Inline functions (spinlock, string, arch_irq) do NOT need module
 * thunks — they compile into each module's code segment directly.
 * Only functions defined in .c files are exported here.
 *
 * Usage:
 *   #include "common/mod/mod_core.h"
 *   mod_core.klogf("VFS: %s\n", msg);
 *   void *p = mod_core.kmem_alloc(&pool);
 *   mod_core.mem_region_alloc(&seg, PPAP_MEM_RAM_DATA, PAGE_SIZE, flags);
 *
 * Implementation: src/kernel/klog.c, src/kernel/mm/kmem.c,
 *                 src/kernel/mm/mem_region.c
 */

#ifndef PPAP_KERNEL_MOD_MOD_CORE_H
#define PPAP_KERNEL_MOD_MOD_CORE_H

#include <stdint.h>
#include <stddef.h>

/* Forward declarations — full definitions in mm/ and blkdev/ headers */
struct kmem_pool;
typedef struct kmem_pool kmem_pool_t;
struct blkdev;
typedef struct blkdev blkdev_t;

#include "../../mm/mem_layout.h"  /* ppap_mem_class_t, proc_image_segment_t */
#include "../../mm/page.h"       /* page_id_t */

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

  /* ── Region allocator (page-granularity) ──────────────────────────────── */

  /*
   * mem_region_alloc — Allocate a memory region.
   *
   *   seg        Output segment descriptor (base, size, class filled in).
   *   mem_class  Memory class (PPAP_MEM_RAM_DATA, PPAP_MEM_RAM_STACK, ...).
   *   size       Requested size in bytes (rounded up to page boundary).
   *   flags      PROC_IMAGE_SEG_WRITABLE, PROC_IMAGE_SEG_OWNED, etc.
   *
   * Returns 0 on success, -ENOMEM or -EINVAL on failure.
   */
  MOD_FUNC(core, int, mem_region_alloc, proc_image_segment_t *,
                       ppap_mem_class_t, uint32_t, uint32_t)

  /*
   * mem_region_free — Release a previously allocated memory region.
   *
   *   seg  Segment descriptor returned by mem_region_alloc.
   */
  MOD_FUNC(core, void, mem_region_free, const proc_image_segment_t *)

  /*
   * mem_region_total_bytes — Total bytes available for a memory class.
   * mem_region_free_bytes  — Free bytes available for a memory class.
   *
   * Used by procfs for /proc/meminfo reporting.
   */
  MOD_FUNC(core, uint32_t, mem_region_total_bytes, ppap_mem_class_t)
  MOD_FUNC(core, uint32_t, mem_region_free_bytes, ppap_mem_class_t)

  /* ── Page-indexed memory ─────────────────────────────────────────────── */

  /*
   * mm_page_alloc — Allocate one page, return its index.
   *
   * Returns PAGE_ID_INVALID on OOM.
   */
  MOD_FUNC(core, page_id_t, mm_page_alloc, void)

  /*
   * mm_page_free — Free a page by index.
   */
  MOD_FUNC(core, void, mm_page_free, page_id_t)

  /*
   * mm_page_read — Read `len` bytes from page `id` at offset `off` into `buf`.
   */
  MOD_FUNC(core, void, mm_page_read, page_id_t, uint16_t, void *, uint16_t)

  /*
   * mm_page_write — Write `len` bytes from `buf` to page `id` at offset `off`.
   */
  MOD_FUNC(core, void, mm_page_write, page_id_t, uint16_t, const void *,
                                       uint16_t)

  /* ── Block device I/O (cross-module safe) ────────────────────────────── */

  /*
   * blkdev_read — Read sectors from a block device.
   *
   * Wrapper around dev->read() that executes in core's CS.
   * Needed because blkdev_t function pointers are near pointers
   * valid only in core's code segment.  VFS modules must call
   * this instead of dev->read() directly.
   *
   *   dev     Block device (in SS=0 shared data).
   *   buf     Destination buffer (in SS=0).
   *   sector  Starting sector number.
   *   count   Number of sectors to read.
   *
   * Returns 0 on success, negative errno on failure.
   */
  MOD_FUNC(core, int, blkdev_read, blkdev_t *, void *,
                                    uint32_t, uint32_t)

  /*
   * blkdev_write — Write sectors to a block device.
   */
  MOD_FUNC(core, int, blkdev_write, blkdev_t *, const void *,
                                     uint32_t, uint32_t)

MOD_DECLARE_END(core)

/* Number of function pointers in mod_core_t.
 * Must match core_stubs.S slot count and core_entries.S stub count.
 * Static assert catches mismatches at compile time. */
#define MOD_CORE_FUNC_COUNT 16
_Static_assert(sizeof(mod_core_t) == MOD_CORE_FUNC_COUNT * sizeof(void (*)(void)),
               "mod_core_t size mismatch — update MOD_CORE_FUNC_COUNT, "
               "core_stubs.S, and core_entries.S");

#endif /* PPAP_KERNEL_MOD_MOD_CORE_H */
