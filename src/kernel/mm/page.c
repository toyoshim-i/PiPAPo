/*
 * page.c — Physical page allocator
 *
 * Free-stack implementation: a fixed array of PAGE_COUNT_MAX void* pointers,
 * managed as a LIFO stack.  Alloc pops the top; free pushes onto the top.
 * Both operations are O(1) with no heap or dynamic storage needed.
 *
 * mm_init() also prints the boot-time memory map, showing:
 *   - actual kernel data usage (measured from __bss_end at link time)
 *   - page pool, I/O buffer, and DMA/Core1 region sizes
 * This lets you track how much kernel data grows as Phase 1+ code is added.
 */

#include "page.h"
#include "kmem.h"
#if !defined(__m68k__)
#include "xip.h"
#endif
#include "../spinlock.h"
#include "../klog.h"
#include <stddef.h>

/* ── Linker-provided symbols ──────────────────────────────────────────────── */
/*
 * These are section boundary symbols defined by the linker script.
 * In C, a linker symbol `foo` is accessed as `&foo` — the *address* of the
 * symbol IS its value (i.e. the memory address it marks).
 *
 * __bss_end:  first byte after the .bss section (= first free byte of
 *             kernel data region after zeroing).
 * __stack_top: top of the initial kernel stack (= end of RAM_KERNEL region).
 */
extern char __bss_end;
extern char __stack_top;

/* ── Free-stack state ─────────────────────────────────────────────────────── */

static void    *free_stack[PAGE_COUNT_MAX];
static uint32_t free_top = 0u;   /* index of next empty slot (0 = pool empty) */
uint32_t        page_count = 0u; /* runtime page count (set by mm_init)       */
uint32_t        oom_count = 0u;  /* number of page_alloc() failures */

#if defined(__m68k__)
/* Assembly RAM probe — detects accessible RAM via bus error catching.
 * Returns total accessible bytes starting from `start`, up to `max_size`. */
extern uint32_t m68k_probe_ram(uint32_t start, uint32_t max_size);
#endif

/* ── Internal helpers ─────────────────────────────────────────────────────── */

/* ── Public API ───────────────────────────────────────────────────────────── */

void mm_init(void)
{
    uint32_t bss_end   = (uint32_t)(uintptr_t)&__bss_end;
    uint32_t stack_top = (uint32_t)(uintptr_t)&__stack_top;
    uint32_t kern_used = bss_end - SRAM_KERNEL_BASE;

    /* Detect available RAM and set runtime page_count.
     * On m68k: probe RAM via bus error catching (two-phase: 1MB then 4KB).
     * On ARM:  fixed RAM size, use compile-time PAGE_COUNT_MAX directly. */
#if defined(__m68k__)
    {
        uint32_t probe_end = RAM_END;
        uint32_t max_bytes = probe_end - PAGE_POOL_BASE;
        uint32_t ram_bytes = m68k_probe_ram(PAGE_POOL_BASE, max_bytes);
        if (ram_bytes > max_bytes)
            ram_bytes = max_bytes;
        page_count = ram_bytes / PAGE_SIZE;
        if (page_count > PAGE_COUNT_MAX)
            page_count = PAGE_COUNT_MAX;
    }
#else
    page_count = PAGE_COUNT_MAX;
#endif

    /* Build the free stack: push pages from the pool that don't overlap
     * with the kernel stack.  On QEMU (flat memory model) the initial
     * stack may extend into the page pool region.  Skip those pages. */
    uint32_t stack_page_top = (stack_top + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    free_top = 0;
    for (uint32_t i = 0u; i < page_count; i++) {
        uint32_t paddr = PAGE_POOL_BASE + i * PAGE_SIZE;
        if (paddr < stack_page_top)
            continue;   /* overlaps with kernel stack */
        free_stack[free_top++] = (void *)(uintptr_t)paddr;
    }

    /* ── Boot-time memory map ─────────────────────────────────────────────── */
    klog("MM: SRAM memory map\n");
    klogf("MM:   kernel  %x–%x  %u KB reserved\n",
          SRAM_KERNEL_BASE, SRAM_KERNEL_BASE + SRAM_KERNEL_SIZE - 1u,
          SRAM_KERNEL_SIZE / 1024u);
    if (stack_top > bss_end)
        klogf("MM:     .data/.bss:  %x B used, %x B to stack top\n",
              kern_used, stack_top - bss_end);
    else
        klogf("MM:     .data/.bss:  %x B used\n", kern_used);

    uint32_t actual_base = (free_top > 0)
        ? (uint32_t)(uintptr_t)free_stack[0] : PAGE_POOL_BASE;
    klogf("MM:   pages   %x–%x %u KB (%u × 4 KB, all free)\n",
          actual_base, PAGE_POOL_BASE + page_count * PAGE_SIZE - 1u,
          free_top * PAGE_SIZE / 1024u, free_top);
#if !defined(__m68k__)
    klogf("MM:   io_buf  %x–%x  %u KB\n",
          SRAM_IOBUF_BASE, SRAM_IOBUF_BASE + SRAM_IOBUF_SIZE - 1u,
          SRAM_IOBUF_SIZE / 1024u);
    klogf("MM:   dma     %x–%x  %u KB\n",
          SRAM_DMA_BASE, SRAM_DMA_BASE + SRAM_DMA_SIZE - 1u,
          SRAM_DMA_SIZE / 1024u);
#endif

#ifdef PPAP_TESTS
    /* ── kmem self-test ───────────────────────────────────────────────────── */
    typedef struct { uint32_t a; uint32_t b; } test_obj_t;
    static uint8_t       test_mem[4 * sizeof(test_obj_t)];
    static kmem_pool_t   test_pool;
    kmem_pool_init(&test_pool, test_mem, sizeof(test_obj_t), 4u);

    test_obj_t *o1 = kmem_alloc(&test_pool);   /* 3 free */
    test_obj_t *o2 = kmem_alloc(&test_pool);   /* 2 free */
    kmem_free(&test_pool, o1);                  /* 3 free */
    test_obj_t *o3 = kmem_alloc(&test_pool);   /* 2 free */
    (void)o2; (void)o3;

    uint32_t ok = (o1 != NULL) && (o2 != NULL) && (o3 != NULL)
               && (kmem_free_count(&test_pool) == 2u);

    klogf("MM: kmem self-test %s\n", ok ? "PASSED" : "FAILED");
#endif

#if defined(PPAP_TESTS) && !defined(__m68k__)
    /* ── XIP verification and benchmark (ARM only) ───────────────────────── */
    xip_verify();
#endif
}

void *page_alloc(void)
{
    uint32_t saved = spin_lock_irqsave(SPIN_PAGE);
    void *p = NULL;
    if (free_top != 0u) {
        p = free_stack[--free_top];
    } else {
        oom_count++;
    }
    spin_unlock_irqrestore(SPIN_PAGE, saved);
    if (!p)
        klog("MM: OOM — page_alloc failed\n");
    return p;
}

void *page_alloc_at(void *addr)
{
    uint32_t target = (uint32_t)(uintptr_t)addr;

    /* Validate: must be page-aligned and within the runtime pool */
    if (target < PAGE_POOL_BASE || target >= PAGE_POOL_BASE + page_count * PAGE_SIZE)
        return NULL;
    if (target & (PAGE_SIZE - 1u))
        return NULL;

    uint32_t saved = spin_lock_irqsave(SPIN_PAGE);
    void *result = NULL;

    /* Scan the free stack for the requested page */
    for (uint32_t i = 0u; i < free_top; i++) {
        if (free_stack[i] == addr) {
            /* Remove by swapping with the top element */
            free_top--;
            free_stack[i] = free_stack[free_top];
            result = addr;
            break;
        }
    }

    spin_unlock_irqrestore(SPIN_PAGE, saved);
    return result;
}

void page_free(void *page)
{
    /* Rudimentary double-free / out-of-range guard */
    uint32_t addr = (uint32_t)(uintptr_t)page;
    if (addr < PAGE_POOL_BASE || addr >= PAGE_POOL_BASE + page_count * PAGE_SIZE)
        return;   /* ignore bogus pointer rather than corrupt the stack */

    uint32_t saved = spin_lock_irqsave(SPIN_PAGE);

    /* Scan for double-free: O(free_top) ≈ O(51) at 133 MHz ≈ 1 µs */
    for (uint32_t i = 0u; i < free_top; i++) {
        if (free_stack[i] == page) {
            spin_unlock_irqrestore(SPIN_PAGE, saved);
            klogf("MM: double-free detected @ %x\n", addr);
            return;
        }
    }

    if (free_top < page_count)
        free_stack[free_top++] = page;
    spin_unlock_irqrestore(SPIN_PAGE, saved);
}

uint32_t page_free_count(void)
{
    uint32_t saved = spin_lock_irqsave(SPIN_PAGE);
    uint32_t count = free_top;
    spin_unlock_irqrestore(SPIN_PAGE, saved);
    return count;
}

/* ── Bitmap helpers for contiguous allocation ────────────────────────────── */

/* Build a bitmap of free pages: bit i set = page i is free.
 * Must be called with SPIN_PAGE held. */
static void build_free_bitmap(uint32_t *bmap, uint32_t n_words)
{
    for (uint32_t w = 0; w < n_words; w++)
        bmap[w] = 0;
    for (uint32_t i = 0; i < free_top; i++) {
        uint32_t idx = ((uint32_t)(uintptr_t)free_stack[i] - PAGE_POOL_BASE)
                       / PAGE_SIZE;
        if (idx < page_count)
            bmap[idx / 32] |= (1u << (idx % 32));
    }
}

static int bmap_test(const uint32_t *bmap, uint32_t idx)
{
    return (bmap[idx / 32] >> (idx % 32)) & 1;
}

uint32_t page_max_contiguous(void)
{
    uint32_t saved = spin_lock_irqsave(SPIN_PAGE);
    uint32_t n_words = (page_count + 31) / 32;
    uint32_t bmap[n_words];
    build_free_bitmap(bmap, n_words);

    uint32_t best = 0, run = 0;
    for (uint32_t i = 0; i < page_count; i++) {
        if (bmap_test(bmap, i)) {
            run++;
            if (run > best)
                best = run;
        } else {
            run = 0;
        }
    }

    spin_unlock_irqrestore(SPIN_PAGE, saved);
    return best;
}

/* Allocate n_pages contiguous pages.  Uses a bitmap scan to find a run,
 * then removes the pages from the free stack. O(page_count). */
uint8_t *page_alloc_contiguous(uint32_t n_pages)
{
    if (n_pages == 0)
        return NULL;
    if (n_pages == 1)
        return (uint8_t *)page_alloc();

    uint32_t saved = spin_lock_irqsave(SPIN_PAGE);

    /* Build bitmap of free pages */
    uint32_t n_words = (page_count + 31) / 32;
    uint32_t bmap[n_words];
    build_free_bitmap(bmap, n_words);

    /* Scan for a contiguous run of n_pages free pages */
    uint32_t run_start = 0, run_len = 0;
    uint32_t found_start = 0;
    int found = 0;

    for (uint32_t i = 0; i < page_count; i++) {
        if (bmap_test(bmap, i)) {
            if (run_len == 0)
                run_start = i;
            run_len++;
            if (run_len >= n_pages) {
                found_start = run_start;
                found = 1;
                break;
            }
        } else {
            run_len = 0;
        }
    }

    if (!found) {
        spin_unlock_irqrestore(SPIN_PAGE, saved);
        return NULL;
    }

    /* Remove the found pages from the free stack.
     * Mark which pages to remove, then compact the stack. */
    uint32_t base_addr = PAGE_POOL_BASE + found_start * PAGE_SIZE;
    uint32_t end_addr  = base_addr + n_pages * PAGE_SIZE;
    uint32_t new_top = 0;
    for (uint32_t i = 0; i < free_top; i++) {
        uint32_t pa = (uint32_t)(uintptr_t)free_stack[i];
        if (pa >= base_addr && pa < end_addr) {
            /* This page is being allocated — skip it */
        } else {
            free_stack[new_top++] = free_stack[i];
        }
    }
    free_top = new_top;

    spin_unlock_irqrestore(SPIN_PAGE, saved);
    return (uint8_t *)(uintptr_t)base_addr;
}
