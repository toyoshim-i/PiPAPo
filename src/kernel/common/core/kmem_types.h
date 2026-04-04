/*
 * common/core/kmem_types.h — Slab allocator types
 *
 * Extracted from core/mm/kmem.h for cross-module use.
 * Types only — no function declarations.
 */

#ifndef PPAP_KERNEL_COMMON_CORE_KMEM_TYPES_H
#define PPAP_KERNEL_COMMON_CORE_KMEM_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef struct kmem_pool {
  void *free_list;     /* head of the intrusive free list          */
  size_t obj_size;     /* bytes per object (>= sizeof(void *))     */
  uint32_t total;      /* total objects in pool                    */
  uint32_t free_count; /* objects currently on the free list       */
} kmem_pool_t;

#endif /* PPAP_KERNEL_COMMON_CORE_KMEM_TYPES_H */
