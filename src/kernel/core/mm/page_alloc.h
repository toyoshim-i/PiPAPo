/*
 * page_alloc.h — Pure page-allocator core (no locking, no klogf)
 *
 * Address-sorted list of contiguous free runs.  Each entry describes one
 * run as (base page_id, page count).  Invariants:
 *
 *   - Entries are sorted by `base` in ascending order.
 *   - No two adjacent entries are address-contiguous (always merged on free).
 *   - `pages > 0` for every valid entry.
 *
 * Allocations use best-fit (smallest block with `pages >= n`) so single-page
 * allocs fill the smallest holes first and large runs stay intact for big
 * contiguous requests.  Free coalesces with both address-neighbors so
 * transient alloc/free cycles cannot permanently fragment the pool.
 *
 * The core has NO external dependencies beyond <stdint.h> and page_types.h,
 * which is why it compiles as-is on the host and is unit-tested in
 * tests/host/test_page_alloc.c.  page.c wraps these entry points with
 * SPIN_PAGE and adds klogf messages for OOM and double-free events.
 */

#ifndef PPAP_KERNEL_CORE_MM_PAGE_ALLOC_H
#define PPAP_KERNEL_CORE_MM_PAGE_ALLOC_H

#include <stdint.h>

#include "kernel/common/core/page_types.h"

/* Reset the allocator to a fully-empty state.  Call once at boot before
 * any page_alloc_add_range(). */
void page_alloc_reset(void);

/* Seed the pool with `n` contiguous free pages starting at `base`.  Intended
 * for init time — ranges added here are NOT coalesced with existing entries.
 * Call in ascending-`base` order.  Returns 0 on success, -1 if the metadata
 * array is full. */
int page_alloc_add_range(page_id_t base, uint16_t n);

/* Allocate `n` contiguous pages.  Single-page (n=1) takes the BACK of
 * the highest-address block (absolute highest free pid); multi-page
 * takes the FRONT of the lowest block that fits (first-fit-from-low).
 * Returns the base page_id on success, PAGE_ID_INVALID on OOM. */
page_id_t page_alloc_n(uint16_t n);

/* Grab a specific page by id.  Returns 0 on success, -1 if the page is not
 * currently in any free block. */
int page_alloc_at_id(page_id_t id);

/* Return `n` consecutive pages starting at `base` to the pool.  Coalesces
 * with address-adjacent neighbors.  Returns 0 on success, -1 if the
 * metadata array is full (rare — can only happen if n=0 caller violates
 * the invariant by inserting mid-pool without coalescing). */
int page_alloc_free_range(page_id_t base, uint16_t n);

/* Sum of pages across all free blocks. */
uint32_t page_alloc_free_total(void);

/* Length of the longest single free run. */
uint16_t page_alloc_max_contiguous(void);

/* Return non-zero if `id` is in the free pool (used by page.c for the
 * double-free check). */
int page_alloc_is_free(page_id_t id);

/* ── Test-only introspection ─────────────────────────────────────────────── */
#if defined(PPAP_HOST_TEST)
/* Expose the current free-block count and a snapshot-read so host tests
 * can assert invariants after each operation. */
uint32_t page_alloc_block_count(void);
void page_alloc_block_get(uint32_t idx, page_id_t *base, uint16_t *pages);
#endif

#endif /* PPAP_KERNEL_CORE_MM_PAGE_ALLOC_H */
