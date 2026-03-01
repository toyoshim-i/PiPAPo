/*
 * kmem.h — Fixed-size kernel object pool (slab allocator)
 *
 * Provides O(1) alloc/free for same-sized kernel objects (PCBs, file structs,
 * etc.) without the overhead of a general malloc (alignment headers, coalescing).
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
 */

#ifndef PPAP_MM_KMEM_H
#define PPAP_MM_KMEM_H

#include <stddef.h>
#include <stdint.h>

typedef struct kmem_pool {
    void    *free_list;     /* head of the intrusive free list          */
    size_t   obj_size;      /* bytes per object (>= sizeof(void *))     */
    uint32_t total;         /* total objects in pool                    */
    uint32_t free_count;    /* objects currently on the free list       */
} kmem_pool_t;

/* Initialise a pool over the caller-supplied storage block.
 * `mem`      — start of the backing storage (must be pointer-aligned)
 * `obj_size` — size of each object in bytes; minimum sizeof(void *)
 * `count`    — number of objects in the pool */
void kmem_pool_init(kmem_pool_t *pool, void *mem, size_t obj_size, uint32_t count);

/* Allocate one object from the pool.  Returns NULL if the pool is empty. */
void *kmem_alloc(kmem_pool_t *pool);

/* Return an object to the pool.  Behaviour is undefined if `obj` was not
 * obtained from this pool or has already been freed. */
void kmem_free(kmem_pool_t *pool, void *obj);

/* Number of objects currently available for allocation. */
uint32_t kmem_free_count(const kmem_pool_t *pool);

#endif /* PPAP_MM_KMEM_H */
