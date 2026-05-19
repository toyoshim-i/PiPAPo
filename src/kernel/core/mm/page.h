/*
 * page.h — Page-id wrapper over the pure pool core
 *
 * Wraps page_pool.c with SPIN_PAGE, OOM / double-free klogf messages,
 * and the boot-time memory-map print.  All allocations return a
 * page_id_t (linear page number = address / PAGE_SIZE) instead of
 * void *.
 *
 * Boundary: this header is the mm-layer's public surface for page
 * allocation.  Callers that need to copy bytes in or out of a page
 * include mm/page_io.h; callers that need a typed segment descriptor
 * (proc_image_segment_t, mem_class_t arena dispatch) go through
 * region_alloc — see region.h.
 */

#ifndef PPAP_KERNEL_CORE_MM_PAGE_H
#define PPAP_KERNEL_CORE_MM_PAGE_H

#include <stdint.h>

#include "kernel/common/config.h"
#include "kernel/common/core/page_types.h"

/* Memory map constants (SRAM_*, PAGE_POOL_*) are in config.h.
 * PAGE_SIZE and PAGE_COUNT_MAX are also in config.h. */

/* ── Init ────────────────────────────────────────────────────────────────── */

/* Initialise the page pool and print the boot-time memory map.
 * Must be called once from kmain(), after UART is ready. */
void mm_init(void);

/* ── Allocation ─────────────────────────────────────────────────────────── */

/* Allocate one page.  Returns its page_id_t (or PAGE_ID_INVALID on OOM). */
page_id_t page_alloc(void);

/* Allocate n contiguous pages.  Returns the base page_id_t
 * (or PAGE_ID_INVALID). */
page_id_t page_alloc_n(uint32_t n_pages);

/* Allocate the longest free contiguous run within [min, max] pages and
 * report the actual count via *got_pages.  Returns PAGE_ID_INVALID if no
 * run of at least min_pages exists. */
page_id_t page_alloc_largest(uint32_t min_pages, uint32_t max_pages,
                             uint32_t *got_pages);

/* Allocate the specific page identified by `id`.  Returns 0 on success,
 * -1 if the page is not currently free (or not in any pool block).
 * Used by region_alloc_at() to place segments at link-time addresses. */
int page_alloc_at(page_id_t id);

/* Free one pool page by page_id_t. */
void page_free(page_id_t id);

/* ── Page-id / address conversions ──────────────────────────────────────── */

/* Return the 32-bit linear address of a page_id_t.
 * Returns 0 for PAGE_ID_INVALID. */
uint32_t page_linear(page_id_t id);

/* page_to_ptr (page_id_t -> flat void *) is a migration shim that
 * lives in mm/page_ptr.h.  i16 builds omit page_ptr.c on purpose so
 * an unguarded caller surfaces as a link error.  Prefer page_io.h. */

/* Return the page_id_t for an existing pointer (linear address / PAGE_SIZE).
 * Returns PAGE_ID_INVALID only for NULL. */
page_id_t page_from_ptr(void *ptr);

/* ── Pool introspection ─────────────────────────────────────────────────── */

/* Return the runtime page pool base linear address.  Set during
 * mm_init: equals PAGE_POOL_BASE on targets with a fixed pool, or
 * whatever mem_helper_init_pool wrote for targets that allocate
 * the pool at boot. */
uint32_t page_pool_base(void);

/* Return the number of pages currently free in the pool. */
uint32_t page_free_count(void);

/* Return the length (in pages) of the largest contiguous free run.
 * Used by malloc to report accurate availability. */
uint32_t page_max_contiguous(void);

/* page_count and oom_count externs are in page_types.h */

#endif /* PPAP_KERNEL_CORE_MM_PAGE_H */
