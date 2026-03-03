/*
 * page.h — Physical page allocator
 *
 * Manages the page pool starting at 0x20005000 (size set by PAGE_COUNT).
 * Uses a free-stack (array-based LIFO) for O(1) alloc/free with no per-page
 * overhead.
 *
 * All pages are single-owner; no reference counting.  True CoW is not
 * feasible on the RP2040's 4-region MPU — see phase1-plan.md §Step 1.
 */

#ifndef PPAP_MM_PAGE_H
#define PPAP_MM_PAGE_H

#include <stdint.h>
#include "config.h"

/* ── Memory map constants (must match ppap.ld / qemu.ld) ──────────────────── */
/* PAGE_SIZE and PAGE_COUNT are defined in config.h */

#define SRAM_KERNEL_BASE  0x20000000u       /* kernel data region start        */
#define SRAM_KERNEL_SIZE     (20u * 1024u)  /* 20 KB reserved for kernel       */
#define PAGE_POOL_BASE    0x20005000u       /* first page in the pool          */
#define PAGE_POOL_SIZE    (PAGE_COUNT * PAGE_SIZE)
#define SRAM_IOBUF_BASE   (PAGE_POOL_BASE + PAGE_POOL_SIZE)  /* after pool    */
#define SRAM_IOBUF_SIZE      (24u * 1024u)  /* 24 KB                           */
#define SRAM_DMA_BASE     (SRAM_IOBUF_BASE + SRAM_IOBUF_SIZE)
#define SRAM_DMA_SIZE        (16u * 1024u)  /* 16 KB                           */

/* ── API ───────────────────────────────────────────────────────────────────── */

/* Initialise the page pool and print the boot-time memory map.
 * Must be called once from kmain(), after UART is ready. */
void mm_init(void);

/* Allocate one 4 KB page.  Returns a 4-KB-aligned pointer into the page pool,
 * or NULL if the pool is exhausted (OOM). */
void *page_alloc(void);

/* Allocate the specific page at `addr`.  Returns `addr` on success, or NULL
 * if the address is not page-aligned, out of range, or already allocated.
 * Used by do_execve() to place user code at its linked address. */
void *page_alloc_at(void *addr);

/* Return a page to the pool.  Behaviour is undefined if `page` was not
 * obtained from page_alloc(), or if it is freed more than once. */
void page_free(void *page);

/* Return the number of pages currently on the free stack. */
uint32_t page_free_count(void);

#endif /* PPAP_MM_PAGE_H */
