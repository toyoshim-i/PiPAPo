/*
 * kmem.c — Fixed-size kernel object pool
 *
 * The free list is intrusive: when an object is free, its first sizeof(void *)
 * bytes hold a pointer to the next free object.  This imposes a minimum object
 * size of sizeof(void *) = 4 bytes on a 32-bit target, which is always satisfied
 * by real kernel objects (PCBs, file structs, etc.).
 *
 * kmem_pool_init() threads all objects into the free list by walking the backing
 * storage in obj_size strides and writing the next pointer into each slot.
 * The last slot points to NULL (end of list).
 *
 * kmem_alloc(): pop the head of the free list.  O(1).
 * kmem_free():  push to the head of the free list.  O(1).
 *
 * No locks — callers are responsible for mutual exclusion.  In Phase 1 the
 * kernel runs with interrupts disabled during PCB allocation; in later phases
 * a spinlock will be added around pool operations.
 */

#include "kmem.h"
#include <stddef.h>   /* NULL */

void kmem_pool_init(kmem_pool_t *pool, void *mem, size_t obj_size, uint32_t count)
{
    uint8_t *p = (uint8_t *)mem;

    pool->obj_size   = obj_size;
    pool->total      = count;
    pool->free_count = count;

    /* Thread each slot into the free list */
    for (uint32_t i = 0u; i < count; i++) {
        void *next = (i + 1u < count) ? (void *)(p + (i + 1u) * obj_size) : NULL;
        /* Write `next` pointer into the first sizeof(void*) bytes of the slot */
        __builtin_memcpy(p + i * obj_size, &next, sizeof(void *));
    }
    pool->free_list = (count > 0u) ? mem : NULL;
}

void *kmem_alloc(kmem_pool_t *pool)
{
    if (!pool->free_list)
        return NULL;

    void *obj = pool->free_list;
    /* Advance free_list to the next pointer stored at the head of obj */
    __builtin_memcpy(&pool->free_list, obj, sizeof(void *));
    pool->free_count--;
    return obj;
}

void kmem_free(kmem_pool_t *pool, void *obj)
{
    if (!obj)
        return;
    /* Write the current head into the first bytes of obj, then set obj as head */
    __builtin_memcpy(obj, &pool->free_list, sizeof(void *));
    pool->free_list = obj;
    pool->free_count++;
}

uint32_t kmem_free_count(const kmem_pool_t *pool)
{
    return pool->free_count;
}
