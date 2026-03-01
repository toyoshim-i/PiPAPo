/*
 * page.h — Physical page allocator
 *
 * Manages the 208 KB page pool (52 × 4 KB pages) at 0x20004000–0x20037FFF.
 * Uses a free-stack (array-based LIFO) for O(1) alloc/free with no per-page
 * overhead.
 *
 * All pages are single-owner; no reference counting.  True CoW is not
 * feasible on the RP2040's 4-region MPU — see phase1-plan.md §Step 1.
 */

#ifndef PPAP_MM_PAGE_H
#define PPAP_MM_PAGE_H

#include <stdint.h>

/* ── Memory map constants (must match ppap.ld / qemu.ld) ──────────────────── */

#define PAGE_SIZE        4096u              /* bytes per page                  */
#define PAGE_COUNT         52u              /* pages in the pool               */

#define SRAM_KERNEL_BASE  0x20000000u       /* kernel data region start        */
#define SRAM_KERNEL_SIZE     (16u * 1024u)  /* 16 KB reserved for kernel       */
#define PAGE_POOL_BASE    0x20004000u       /* first page in the pool          */
#define PAGE_POOL_SIZE    (PAGE_COUNT * PAGE_SIZE)  /* 208 KB                  */
#define SRAM_IOBUF_BASE   0x20038000u       /* I/O buffer region               */
#define SRAM_IOBUF_SIZE      (24u * 1024u)  /* 24 KB                           */
#define SRAM_DMA_BASE     0x2003E000u       /* DMA / Core 1 region             */
#define SRAM_DMA_SIZE        (16u * 1024u)  /* 16 KB                           */

/* ── API ───────────────────────────────────────────────────────────────────── */

/* Initialise the page pool and print the boot-time memory map.
 * Must be called once from kmain(), after UART is ready. */
void mm_init(void);

/* Allocate one 4 KB page.  Returns a 4-KB-aligned pointer into the page pool,
 * or NULL if the pool is exhausted (OOM). */
void *page_alloc(void);

/* Return a page to the pool.  Behaviour is undefined if `page` was not
 * obtained from page_alloc(), or if it is freed more than once. */
void page_free(void *page);

/* Return the number of pages currently on the free stack. */
uint32_t page_free_count(void);

#endif /* PPAP_MM_PAGE_H */
