/*
 * mem_region.h — Memory-region allocation helpers
 *
 * Provides a small allocator boundary between loaders and target-
 * specific memory backends.  The default path is page-backed for
 * every memory class; arches that need additional arenas plug in
 * through the mem_helper hooks (see mem_helper.h).
 */

#ifndef PPAP_KERNEL_CORE_MM_MEM_REGION_H
#define PPAP_KERNEL_CORE_MM_MEM_REGION_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/common/core/proc_image.h"
#include "kernel/core/mm/page.h"

int mem_region_init(void);

int mem_region_alloc(proc_image_segment_t *seg, ppap_mem_class_t mem_class,
                     uint32_t size, uint32_t flags);

int mem_region_alloc_at(proc_image_segment_t *seg, ppap_mem_class_t mem_class,
                        void *base, uint32_t size, uint32_t flags);

void mem_region_free(const proc_image_segment_t *seg);

/* ── Capacity queries ──────────────────────────────────────────────── */

uint32_t mem_region_total_bytes(ppap_mem_class_t mem_class);

uint32_t mem_region_free_bytes(ppap_mem_class_t mem_class);

uint32_t mem_region_largest_free_bytes(ppap_mem_class_t mem_class);

#endif /* PPAP_KERNEL_CORE_MM_MEM_REGION_H */
