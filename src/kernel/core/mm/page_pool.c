/*
 * page_pool.c — Pure page-allocator core
 *
 * See page_pool.h for the algorithm overview and invariants.  This file
 * contains the address-sorted free-list implementation with no external
 * dependencies — callers (page.c on target, the host test on dev machine)
 * serialize access themselves.
 */

#include "kernel/core/mm/page_pool.h"

#include <stdint.h>

#include "kernel/common/config.h" /* PAGE_COUNT_MAX */

/* True worst case: every single page is its own free block (all-one-page
 * holes, no coalescing).  With PAGE_COUNT_MAX slots we are guaranteed to
 * never drop a free — there is always room for the most fragmented state
 * the pool can reach.  Cost: 4 B/slot (e.g. 200 B BSS at
 * PAGE_COUNT_MAX=50, 16 KB at PAGE_COUNT_MAX=4096 on m68k), worth it for
 * the correctness guarantee. */
#define FREE_BLOCKS_MAX PAGE_COUNT_MAX

typedef struct {
  page_id_t base;
  uint16_t pages;
} free_block_t;

static free_block_t free_blocks[FREE_BLOCKS_MAX];
static uint32_t free_block_count;
static uint32_t free_page_total;

#define BLOCK_NONE ((uint32_t) - 1)

/* ── Array manipulation ──────────────────────────────────────────────────── */

static int block_insert_at(uint32_t idx, page_id_t base, uint16_t n) {
  if (free_block_count >= FREE_BLOCKS_MAX) return -1;
  for (uint32_t i = free_block_count; i > idx; i--)
    free_blocks[i] = free_blocks[i - 1];
  free_blocks[idx].base = base;
  free_blocks[idx].pages = n;
  free_block_count++;
  return 0;
}

static void block_remove(uint32_t idx) {
  for (uint32_t i = idx; i + 1 < free_block_count; i++)
    free_blocks[i] = free_blocks[i + 1];
  free_block_count--;
}

static uint32_t block_find_containing(page_id_t pid) {
  for (uint32_t i = 0; i < free_block_count; i++) {
    if (pid >= free_blocks[i].base &&
        pid < (page_id_t)(free_blocks[i].base + free_blocks[i].pages))
      return i;
  }
  return BLOCK_NONE;
}

/* First-fit: the lowest-address block with pages >= n.  Because the
 * array is sorted ascending by base, we can stop at the first hit. */
static uint32_t block_find_first_fit(uint16_t n) {
  for (uint32_t i = 0; i < free_block_count; i++) {
    if (free_blocks[i].pages >= n) return i;
  }
  return BLOCK_NONE;
}

static void block_take_front(uint32_t idx, uint16_t n) {
  if (n >= free_blocks[idx].pages) {
    block_remove(idx);
  } else {
    free_blocks[idx].base = (page_id_t)(free_blocks[idx].base + n);
    free_blocks[idx].pages -= n;
  }
}

/* Take the last page of the block at idx — used for single-page alloc to
 * keep scratch/stack pages clustered at the high end, leaving the low end
 * contiguous for proc brk growth. */
static page_id_t block_take_back_one(uint32_t idx) {
  page_id_t base = free_blocks[idx].base;
  uint16_t pages = free_blocks[idx].pages;
  page_id_t result = (page_id_t)((uint32_t)base + pages - 1);
  if (pages == 1) {
    block_remove(idx);
  } else {
    free_blocks[idx].pages = pages - 1;
  }
  return result;
}

static int block_insert_or_merge(page_id_t base, uint16_t n) {
  if (n == 0) return 0;
  uint32_t i = 0;
  while (i < free_block_count && free_blocks[i].base < base) i++;
  int merged_left = 0;
  if (i > 0 &&
      (page_id_t)(free_blocks[i - 1].base + free_blocks[i - 1].pages) == base) {
    uint32_t left = i - 1;
    free_blocks[left].pages += n;
    base = free_blocks[left].base;
    n = free_blocks[left].pages;
    merged_left = 1;
  }
  if (i < free_block_count && (page_id_t)(base + n) == free_blocks[i].base) {
    if (merged_left) {
      free_blocks[i - 1].pages += free_blocks[i].pages;
      block_remove(i);
    } else {
      free_blocks[i].base = base;
      free_blocks[i].pages += n;
    }
    return 0;
  }
  if (merged_left) return 0;
  return block_insert_at(i, base, n);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void pool_reset(void) {
  free_block_count = 0;
  free_page_total = 0;
}

int pool_add(page_id_t base, uint16_t n) {
  if (n == 0) return 0;
  if (free_block_count >= FREE_BLOCKS_MAX) return -1;
  free_blocks[free_block_count].base = base;
  free_blocks[free_block_count].pages = n;
  free_block_count++;
  free_page_total += n;
  return 0;
}

page_id_t pool_take_n(uint16_t n) {
  if (n == 0) return PAGE_ID_INVALID;

  /* Single-page allocs take the BACK of the HIGHEST-address block
   * (i.e. the absolute highest free pid).  Because free_blocks is
   * sorted ascending by base, the last entry is always the highest.
   * This mirrors the historical policy: single-page allocs (stacks,
   * scratch) cluster at the top of the pool, leaving the low end
   * contiguous so proc images can extend via sys_brk (which needs the
   * specific pages at `proc_base + i*PAGE_SIZE` to be free). */
  if (n == 1) {
    if (free_block_count == 0) return PAGE_ID_INVALID;
    uint32_t idx = free_block_count - 1;
    page_id_t id = block_take_back_one(idx);
    free_page_total -= 1;
    return id;
  }

  /* Multi-page contiguous allocs take the FRONT of the lowest block that
   * fits (first-fit-from-low).  This keeps large chunks like ELF images
   * and DOS segments anchored at the bottom of the pool. */
  uint32_t idx = block_find_first_fit(n);
  if (idx == BLOCK_NONE) return PAGE_ID_INVALID;
  page_id_t base = free_blocks[idx].base;
  block_take_front(idx, n);
  free_page_total -= n;
  return base;
}

page_id_t pool_take_largest(uint16_t min_pages, uint16_t max_pages,
                            uint16_t *got) {
  if (got) *got = 0;
  if (min_pages == 0 || max_pages < min_pages) return PAGE_ID_INVALID;

  uint32_t best = BLOCK_NONE;
  uint16_t best_size = 0;
  for (uint32_t i = 0; i < free_block_count; i++) {
    if (free_blocks[i].pages > best_size) {
      best = i;
      best_size = free_blocks[i].pages;
    }
  }
  if (best == BLOCK_NONE || best_size < min_pages) return PAGE_ID_INVALID;

  uint16_t take = (best_size > max_pages) ? max_pages : best_size;
  page_id_t base = free_blocks[best].base;
  block_take_front(best, take);
  free_page_total -= take;
  if (got) *got = take;
  return base;
}

int pool_take_at(page_id_t id) {
  uint32_t idx = block_find_containing(id);
  if (idx == BLOCK_NONE) return -1;
  free_block_t b = free_blocks[idx];
  block_remove(idx);
  if (id > b.base) {
    uint16_t pre = (uint16_t)(id - b.base);
    block_insert_or_merge(b.base, pre);
  }
  page_id_t post_base = (page_id_t)(id + 1);
  page_id_t b_end = (page_id_t)(b.base + b.pages);
  if (post_base < b_end) {
    uint16_t post = (uint16_t)(b_end - post_base);
    block_insert_or_merge(post_base, post);
  }
  free_page_total -= 1;
  return 0;
}

int pool_release(page_id_t base, uint16_t n) {
  if (n == 0) return 0;
  if (block_insert_or_merge(base, n) < 0) return -1;
  free_page_total += n;
  return 0;
}

uint32_t pool_free_total(void) { return free_page_total; }

uint16_t pool_max_contig(void) {
  uint16_t best = 0;
  for (uint32_t i = 0; i < free_block_count; i++) {
    if (free_blocks[i].pages > best) best = free_blocks[i].pages;
  }
  return best;
}

int pool_is_free(page_id_t id) {
  return block_find_containing(id) != BLOCK_NONE;
}

#if defined(PPAP_HOST_TEST)
uint32_t pool_block_count(void) { return free_block_count; }

void pool_block_get(uint32_t idx, page_id_t *base, uint16_t *pages) {
  if (idx >= free_block_count) {
    if (base) *base = PAGE_ID_INVALID;
    if (pages) *pages = 0;
    return;
  }
  if (base) *base = free_blocks[idx].base;
  if (pages) *pages = free_blocks[idx].pages;
}
#endif
