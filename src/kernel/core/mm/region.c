/*
 * region.c — Typed class-dispatched mm allocation
 *
 * Generic implementation.  All target-specific behaviour (extra
 * arenas, dual-mapping checks, alternative allocators) lives behind
 * the mem_helper hooks in src/arch/<arch>/kernel/core/mem_helper.c,
 * so this file has zero architecture conditionals.
 */

#include "kernel/core/mm/region.h"

#include "common/errno.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/common/spinlock.h"
#include "kernel/core/mm/mem_helper.h"
#include "kernel/core/mm/page.h"

static uint32_t region_page_count(uint32_t size) {
  return (size + PAGE_SIZE - 1u) / PAGE_SIZE;
}

static uint32_t region_page_pool_total_bytes(void) {
  return page_count * PAGE_SIZE;
}

static uint32_t region_page_pool_free_bytes(void) {
  return page_free_count() * PAGE_SIZE;
}

static uint32_t region_page_pool_largest_free_bytes(void) {
  return page_max_contiguous() * PAGE_SIZE;
}

int region_init(void) { return mem_helper_init_arenas(); }

static int region_alloc_page_backed(uint32_t size, region_t *out) {
  if (size == 0) {
    out->base = NULL;
    out->base_page = PAGE_ID_INVALID;
    return 0;
  }

  /* Allocate by page_id so we can populate out->base_page directly.
   * On i16 a void * cast of the linear address truncates to 16 bits,
   * which loses any page above the first 64 KB.  Callers reach the
   * payload via page_read / page_write through base_page, not by
   * dereferencing base. */
  uint32_t n_pages = region_page_count(size);
  page_id_t pid = (n_pages == 1) ? page_alloc() : page_alloc_n(n_pages);
  if (pid == PAGE_ID_INVALID) return -(int)ENOMEM;

  out->base = (void *)(uintptr_t)page_linear(pid);
  out->base_page = pid;
  return 0;
}

int region_alloc(ppap_mem_class_t mem_class, uint32_t size, uint32_t flags,
                 region_t *out) {
  if (!out) return -(int)EINVAL;
  int rc = mem_helper_alloc(mem_class, size, flags, out);
  if (rc != -(int)ENOSYS) return rc;
  return region_alloc_page_backed(size, out);
}

int region_alloc_at(ppap_mem_class_t mem_class, void *base, uint32_t size,
                    uint32_t flags, region_t *out) {
  (void)flags;
  if (!out || !base || size == 0) return -(int)EINVAL;

  if (mem_class == PPAP_MEM_RAM_DATA || mem_class == PPAP_MEM_RAM_STACK ||
      mem_class == PPAP_MEM_RAM_RODATA || mem_class == PPAP_MEM_DEVICE_DMA) {
    uint32_t n_pages = region_page_count(size);
    page_id_t base_id = page_from_ptr(base);
    for (uint32_t i = 0; i < n_pages; i++) {
      if (page_alloc_at(base_id + (page_id_t)i) < 0) {
        for (uint32_t j = 0; j < i; j++) page_free(base_id + (page_id_t)j);
        return -(int)ENOMEM;
      }
    }
    out->base = base;
    out->base_page = base_id;
    return 0;
  }

  return -(int)EINVAL;
}

static void region_free_page_backed(uint32_t size, const region_t *r) {
  uint32_t n_pages = region_page_count(size);
  page_id_t base_id = r->base_page;
  if (base_id == PAGE_ID_INVALID) base_id = page_from_ptr(r->base);
  for (uint32_t i = 0; i < n_pages; i++) page_free(base_id + (page_id_t)i);
}

void region_free(ppap_mem_class_t mem_class, uint32_t size, const region_t *r) {
  if (!r || size == 0) return;
  /* Either a usable base pointer or a valid base_page is required.
   * On i16, callers may set base_page only because r->base would be
   * a truncated pointer that the page-backed paths below ignore. */
  if (!r->base && r->base_page == PAGE_ID_INVALID) return;
  if (mem_helper_free(mem_class, size, r) == 0) return;
  region_free_page_backed(size, r);
}

/* ── Capacity queries ──────────────────────────────────────────────── */

/* Each query first asks the arch helper.  A negative return means
 * the class isn't arch-managed and the answer is the page-pool
 * counter — callers that ask about an unallocatable class get the
 * pool's counter, which is meaningless but harmless. */
uint32_t region_total_bytes(ppap_mem_class_t mem_class) {
  int32_t v = mem_helper_total_bytes(mem_class);
  return v >= 0 ? (uint32_t)v : region_page_pool_total_bytes();
}

uint32_t region_free_bytes(ppap_mem_class_t mem_class) {
  int32_t v = mem_helper_free_bytes(mem_class);
  return v >= 0 ? (uint32_t)v : region_page_pool_free_bytes();
}

uint32_t region_largest_free_bytes(ppap_mem_class_t mem_class) {
  int32_t v = mem_helper_largest_free(mem_class);
  return v >= 0 ? (uint32_t)v : region_page_pool_largest_free_bytes();
}
