/*
 * region.h — Typed class-dispatched mm allocation
 *
 * Class-aware allocator that dispatches to the page pool by default
 * and to arch arenas (Xtensa ram_text, ext_text, ext_rodata) via the
 * mem_helper hooks.  Returns a region_t — the minimal information
 * the mm layer owns: a base pointer and the page id when page-backed.
 *
 * Callers that need a typed image-segment descriptor (vaddr, OWNED
 * flags, mem_class for procfs reporting) build it locally via
 * proc_image_segment_from_region() in proc_image.h.  Those fields are
 * proc/exec-layer concerns; mm does not own them.
 */

#ifndef PPAP_KERNEL_CORE_MM_REGION_H
#define PPAP_KERNEL_CORE_MM_REGION_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/common/core/mem_class.h"
#include "kernel/common/core/region_types.h"

int region_init(void);

int region_alloc(ppap_mem_class_t mem_class, uint32_t size, uint32_t flags,
                 region_t *out);

int region_alloc_at(ppap_mem_class_t mem_class, void *base, uint32_t size,
                    uint32_t flags, region_t *out);

void region_free(ppap_mem_class_t mem_class, uint32_t size, const region_t *r);

/* ── Capacity queries ──────────────────────────────────────────────── */

uint32_t region_total_bytes(ppap_mem_class_t mem_class);

uint32_t region_free_bytes(ppap_mem_class_t mem_class);

uint32_t region_largest_free_bytes(ppap_mem_class_t mem_class);

#endif /* PPAP_KERNEL_CORE_MM_REGION_H */
