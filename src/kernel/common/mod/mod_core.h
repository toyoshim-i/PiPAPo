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

#include "../../mm/mem_layout.h"  /* ppap_mem_class_t, proc_image_segment_t,
                                  * page_id_t (via page.h) */

#include "module.h"

MOD_DECLARE_BEGIN(core)

  /* Fields are sorted alphabetically to match mod_core.inc indices. */

  MOD_FUNC(core, int, blkdev_read, blkdev_t *, void *,
                                    uint32_t, uint32_t)
  MOD_FUNC(core, int, blkdev_write, blkdev_t *, const void *,
                                     uint32_t, uint32_t)
  MOD_FUNC(core, void, klog, const char *)
  MOD_FUNC(core, void, klogf, const char *, ...)
  MOD_FUNC(core, void *, kmem_alloc, kmem_pool_t *)
  MOD_FUNC(core, void, kmem_free, kmem_pool_t *, void *)
  MOD_FUNC(core, uint32_t, kmem_free_count, const kmem_pool_t *)
  MOD_FUNC(core, void, kmem_pool_init, kmem_pool_t *, void *,
                                        size_t, uint32_t)
  MOD_FUNC(core, int, mem_region_alloc, proc_image_segment_t *,
                       ppap_mem_class_t, uint32_t, uint32_t)
  MOD_FUNC(core, void, mem_region_free, const proc_image_segment_t *)
  MOD_FUNC(core, uint32_t, mem_region_free_bytes, ppap_mem_class_t)
  MOD_FUNC(core, page_id_t, mem_region_page_alloc, void)
  MOD_FUNC(core, void, mem_region_page_free, page_id_t)
  MOD_FUNC(core, void, mem_region_page_read, page_id_t, uint16_t,
                                              void *, uint16_t)
  MOD_FUNC(core, void, mem_region_page_write, page_id_t, uint16_t,
                                               const void *, uint16_t)
  MOD_FUNC(core, uint32_t, mem_region_total_bytes, ppap_mem_class_t)
  MOD_FUNC(core, uint32_t, sched_get_ticks, void)
  MOD_FUNC(core, void, sched_wakeup, void *)
  MOD_FUNC(core, void, sched_yield, void)
  MOD_FUNC(core, void, svc_set_restart, void)
  MOD_FUNC(core, int, uart_getc, void)
  MOD_FUNC(core, int, uart_putc, char, void (*)(void))
  MOD_FUNC(core, int, uart_rx_avail, void)

MOD_DECLARE_END(core)

/* MOD_CORE_FUNC_COUNT is defined in mod_core.inc — the single source
 * of truth shared by both C and assembly stubs. */
#define MOD_CORE_ENTRY(name, idx) /* count only */
#include "mod_core.inc"
#undef MOD_CORE_ENTRY
_Static_assert(sizeof(mod_core_t) == MOD_CORE_FUNC_COUNT * sizeof(void (*)(void)),
               "mod_core_t size mismatch — update MOD_CORE_FUNC_COUNT in "
               "mod_core.inc");

#endif /* PPAP_KERNEL_MOD_MOD_CORE_H */
