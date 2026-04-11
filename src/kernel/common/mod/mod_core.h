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
 *   #include "kernel/common/mod/mod_core.h"
 *   mod_core.klogf("VFS: %s\n", msg);
 *   void *p = mod_core.kmem_alloc(&pool);
 *   mod_core.mem_region_alloc(&seg, PPAP_MEM_RAM_DATA, PAGE_SIZE, flags);
 *
 * Implementation: src/kernel/klog.c, src/kernel/mm/kmem.c,
 *                 src/kernel/mm/mem_region.c
 */

#ifndef PPAP_KERNEL_COMMON_MOD_MOD_CORE_H
#define PPAP_KERNEL_COMMON_MOD_MOD_CORE_H

#include <stddef.h>
#include <stdint.h>

/* Forward declarations — full definitions in mm/, proc/ headers */
struct kmem_pool;
typedef struct kmem_pool kmem_pool_t;
struct pcb;

#include "kernel/common/core/mem_layout.h" /* ppap_mem_class_t, proc_image_segment_t,
                                  * page_id_t (via page.h) */

#include "kernel/common/mod/module.h"

MOD_DECLARE_BEGIN(core)

/* Fields are sorted alphabetically to match mod_core.inc indices. */

/*
 * kmem_alloc — Allocate one object from a slab pool in O(1).
 *
 *   pool  Initialized slab pool.
 *
 * Returns pointer to object, or NULL if pool exhausted.
 */
MOD_FUNC(core, void *, kmem_alloc, kmem_pool_t *)

/*
 * kmem_free — Return an object to a slab pool in O(1).
 *
 *   pool  Pool the object was allocated from.
 *   obj   Object to free (NULL is a no-op).
 */
MOD_FUNC(core, void, kmem_free, kmem_pool_t *, void *)

/*
 * kmem_free_count — Query free objects in a slab pool.
 *
 *   pool  Pool to query.
 *
 * Returns count of objects available for allocation.
 */
MOD_FUNC(core, uint32_t, kmem_free_count, const kmem_pool_t *)

/*
 * kmem_pool_init — Initialize a fixed-size slab pool.
 *
 *   pool      Pool structure to initialize.
 *   mem       Backing storage (contiguous block).
 *   obj_size  Size of each object (>= sizeof(void *)).
 *   count     Maximum number of objects.
 *
 * Threads a free list through the objects for O(1) alloc/free.
 */
MOD_FUNC(core, void, kmem_pool_init, kmem_pool_t *, void *, size_t, uint32_t)

/*
 * mem_region_alloc — Allocate a memory region by class.
 *
 *   seg        Output segment descriptor (base, size, base_page).
 *   mem_class  Memory type (RAM_TEXT, RAM_DATA, RAM_STACK, etc.).
 *   size       Requested size in bytes.
 *   flags      PROC_IMAGE_SEG_WRITABLE, PROC_IMAGE_SEG_OWNED, etc.
 *
 * Returns 0 on success, negative errno (EINVAL, ENOMEM) on failure.
 * On Xtensa, routes to ESP-IDF arena or page pool based on class.
 */
MOD_FUNC(core, int, mem_region_alloc, proc_image_segment_t *, ppap_mem_class_t,
         uint32_t, uint32_t)

/*
 * mem_region_free — Free a previously allocated memory region.
 *
 *   seg  Segment to deallocate (no-op if NULL or zero-sized).
 *
 * Dispatches to class-specific free path (page pool or arena).
 */
MOD_FUNC(core, void, mem_region_free, const proc_image_segment_t *)

/*
 * mem_region_free_bytes — Query free memory in a memory class.
 *
 *   mem_class  Memory type to query.
 *
 * Returns bytes available for allocation.
 */
MOD_FUNC(core, uint32_t, mem_region_free_bytes, ppap_mem_class_t)

/*
 * mem_region_page_read — Read bytes from a page at an offset.
 *
 *   id   Page index.
 *   off  Byte offset within page (0..PAGE_SIZE-1).
 *   buf  Destination buffer.
 *   len  Bytes to read.
 *
 * Safe on all architectures including i16 (handles segment
 * register setup internally).
 */
MOD_FUNC(core, void, mem_region_page_read, page_id_t, uint16_t, void *,
         uint16_t)

/*
 * mem_region_page_write — Write bytes to a page at an offset.
 *
 *   id   Page index.
 *   off  Byte offset within page (0..PAGE_SIZE-1).
 *   buf  Source buffer.
 *   len  Bytes to write.
 *
 * Safe on all architectures including i16.
 */
MOD_FUNC(core, void, mem_region_page_write, page_id_t, uint16_t, const void *,
         uint16_t)

/*
 * mem_region_total_bytes — Query total capacity of a memory class.
 *
 *   mem_class  Memory type to query.
 *
 * Returns total bytes (pool or arena size).
 */
MOD_FUNC(core, uint32_t, mem_region_total_bytes, ppap_mem_class_t)

/*
 * sched_get_ticks — Get monotonic tick count since boot.
 *
 * Returns uint32_t tick counter.  One tick = 10 ms (100 Hz).
 */
MOD_FUNC(core, uint32_t, sched_get_ticks, void)

/*
 * sched_wakeup — Wake all processes blocked on a wait channel.
 *
 *   channel  Wait channel (typically &pipe, &tty_dev, &vnode).
 *
 * Marks matching PROC_BLOCKED processes RUNNABLE and triggers
 * a reschedule.
 */
MOD_FUNC(core, void, sched_wakeup, void *)

/*
 * sched_switch — Voluntarily switch to the next process.
 *
 * On ARM, pends PendSV.  On m68k, TRAP #1.  On RISC-V/Xtensa/i16,
 * arch_yield().
 */
MOD_FUNC(core, void, sched_switch, void)

/*
 * svc_set_restart — Mark current syscall for replay on wakeup.
 *
 * Used by blocking syscalls (pipe read/write, TTY read, sleep).
 * The SVC handler saves original arguments and restores them when
 * the process is woken, re-entering the syscall transparently.
 */
/*
 * subsys_read_proc — Generate /proc content for a subsystem.
 *
 *   tag     Subsystem index (SUBSYS_PPAP, etc.).
 *   p       Target process (NULL to probe).
 *   name    Filename (e.g. "termconv", NULL to probe).
 *   buf     Output buffer (NULL to probe).
 *   bufsiz  Buffer size.
 *
 * Returns bytes written (>= 0) on success.
 * Returns -1 if the subsystem has no handler (probe check).
 */
MOD_FUNC(core, int, subsys_read_proc, int, struct pcb *, const char *, char *,
         int)

MOD_FUNC(core, void, svc_set_restart, void)

MOD_DECLARE_END(core)

/* MOD_CORE_FUNC_COUNT is defined in mod_core.inc — the single source
 * of truth shared by both C and assembly stubs. */
#define MOD_CORE_ENTRY(name, idx) /* count only */
#include "kernel/common/mod/mod_core.inc"
#undef MOD_CORE_ENTRY
_Static_assert(sizeof(mod_core_t) ==
                   MOD_CORE_FUNC_COUNT * sizeof(void (*)(void)),
               "mod_core_t size mismatch — update MOD_CORE_FUNC_COUNT in "
               "mod_core.inc");

#endif /* PPAP_KERNEL_COMMON_MOD_MOD_CORE_H */
