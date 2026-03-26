/*
 * mem_region.h — Memory-region allocation helpers
 *
 * Provides a small allocator boundary between loaders and target-specific
 * memory backends.  Step XT-2.2 uses this to move Xtensa IRAM/DRAM policy
 * out of the ELF loader without changing process teardown yet.
 */

#ifndef PPAP_KERNEL_MM_MEM_REGION_H
#define PPAP_KERNEL_MM_MEM_REGION_H

#include <stdint.h>

#include "mem_layout.h"

int mem_region_alloc(proc_image_segment_t *seg, ppap_mem_class_t mem_class,
                     uint32_t size, uint32_t flags);

void mem_region_free(const proc_image_segment_t *seg);

#endif /* PPAP_KERNEL_MM_MEM_REGION_H */
