/*
 * page.h — Page-id wrapper over the pure pool core
 *
 * Wraps page_pool.c with SPIN_PAGE, OOM / double-free klogf messages,
 * and the boot-time memory-map print.  All allocations return a
 * page_id_t (linear page number = address / PAGE_SIZE) instead of
 * void *.  Data on pages is accessed via page_read / page_write /
 * page_zero, which handle segment register setup on i16 internally;
 * on 32-bit those collapse to memcpy / memset against the linear
 * address described by the (page_id, offset) pair.
 *
 * Boundary: this header is the mm-layer's public surface.  Code
 * outside src/kernel/core/mm/ allocates and frees pool pages through
 * these names.  Callers that need a typed segment descriptor
 * (proc_image_segment_t, mem_class_t arena dispatch) still go through
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

/* ── Page-payload access ────────────────────────────────────────────────── */

/* Return the 32-bit linear address of a page_id_t.
 * Returns 0 for PAGE_ID_INVALID. */
uint32_t page_linear(page_id_t id);

/* Return the linear base pointer of a page_id_t (32-bit only).
 * On i16 this is NOT available — use page_read / page_write instead. */
#if !defined(__ia16__)
void *page_to_ptr(page_id_t id);
#endif

/* Return the page_id_t for an existing pointer (linear address / PAGE_SIZE).
 * Returns PAGE_ID_INVALID only for NULL. */
page_id_t page_from_ptr(void *ptr);

/* Read `len` bytes from page `id` at byte offset `off` into `buf`. */
void page_read(page_id_t id, uint16_t off, void *buf, uint16_t len);

/* Write `len` bytes from `buf` to page `id` at byte offset `off`. */
void page_write(page_id_t id, uint16_t off, const void *buf, uint16_t len);

/* Write `len` zero bytes to page `id` starting at byte offset `off`. */
void page_zero(page_id_t id, uint16_t off, uint16_t len);

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
