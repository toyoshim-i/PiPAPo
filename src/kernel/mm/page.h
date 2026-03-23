/*
 * page.h — Physical page allocator
 *
 * Manages the page pool starting at 0x20005000 (size set by PAGE_COUNT_MAX).
 * Uses a free-stack (array-based LIFO) for O(1) alloc/free with no per-page
 * overhead.
 *
 * All pages are single-owner; no reference counting.  True CoW is not
 * feasible on the RP2040's 4-region MPU — see phase01-plan.md §Step 1.
 */

#ifndef PPAP_KERNEL_MM_PAGE_H
#define PPAP_KERNEL_MM_PAGE_H

#include <stdint.h>

#include "config.h"

/* ── Memory map constants (must match the target linker script) ───────────────
 */
/* PAGE_SIZE and PAGE_COUNT_MAX are defined in config.h */

#if defined(__m68k__)
/* M68K: RAM at 0x0, page pool placed by linker after stack.
 * No IOBUF/DMA regions — those are RP2040-specific.
 *
 * RAM_END is the hard ceiling for the RAM probe — addresses at or above
 * this value are never probed (e.g. X68000 VRAM starts at 0xC00000).
 * Targets override via -DRAM_END=... in CMake; default = PAGE_POOL_BASE
 * + PAGE_COUNT_MAX * PAGE_SIZE (set after PAGE_POOL_BASE is known). */
#define SRAM_KERNEL_BASE 0x00000000u
#define SRAM_KERNEL_SIZE (20u * 1024u)
extern char __page_pool_start;
#define PAGE_POOL_BASE ((uint32_t)(uintptr_t)&__page_pool_start)
#define PAGE_POOL_SIZE (PAGE_COUNT_MAX * PAGE_SIZE)
#ifndef RAM_END
#define RAM_END (PAGE_POOL_BASE + PAGE_POOL_SIZE)
#endif
#elif defined(__xtensa__)
/* Xtensa / ESP32-S3: ESP-IDF manages the linker script.
 * PAGE_POOL_BASE and PAGE_COUNT_MAX are defined via CMake -D flags.
 * No IOBUF/DMA regions — ESP-IDF manages DMA buffers. */
#define SRAM_KERNEL_BASE 0x3FC90000u /* ESP32-S3 DRAM region start      */
#define SRAM_KERNEL_SIZE (32u * 1024u)
#define PAGE_POOL_SIZE (PAGE_COUNT_MAX * PAGE_SIZE)
/* Stub IOBUF/DMA to zero-size regions at end of page pool */
#define SRAM_IOBUF_BASE (PAGE_POOL_BASE + PAGE_POOL_SIZE)
#define SRAM_IOBUF_SIZE 0u
#define SRAM_DMA_BASE   SRAM_IOBUF_BASE
#define SRAM_DMA_SIZE   0u
#else
/* ARM / RP2040: SRAM layout defaults match pico1 / qemu_arm.
 * Targets with a different split (for example PicoCalc) override these via
 * target_compile_definitions(). */
#define SRAM_KERNEL_BASE 0x20000000u /* kernel data region start        */
#ifndef SRAM_KERNEL_SIZE
#define SRAM_KERNEL_SIZE (20u * 1024u) /* reserved for kernel .data/.bss  */
#endif
#ifndef PAGE_POOL_BASE
#define PAGE_POOL_BASE 0x20005000u /* first page in the pool          */
#endif
#define PAGE_POOL_SIZE (PAGE_COUNT_MAX * PAGE_SIZE)
#define SRAM_IOBUF_BASE (PAGE_POOL_BASE + PAGE_POOL_SIZE) /* after pool    */
#define SRAM_IOBUF_SIZE (24u * 1024u) /* 24 KB                           */
#define SRAM_DMA_BASE (SRAM_IOBUF_BASE + SRAM_IOBUF_SIZE)
#define SRAM_DMA_SIZE (16u * 1024u) /* 16 KB                           */
#endif

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
 * Used by do_execve() to place user code at its linked address. */
void *page_alloc_at(void *addr);

/* Return a page to the pool.  Behaviour is undefined if `page` was not
 * obtained from page_alloc(), or if it is freed more than once. */
void page_free(void *page);

/* Free a user-process page.  On Xtensa, user code lives in IRAM
 * (allocated via heap_caps_malloc); detect by address range and call
 * heap_caps_free instead of page_free. */
static inline void user_page_free(void *page) {
#if defined(__xtensa__)
  uint32_t addr = (uint32_t)(uintptr_t)page;
  if (addr >= 0x40370000u && addr < 0x403E0000u) {
    extern void heap_caps_free(void *ptr);
    heap_caps_free(page);
    return;
  }
#endif
  page_free(page);
}

/* Return the number of pages currently on the free stack. */
uint32_t page_free_count(void);

/* Return the length (in pages) of the largest contiguous free run.
 * Used by _MALLOC to report accurate availability. */
uint32_t page_max_contiguous(void);

/* Allocate n_pages contiguous pages.  Returns a pointer to the first page,
 * or NULL if no contiguous run of that size is available.
 * Uses a bitmap scan — O(page_count), safe for large pools. */
uint8_t *page_alloc_contiguous(uint32_t n_pages);

/* Runtime page count — set by mm_init() after probing available RAM.
 * Always <= PAGE_COUNT_MAX.  Use this (not PAGE_COUNT_MAX) for runtime
 * decisions such as total-memory reporting. */
extern uint32_t page_count;

/* OOM event counter (incremented each time page_alloc returns NULL). */
extern uint32_t oom_count;

#endif /* PPAP_KERNEL_MM_PAGE_H */
