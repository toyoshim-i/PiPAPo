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

    /* Build the free stack: push all pages from the pool, lowest address first
     * so page_alloc() returns the lowest-addressed page first (predictable). */
    for (uint32_t i = 0u; i < PAGE_COUNT; i++) {
        free_stack[i] = (void *)(uintptr_t)(PAGE_POOL_BASE + i * PAGE_SIZE);
    }
    free_top = PAGE_COUNT;

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

    /* Page pool */
    uart_puts("MM:   pages   ");
    uart_print_hex32(PAGE_POOL_BASE);
    uart_puts("–");
    uart_print_hex32(PAGE_POOL_BASE + PAGE_POOL_SIZE - 1u);
    uart_puts(" ");
    print_kb(PAGE_POOL_SIZE);
    uart_puts(" (");
    uart_print_dec(PAGE_COUNT);
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
}

void *page_alloc(void)
{
    if (free_top == 0u)
        return NULL;   /* OOM */
    return free_stack[--free_top];
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
