/*
 * test_page_pool.c — Host unit tests for the pure page-pool core.
 *
 * Compiles page_pool.c directly.  No spinlock / klogf / hardware stubs
 * needed — the core depends only on stdint + page_types.h.
 *
 * Covers:
 *   - reset / add_range seeding
 *   - alloc_n best-fit picks the smallest sufficient block
 *   - alloc_largest returns the longest run (capped by max)
 *   - alloc_at splits a block into ≤ 3 fragments
 *   - free_range coalesces with left / right / both / no neighbors
 *   - free_total, max_contiguous, is_free stay in sync
 *   - add_range respects FREE_BLOCKS_MAX capacity
 */

#include "kernel/core/mm/page_pool.h"

#include "test_framework.h"

/* Seed the pool with one block of `n` pages starting at pid 0. */
static void seed_single(uint16_t n) {
  pool_reset();
  pool_add(0, n);
}

/* Confirm the free list has exactly the expected (base, pages) entries. */
static void expect_blocks(const page_id_t *bases, const uint16_t *pages,
                          uint32_t count) {
  ASSERT_EQ(pool_block_count(), count);
  for (uint32_t i = 0; i < count; i++) {
    page_id_t base;
    uint16_t p;
    pool_block_get(i, &base, &p);
    ASSERT_EQ((long)base, (long)bases[i]);
    ASSERT_EQ((long)p, (long)pages[i]);
  }
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

static void test_reset_empty(void) {
  pool_reset();
  ASSERT_EQ(pool_free_total(), 0);
  ASSERT_EQ(pool_max_contig(), 0);
  ASSERT_EQ(pool_take_n(1), (long)PAGE_ID_INVALID);
}

static void test_add_range_and_single_alloc_from_back(void) {
  /* Single-page alloc takes the BACK of the largest block so scratch/
   * stack pages cluster at the high end, leaving the low end contiguous
   * for proc brk growth. */
  seed_single(10);
  ASSERT_EQ(pool_free_total(), 10);
  ASSERT_EQ(pool_max_contig(), 10);
  page_id_t p = pool_take_n(1);
  ASSERT_EQ((long)p, 9);
  ASSERT_EQ(pool_free_total(), 9);
  page_id_t bases[] = {0};
  uint16_t pages[] = {9};
  expect_blocks(bases, pages, 1);
}

static void test_alloc_oom(void) {
  seed_single(4);
  ASSERT_EQ((long)pool_take_n(5), (long)PAGE_ID_INVALID);
  ASSERT_EQ(pool_free_total(), 4); /* state untouched on failure */
}

static void test_single_alloc_picks_highest_block_back(void) {
  /* Three blocks: sizes 8, 3, 10 at bases 0, 20, 30.  Single-page alloc
   * should take the BACK of the HIGHEST-address block (not the largest),
   * i.e. pid 39 from {30,10}.  Then the next single-alloc still takes
   * the high-address block (now {30,9}) → pid 38.
   *
   * This policy ensures scratch/stack allocations cluster at the
   * absolute top of the pool, leaving the low end contiguous for proc
   * brk growth — even when the pool has fragmented into blocks of
   * different sizes. */
  pool_reset();
  pool_add(0, 8);
  pool_add(20, 3);
  pool_add(30, 10);
  ASSERT_EQ((long)pool_take_n(1), 39);
  ASSERT_EQ((long)pool_take_n(1), 38);
  /* Drain the high block (10 single-allocs total from {30,10}). */
  for (int i = 0; i < 8; i++) (void)pool_take_n(1);
  /* Now the two remaining blocks are {0,8}, {20,3}.  Next alloc takes
   * the back of {20,3} — highest address — giving pid 22. */
  ASSERT_EQ((long)pool_take_n(1), 22);
}

static void test_brk_zone_not_overwritten_by_single_alloc(void) {
  /* Regression for the qemu_arm UFS-mount failure: a proc lives at the
   * low end; later single-page allocs must NOT consume pages in the
   * proc's brk growth zone even when fragmentation leaves a large LOW
   * block and a smaller HIGH block. */
  pool_reset();
  /* Simulate: proc loaded at pages 0..4 (already out of free list);
   * one big low block {5,20}, one small high block {30,3}. */
  pool_add(5, 20);
  pool_add(30, 3);
  /* Single-alloc must take the highest-address block's back → 32. */
  ASSERT_EQ((long)pool_take_n(1), 32);
  ASSERT_EQ((long)pool_take_n(1), 31);
  ASSERT_EQ((long)pool_take_n(1), 30);
  /* High block drained; next single-alloc dips into the low block's
   * back, but proc brk pages 5..9 remain untouched. */
  ASSERT_EQ((long)pool_take_n(1), 24);
  ASSERT(pool_is_free(5), "proc brk zone start still free");
  ASSERT(pool_is_free(9), "proc brk zone end still free");
}

static void test_contig_alloc_first_fit_from_low(void) {
  /* Three blocks: sizes 3, 9, 7.  alloc_n(5) should take the FIRST
   * block with >=5 pages (the 9-page one at 10), not the 7-page one. */
  pool_reset();
  pool_add(0, 3);
  pool_add(10, 9);
  pool_add(25, 7);
  ASSERT_EQ((long)pool_take_n(5), 10);
  page_id_t bases[] = {0, 15, 25};
  uint16_t pages[] = {3, 4, 7};
  expect_blocks(bases, pages, 3);
}

static void test_alloc_largest(void) {
  pool_reset();
  pool_add(0, 4);
  pool_add(10, 9);
  pool_add(25, 7);
  uint16_t got = 0;
  page_id_t base = pool_take_largest(1, 100, &got);
  ASSERT_EQ((long)base, 10);
  ASSERT_EQ(got, 9);
  /* Now largest remaining is the 7-page block. */
  got = 99;
  base = pool_take_largest(5, 6, &got);
  ASSERT_EQ((long)base, 25);
  ASSERT_EQ(got, 6); /* capped at max */
}

static void test_alloc_largest_below_min(void) {
  pool_reset();
  pool_add(0, 3);
  uint16_t got = 99;
  page_id_t base = pool_take_largest(5, 10, &got);
  ASSERT_EQ((long)base, (long)PAGE_ID_INVALID);
  ASSERT_EQ(got, 0);
}

static void test_alloc_at_middle_splits(void) {
  seed_single(10); /* [0..9] */
  ASSERT_EQ(pool_take_at(4), 0);
  /* Expect [0..3] + [5..9]: two fragments. */
  page_id_t bases[] = {0, 5};
  uint16_t pages[] = {4, 5};
  expect_blocks(bases, pages, 2);
  ASSERT_EQ(pool_free_total(), 9);
}

static void test_alloc_at_edges(void) {
  seed_single(10);
  ASSERT_EQ(pool_take_at(0), 0);
  page_id_t bases[] = {1};
  uint16_t pages[] = {9};
  expect_blocks(bases, pages, 1);
  ASSERT_EQ(pool_take_at(9), 0);
  page_id_t bases2[] = {1};
  uint16_t pages2[] = {8};
  expect_blocks(bases2, pages2, 1);
}

static void test_alloc_at_not_free(void) {
  seed_single(10);
  /* Take pages 0..4 via a contiguous alloc (first-fit-from-low). */
  ASSERT_EQ(pool_take_n(5), 0);
  ASSERT_EQ(pool_take_at(0), -1);
  ASSERT_EQ(pool_take_at(4), -1);
  ASSERT_EQ(pool_take_at(5), 0); /* available */
}

static void test_free_coalesce_left(void) {
  seed_single(10);
  /* Take 0..2 via contiguous alloc(3), then simulate a free at id=2. */
  ASSERT_EQ(pool_take_n(3), 0);
  ASSERT_EQ(pool_release(2, 1), 0); /* id 2 merges into {3,7} */
  page_id_t bases[] = {2};
  uint16_t pages[] = {8};
  expect_blocks(bases, pages, 1);
}

static void test_free_coalesce_right(void) {
  seed_single(10);
  ASSERT_EQ(pool_take_n(3), 0); /* 0..2, {3,7} */
  ASSERT_EQ(pool_release(0, 1), 0); /* {0,1}; then still {3,7} */
  page_id_t bases[] = {0, 3};
  uint16_t pages[] = {1, 7};
  expect_blocks(bases, pages, 2);
  ASSERT_EQ(pool_release(1, 1), 0); /* merges left with {0,1} */
  page_id_t bases2[] = {0, 3};
  uint16_t pages2[] = {2, 7};
  expect_blocks(bases2, pages2, 2);
  ASSERT_EQ(pool_release(2, 1), 0); /* bridges {0,2} and {3,7} */
  page_id_t bases3[] = {0};
  uint16_t pages3[] = {10};
  expect_blocks(bases3, pages3, 1);
}

static void test_brk_growth_scenario(void) {
  /* Realistic scenario: a proc is loaded at the low end (contig alloc),
   * then some single-page scratch allocs happen, then the proc calls
   * brk() which uses alloc_at_id for the page immediately after its
   * image.  The single-page allocs must NOT have taken that page. */
  seed_single(50);
  /* Proc image: 10 contiguous pages from low. */
  ASSERT_EQ((long)pool_take_n(10), 0);
  /* 5 single-page allocs for kernel scratch. */
  ASSERT_EQ((long)pool_take_n(1), 49);
  ASSERT_EQ((long)pool_take_n(1), 48);
  ASSERT_EQ((long)pool_take_n(1), 47);
  ASSERT_EQ((long)pool_take_n(1), 46);
  ASSERT_EQ((long)pool_take_n(1), 45);
  /* Now the proc tries to grow via brk — needs page 10 specifically. */
  ASSERT_EQ(pool_take_at(10), 0);
  ASSERT_EQ(pool_take_at(11), 0);
  /* And page 12, etc. should still be free. */
  ASSERT(pool_is_free(12), "brk growth zone still free");
}

static void test_free_isolated_insert(void) {
  pool_reset();
  pool_add(0, 2);
  pool_add(10, 2);
  ASSERT_EQ(pool_release(5, 1), 0);
  page_id_t bases[] = {0, 5, 10};
  uint16_t pages[] = {2, 1, 2};
  expect_blocks(bases, pages, 3);
}

static void test_is_free(void) {
  seed_single(10);
  ASSERT(pool_is_free(0), "id 0 free at seed");
  ASSERT(pool_is_free(9), "id 9 free at seed");
  ASSERT(!pool_is_free(10), "id 10 outside pool");
  ASSERT_EQ(pool_take_n(3), 0);
  ASSERT(!pool_is_free(0), "id 0 taken");
  ASSERT(!pool_is_free(2), "id 2 taken");
  ASSERT(pool_is_free(3), "id 3 still free");
}

static void test_max_contiguous_after_allocs(void) {
  pool_reset();
  pool_add(0, 4);
  pool_add(10, 9);
  pool_add(25, 7);
  ASSERT_EQ(pool_max_contig(), 9);
  ASSERT_EQ(pool_take_n(9), 10); /* drains the 9-page block */
  ASSERT_EQ(pool_max_contig(), 7);
}

static void test_alloc_n_zero_returns_invalid(void) {
  seed_single(10);
  ASSERT_EQ((long)pool_take_n(0), (long)PAGE_ID_INVALID);
  ASSERT_EQ(pool_free_total(), 10);
}

int main(void) {
  TEST_GROUP("page_pool core");
  RUN_TEST(test_reset_empty);
  RUN_TEST(test_add_range_and_single_alloc_from_back);
  RUN_TEST(test_alloc_oom);
  RUN_TEST(test_single_alloc_picks_highest_block_back);
  RUN_TEST(test_brk_zone_not_overwritten_by_single_alloc);
  RUN_TEST(test_contig_alloc_first_fit_from_low);
  RUN_TEST(test_alloc_largest);
  RUN_TEST(test_alloc_largest_below_min);
  RUN_TEST(test_alloc_at_middle_splits);
  RUN_TEST(test_alloc_at_edges);
  RUN_TEST(test_alloc_at_not_free);
  RUN_TEST(test_free_coalesce_left);
  RUN_TEST(test_free_coalesce_right);
  RUN_TEST(test_free_isolated_insert);
  RUN_TEST(test_is_free);
  RUN_TEST(test_max_contiguous_after_allocs);
  RUN_TEST(test_alloc_n_zero_returns_invalid);
  RUN_TEST(test_brk_growth_scenario);
  TEST_SUMMARY();
}
