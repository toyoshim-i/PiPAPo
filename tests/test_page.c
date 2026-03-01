/*
 * test_page.c — Unit tests for src/kernel/mm/page.c
 *
 * page.c manages a fixed-address page pool (PAGE_POOL_BASE = 0x20004000 on
 * RP2040).  On the host, page_alloc() returns pointers with those RP2040
 * addresses — we NEVER dereference them; we only inspect the addresses and
 * test the allocator's bookkeeping.
 *
 * page.c calls uart_puts/uart_print_hex32/uart_print_dec and references the
 * linker symbols __bss_end/__stack_top; all are provided by stubs/uart_stub.c.
 * page.c internally exercises kmem (self-test in mm_init), so kmem.c is also
 * compiled into this test executable.
 */

#include "test_framework.h"
#include "kernel/mm/page.h"
#include <stdint.h>

/* Helpers */
static int addr_in_pool(void *p)
{
    uint32_t a = (uint32_t)(uintptr_t)p;
    return a >= PAGE_POOL_BASE && a < PAGE_POOL_BASE + PAGE_POOL_SIZE;
}

/* Reset the pool to a known-full state before each test group */
static void reset_pool(void) { mm_init(); }

/* ── Test cases ──────────────────────────────────────────────────────────── */

static void test_init_full_pool(void)
{
    reset_pool();
    ASSERT_EQ(page_free_count(), PAGE_COUNT);
}

static void test_alloc_returns_non_null(void)
{
    reset_pool();
    void *p = page_alloc();
    ASSERT_NOT_NULL(p);
}

static void test_alloc_returns_pool_address(void)
{
    reset_pool();
    void *p = page_alloc();
    ASSERT(addr_in_pool(p), "page address must be inside PAGE_POOL range");
}

static void test_alloc_is_page_aligned(void)
{
    reset_pool();
    void *p = page_alloc();
    uint32_t a = (uint32_t)(uintptr_t)p;
    ASSERT((a & (PAGE_SIZE - 1u)) == 0u, "page address must be PAGE_SIZE-aligned");
}

static void test_alloc_decrements_count(void)
{
    reset_pool();
    uint32_t before = page_free_count();
    page_alloc();
    ASSERT_EQ(page_free_count(), before - 1u);
}

static void test_alloc_sequential_distinct(void)
{
    reset_pool();
    void *a = page_alloc();
    void *b = page_alloc();
    ASSERT(a != b, "two allocations must return distinct pages");
}

static void test_alloc_sequential_non_overlapping(void)
{
    reset_pool();
    uint32_t a = (uint32_t)(uintptr_t)page_alloc();
    uint32_t b = (uint32_t)(uintptr_t)page_alloc();
    /* Pages must not overlap: difference is at least PAGE_SIZE */
    uint32_t diff = (a > b) ? (a - b) : (b - a);
    ASSERT(diff >= PAGE_SIZE, "consecutive pages must not overlap");
}

static void test_free_increments_count(void)
{
    reset_pool();
    void *p = page_alloc();
    uint32_t before = page_free_count();
    page_free(p);
    ASSERT_EQ(page_free_count(), before + 1u);
}

static void test_free_allows_realloc(void)
{
    reset_pool();
    void *p = page_alloc();
    page_free(p);
    void *q = page_alloc();
    ASSERT_NOT_NULL(q);
}

static void test_free_null_is_safe(void)
{
    reset_pool();
    uint32_t before = page_free_count();
    page_free(NULL);   /* must not crash or corrupt state */
    ASSERT_EQ(page_free_count(), before);
}

static void test_free_out_of_range_is_safe(void)
{
    reset_pool();
    uint32_t before = page_free_count();
    /* Pass a bogus address outside the pool — should be silently ignored */
    page_free((void *)0xDEADBEEFu);
    ASSERT_EQ(page_free_count(), before);
}

static void test_oom_returns_null(void)
{
    reset_pool();
    void *pages[PAGE_COUNT];
    for (uint32_t i = 0u; i < PAGE_COUNT; i++)
        pages[i] = page_alloc();

    ASSERT_EQ(page_free_count(), 0u);
    ASSERT_NULL(page_alloc());   /* pool exhausted */

    /* Restore: free all pages so subsequent tests start clean */
    for (uint32_t i = 0u; i < PAGE_COUNT; i++)
        page_free(pages[i]);
}

static void test_alloc_then_free_all(void)
{
    reset_pool();
    void *pages[PAGE_COUNT];
    for (uint32_t i = 0u; i < PAGE_COUNT; i++)
        pages[i] = page_alloc();

    for (uint32_t i = 0u; i < PAGE_COUNT; i++)
        page_free(pages[i]);

    ASSERT_EQ(page_free_count(), PAGE_COUNT);
    /* Pool must be fully usable again */
    ASSERT_NOT_NULL(page_alloc());
}

static void test_multiple_mm_init_resets_pool(void)
{
    mm_init();
    page_alloc();
    page_alloc();
    uint32_t after_alloc = page_free_count();
    mm_init();   /* second init must reset the free stack */
    ASSERT_EQ(page_free_count(), PAGE_COUNT);
    ASSERT(page_free_count() > after_alloc, "reset should restore full pool");
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== test_page ===\n");

    TEST_GROUP("Initialisation");
    RUN_TEST(test_init_full_pool);
    RUN_TEST(test_multiple_mm_init_resets_pool);

    TEST_GROUP("Allocation");
    RUN_TEST(test_alloc_returns_non_null);
    RUN_TEST(test_alloc_returns_pool_address);
    RUN_TEST(test_alloc_is_page_aligned);
    RUN_TEST(test_alloc_decrements_count);
    RUN_TEST(test_alloc_sequential_distinct);
    RUN_TEST(test_alloc_sequential_non_overlapping);

    TEST_GROUP("Free");
    RUN_TEST(test_free_increments_count);
    RUN_TEST(test_free_allows_realloc);
    RUN_TEST(test_free_null_is_safe);
    RUN_TEST(test_free_out_of_range_is_safe);

    TEST_GROUP("OOM and recovery");
    RUN_TEST(test_oom_returns_null);
    RUN_TEST(test_alloc_then_free_all);

    TEST_SUMMARY();
}
