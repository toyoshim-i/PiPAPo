/*
 * kmem.h — Fixed-size kernel object pool (slab allocator)
 *
 * Provides O(1) alloc/free for same-sized kernel objects (PCBs, file structs,
 * etc.) without the overhead of a general malloc (alignment headers,
 * coalescing).
 *
 * Each pool is backed by a contiguous block of memory — either a static array
 * declared by the caller or a page from the page pool.  The free list is an
 * intrusive singly-linked list threaded through the objects themselves, so
 * there is zero per-object metadata overhead when the object is in use.
 *
 * Usage (static pool — no dynamic allocation):
 *
 *   static uint8_t pcb_storage[PROC_MAX * sizeof(pcb_t)];
 *   static kmem_pool_t pcb_pool;
 *   kmem_pool_init(&pcb_pool, pcb_storage, sizeof(pcb_t), PROC_MAX);
 *
 *   pcb_t *p = kmem_alloc(&pcb_pool);
 *   kmem_free(&pcb_pool, p);
 *
 * Synchronization: kmem pools are intentionally unlocked.  The pool owner
 * must serialize kmem_alloc(), kmem_free(), and kmem_free_count() with the
 * appropriate subsystem lock.
 */

#ifndef PPAP_KERNEL_CORE_MM_KMEM_H
#define PPAP_KERNEL_CORE_MM_KMEM_H

#include "kernel/common/core/kmem_types.h"

/* Initialise a pool over the caller-supplied storage block.
 * `mem`      — start of the backing storage (must be pointer-aligned)
 * `obj_size` — size of each object in bytes; minimum sizeof(void *)
 * `count`    — number of objects in the pool */
void kmem_pool_init(kmem_pool_t *pool, void *mem, size_t obj_size,
                    uint32_t count);

/* Allocate one object from the pool.  Caller must hold the pool owner's lock.
 * Returns NULL if the pool is empty. */
void *kmem_alloc(kmem_pool_t *pool);

/* Return an object to the pool.  Behaviour is undefined if `obj` was not
 * obtained from this pool or has already been freed.  Caller must hold the
 * pool owner's lock. */
void kmem_free(kmem_pool_t *pool, void *obj);

/* Number of objects currently available for allocation.  Caller must hold the
 * pool owner's lock. */
uint32_t kmem_free_count(const kmem_pool_t *pool);

#endif /* PPAP_KERNEL_CORE_MM_KMEM_H */
