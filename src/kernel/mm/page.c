/*
 * page.c — Physical page allocator
 *
 * Free-stack implementation: a fixed array of PAGE_COUNT void* pointers,
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
#include "xip.h"
#include "drivers/uart.h"
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

static void    *free_stack[PAGE_COUNT];
static uint32_t free_top = 0u;   /* index of next empty slot (0 = pool empty) */

/* ── Internal helpers ─────────────────────────────────────────────────────── */

/* Print "NNN KB" where NNN = bytes / 1024 */
static void print_kb(uint32_t bytes)
{
    uart_print_dec(bytes / 1024u);
    uart_puts(" KB");
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void mm_init(void)
{
    uint32_t bss_end   = (uint32_t)(uintptr_t)&__bss_end;
    uint32_t stack_top = (uint32_t)(uintptr_t)&__stack_top;
    uint32_t kern_used = bss_end - SRAM_KERNEL_BASE;

    /* Build the free stack: push pages from the pool that don't overlap
     * with the kernel stack.  On QEMU (flat memory model) the initial
     * stack may extend into the page pool region.  Skip those pages. */
    uint32_t stack_page_top = (stack_top + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    free_top = 0;
    for (uint32_t i = 0u; i < PAGE_COUNT; i++) {
        uint32_t paddr = PAGE_POOL_BASE + i * PAGE_SIZE;
        if (paddr < stack_page_top)
            continue;   /* overlaps with kernel stack */
        free_stack[free_top++] = (void *)(uintptr_t)paddr;
    }

    /* ── Boot-time memory map ─────────────────────────────────────────────── */
    uart_puts("MM: SRAM memory map\n");

    /* Kernel region */
    uart_puts("MM:   kernel  ");
    uart_print_hex32(SRAM_KERNEL_BASE);
    uart_puts("–");
    uart_print_hex32(SRAM_KERNEL_BASE + SRAM_KERNEL_SIZE - 1u);
    uart_puts("  ");
    print_kb(SRAM_KERNEL_SIZE);
    uart_puts(" reserved\n");

    /* Kernel data usage */
    uart_puts("MM:     .data/.bss:  ");
    uart_print_hex32(kern_used);
    uart_puts(" B used");
    if (stack_top > bss_end) {
        uart_puts(", ");
        uart_print_hex32(stack_top - bss_end);
        uart_puts(" B to stack top\n");
    } else {
        uart_puts("\n");
    }

    /* Page pool — show actual usable start (may skip kernel-stack overlap) */
    uint32_t actual_base = (free_top > 0)
        ? (uint32_t)(uintptr_t)free_stack[0] : PAGE_POOL_BASE;
    uart_puts("MM:   pages   ");
    uart_print_hex32(actual_base);
    uart_puts("–");
    uart_print_hex32(PAGE_POOL_BASE + PAGE_POOL_SIZE - 1u);
    uart_puts(" ");
    print_kb(free_top * PAGE_SIZE);
    uart_puts(" (");
    uart_print_dec(free_top);
    uart_puts(" × 4 KB, all free)\n");

    /* I/O buffer */
    uart_puts("MM:   io_buf  ");
    uart_print_hex32(SRAM_IOBUF_BASE);
    uart_puts("–");
    uart_print_hex32(SRAM_IOBUF_BASE + SRAM_IOBUF_SIZE - 1u);
    uart_puts("  ");
    print_kb(SRAM_IOBUF_SIZE);
    uart_puts("\n");

    /* DMA / Core 1 */
    uart_puts("MM:   dma     ");
    uart_print_hex32(SRAM_DMA_BASE);
    uart_puts("–");
    uart_print_hex32(SRAM_DMA_BASE + SRAM_DMA_SIZE - 1u);
    uart_puts("  ");
    print_kb(SRAM_DMA_SIZE);
    uart_puts("\n");

#ifdef PPAP_TESTS
    /* ── kmem self-test ───────────────────────────────────────────────────── */
    /* Exercise the pool mechanism with a tiny local buffer before any real
     * pool (PCBs, etc.) is created.  Validates alloc, free, and free_count. */
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

    uart_puts("MM: kmem self-test ");
    uart_puts(ok ? "PASSED\n" : "FAILED\n");
#endif

#ifdef PPAP_TESTS
    /* ── XIP verification and benchmark ──────────────────────────────────── */
    xip_verify();
#endif
}

void *page_alloc(void)
{
    if (free_top == 0u)
        return NULL;   /* OOM */
    return free_stack[--free_top];
}

void *page_alloc_at(void *addr)
{
    uint32_t target = (uint32_t)(uintptr_t)addr;

    /* Validate: must be page-aligned and within the pool */
    if (target < PAGE_POOL_BASE || target >= PAGE_POOL_BASE + PAGE_POOL_SIZE)
        return NULL;
    if (target & (PAGE_SIZE - 1u))
        return NULL;

    /* Scan the free stack for the requested page */
    for (uint32_t i = 0u; i < free_top; i++) {
        if (free_stack[i] == addr) {
            /* Remove by swapping with the top element */
            free_top--;
            free_stack[i] = free_stack[free_top];
            return addr;
        }
    }

    return NULL;   /* already allocated */
}

void page_free(void *page)
{
    /* Rudimentary double-free / out-of-range guard */
    uint32_t addr = (uint32_t)(uintptr_t)page;
    if (addr < PAGE_POOL_BASE || addr >= PAGE_POOL_BASE + PAGE_POOL_SIZE)
        return;   /* ignore bogus pointer rather than corrupt the stack */
    if (free_top < PAGE_COUNT)
        free_stack[free_top++] = page;
}

uint32_t page_free_count(void)
{
    return free_top;
}
