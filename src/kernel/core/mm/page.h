/*
 * page.h — Physical page allocator backend
 *
 * Manages the page pool starting at 0x20005000 (size set by PAGE_COUNT_MAX).
 * Uses a free-stack (array-based LIFO) for O(1) alloc/free with no per-page
 * overhead.
 *
 * All pages are single-owner; no reference counting.  True CoW is not
 * feasible on the RP2040's 4-region MPU — see phase01-plan.md §Step 1.
 *
 * This header is backend-facing.  New code outside `src/kernel/mm/` should
 * allocate via `mem_region_*` so architecture-specific memory policy stays
 * centralized in `mem_region.c`.
 */

#ifndef PPAP_KERNEL_CORE_MM_PAGE_H
#define PPAP_KERNEL_CORE_MM_PAGE_H

#include <stdint.h>

#include "kernel/common/config.h"
#include "kernel/common/core/page_types.h"

/* Memory map constants (SRAM_*, PAGE_POOL_*) are in config.h.
 * PAGE_SIZE and PAGE_COUNT_MAX are also in config.h. */

/* ── API ─────────────────────────────────────────────────────────────────────
 */

/* Initialise the page pool and print the boot-time memory map.
 * Must be called once from kmain(), after UART is ready. */
void mm_init(void);

/* Allocate one 4 KB page.  Returns a 4-KB-aligned pointer into the page pool,
 * or NULL if the pool is exhausted (OOM). */
void *page_alloc(void);

/* Allocate the specific page at `addr`.  Returns `addr` on success, or NULL
 * if the address is not page-aligned, out of range, or already allocated.
 * Used by execve() to place user code at its linked address. */
void *page_alloc_at(void *addr);

/* Return a page to the pool.  Behaviour is undefined if `page` was not
 * obtained from page_alloc(), or if it is freed more than once. */
void page_free(void *page);

/* Return the runtime page pool base linear address.  On Xtensa this is
 * dynamically allocated from ESP-IDF's heap; on i16 it may be advanced
 * above reserved low-memory partitions; on other targets it equals the
 * compile-time PAGE_POOL_BASE. */
uint32_t page_pool_base(void);

/* Return the number of pages currently on the free stack. */
uint32_t page_free_count(void);

/* Return the length (in pages) of the largest contiguous free run.
 * Used by _MALLOC to report accurate availability. */
uint32_t page_max_contiguous(void);

/* Allocate n_pages contiguous pages.  Returns a pointer to the first page,
 * or NULL if no contiguous run of that size is available.
 * Uses a bitmap scan — O(page_count), safe for large pools. */
uint8_t *page_alloc_contiguous(uint32_t n_pages);

/* page_count and oom_count externs are in page_types.h */

/* ── Page-indexed API (mm-internal) ───────────────────────────────────────
 *
 * These functions are internal to src/kernel/mm/.  Code outside mm/
 * should use the mem_region_page_* wrappers in mem_region.h instead.
 *
 * Allocations return a page_id_t (linear page number = address / PAGE_SIZE)
 * instead of void *.  Data on pages is accessed via mm_page_read/write which
 * handles segment setup on i16 internally.  On 32-bit, it is memcpy to/from
 * the linear address described by the page_id + offset pair.
 */

/* Allocate one page from the page pool, return its page_id_t
 * (or PAGE_ID_INVALID on OOM). */
page_id_t mm_page_alloc(void);

/* Allocate n contiguous pool pages, return the base page_id_t
 * (or PAGE_ID_INVALID). */
page_id_t mm_page_alloc_contiguous(uint32_t n_pages);

/* Free one pool page by page_id_t. */
void mm_page_free(page_id_t id);

/* Return the 32-bit linear address of a page_id_t. */
uint32_t mm_page_linear(page_id_t id);

/* Read `len` bytes from page `id` at byte offset `off` into `buf`. */
void mm_page_read(page_id_t id, uint16_t off, void *buf, uint16_t len);

/* Write `len` bytes from `buf` to page `id` at byte offset `off`. */
void mm_page_write(page_id_t id, uint16_t off, const void *buf, uint16_t len);

/* Return the linear base pointer of a page_id_t (32-bit only).
 * On i16 this is NOT available — use mm_page_read/write instead. */
#if !defined(__ia16__)
void *mm_page_to_ptr(page_id_t id);
#endif

/* Return the page_id_t for an existing pointer (linear address / PAGE_SIZE).
 * Returns PAGE_ID_INVALID only for NULL. */
page_id_t mm_ptr_to_page(void *ptr);

#endif /* PPAP_KERNEL_CORE_MM_PAGE_H */
