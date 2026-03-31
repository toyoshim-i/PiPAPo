/*
 * mem_region.h — Memory-region allocation helpers
 *
 * Provides a small allocator boundary between loaders and target-specific
 * memory backends. Xtensa now uses this for PPAP-owned internal and
 * external arenas, while other classes keep their existing page-backed
 * behaviour.
 */

#ifndef PPAP_KERNEL_MM_MEM_REGION_H
#define PPAP_KERNEL_MM_MEM_REGION_H

#include <stdint.h>

#include "mem_layout.h"
#include "page.h"

int mem_region_init(void);

int mem_region_alloc(proc_image_segment_t *seg, ppap_mem_class_t mem_class,
                     uint32_t size, uint32_t flags);

int mem_region_alloc_at(proc_image_segment_t *seg, ppap_mem_class_t mem_class,
                        void *base, uint32_t size, uint32_t flags);

void mem_region_free(const proc_image_segment_t *seg);

void mem_region_free_tracked_page(void *page);

/* Free a tracked user page by page index. */
void mem_region_free_tracked_page_id(page_id_t id);

uint32_t mem_region_total_bytes(ppap_mem_class_t mem_class);

uint32_t mem_region_free_bytes(ppap_mem_class_t mem_class);

uint32_t mem_region_largest_free_bytes(ppap_mem_class_t mem_class);

#endif /* PPAP_KERNEL_MM_MEM_REGION_H */
