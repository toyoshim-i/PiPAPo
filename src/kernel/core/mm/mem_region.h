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

#include "kernel/common/core/mem_layout.h"
#include "kernel/core/mm/page.h"

int mem_region_init(void);

int mem_region_alloc(proc_image_segment_t *seg, ppap_mem_class_t mem_class,
                     uint32_t size, uint32_t flags);

int mem_region_alloc_at(proc_image_segment_t *seg, ppap_mem_class_t mem_class,
                        void *base, uint32_t size, uint32_t flags);

void mem_region_free(const proc_image_segment_t *seg);

/* Free a tracked user page by page_id_t. */
void mem_region_free_tracked_page_id(page_id_t id);

/* ── Page wrappers (public API for mm_page_* internals) ────────────── */

/* Allocate one page, return its page_id_t (or PAGE_ID_INVALID on OOM). */
page_id_t mem_region_page_alloc(void);

/* Allocate n contiguous pages, return the base page_id_t
 * (or PAGE_ID_INVALID). */
page_id_t mem_region_page_alloc_contiguous(uint32_t n_pages);

/* Allocate the longest available contiguous run within [min, max] pages and
 * grab all of it atomically.  Reports the actual page count via *got.
 * Returns PAGE_ID_INVALID if no free run of at least min pages exists. */
page_id_t mem_region_page_alloc_largest_contiguous(uint32_t min_pages,
                                                   uint32_t max_pages,
                                                   uint32_t *got_pages);

/* Free a page by page_id_t. */
void mem_region_page_free(page_id_t id);

/* Return the 32-bit linear address of a page_id_t. */
uint32_t mem_region_page_linear(page_id_t id);

/* Return the page_id for an existing pointer (reverse lookup). */
page_id_t mem_region_ptr_to_page(void *ptr);

/* Return a dereferenceable pointer (32-bit only, not available on i16). */
#if !defined(__ia16__)
void *mem_region_page_to_ptr(page_id_t id);
#endif

/* Read `len` bytes from page `id` at byte offset `off` into `buf`. */
void mem_region_page_read(page_id_t id, uint16_t off, void *buf, uint16_t len);

/* Write `len` bytes from `buf` to page `id` at byte offset `off`. */
void mem_region_page_write(page_id_t id, uint16_t off, const void *buf,
                           uint16_t len);

/* Write `len` zero bytes to page `id` starting at byte offset `off`. */
void mem_region_page_zero(page_id_t id, uint16_t off, uint16_t len);

/* ── Capacity queries ──────────────────────────────────────────────── */

uint32_t mem_region_total_bytes(ppap_mem_class_t mem_class);

uint32_t mem_region_free_bytes(ppap_mem_class_t mem_class);

uint32_t mem_region_largest_free_bytes(ppap_mem_class_t mem_class);

#endif /* PPAP_KERNEL_CORE_MM_MEM_REGION_H */
