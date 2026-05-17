/*
 * mem_region.c — Memory-region allocation helpers
 *
 * Generic implementation.  All target-specific behaviour (extra
 * arenas, dual-mapping checks, alternative allocators) lives behind
 * the mem_helper hooks in src/arch/<arch>/kernel/core/mem_helper.c,
 * so this file has zero architecture conditionals.
 */

#include "kernel/core/mm/mem_region.h"

#include "common/errno.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/common/spinlock.h"
#include "kernel/core/mm/mem_helper.h"
#include "kernel/core/mm/page.h"

static uint32_t mem_region_page_count(uint32_t size) {
  return (size + PAGE_SIZE - 1u) / PAGE_SIZE;
}

static uint32_t mem_region_page_pool_total_bytes(void) {
  return page_count * PAGE_SIZE;
}

static uint32_t mem_region_page_pool_free_bytes(void) {
  return page_free_count() * PAGE_SIZE;
}

static uint32_t mem_region_page_pool_largest_free_bytes(void) {
  return page_max_contiguous() * PAGE_SIZE;
}

int mem_region_init(void) { return mem_helper_init_arenas(); }

static int mem_region_alloc_page_backed(proc_image_segment_t *seg,
                                        ppap_mem_class_t mem_class,
                                        uint32_t size, uint32_t flags) {
  if (size == 0) {
    *seg = (proc_image_segment_t){0};
    return 0;
  }

  /* Allocate via the page-indexed API so we get the page_id directly.
   * On i16 the void * returned by page_alloc() is the linear address
   * truncated to 16 bits, which makes mm_ptr_to_page() round-trip back
   * to the wrong page id for any page above the first 64 KB.  Going
   * through mm_page_alloc + mm_page_linear keeps base_page correct on
   * every architecture and lets i16 callers safely reach pages outside
   * the 64 KB DS=0 window via mem_region_page_read/write. */
  uint32_t n_pages = mem_region_page_count(size);
  page_id_t pid =
      (n_pages == 1) ? mm_page_alloc() : mm_page_alloc_contiguous(n_pages);
  if (pid == PAGE_ID_INVALID) return -(int)ENOMEM;

  void *base = (void *)(uintptr_t)mm_page_linear(pid);
  *seg = proc_image_segment_make(base, size, mem_class, flags);
  seg->base_page = pid;
  return 0;
}

int mem_region_alloc(proc_image_segment_t *seg, ppap_mem_class_t mem_class,
                     uint32_t size, uint32_t flags) {
  if (!seg) return -(int)EINVAL;
  int rc = mem_helper_alloc(mem_class, size, flags, seg);
  if (rc != -(int)ENOSYS) return rc;
  return mem_region_alloc_page_backed(seg, mem_class, size, flags);
}

int mem_region_alloc_at(proc_image_segment_t *seg, ppap_mem_class_t mem_class,
                        void *base, uint32_t size, uint32_t flags) {
  if (!seg || !base || size == 0) return -(int)EINVAL;

  if (mem_class == PPAP_MEM_RAM_DATA || mem_class == PPAP_MEM_RAM_STACK ||
      mem_class == PPAP_MEM_RAM_RODATA || mem_class == PPAP_MEM_DEVICE_DMA) {
    uint32_t n_pages = mem_region_page_count(size);
    for (uint32_t i = 0; i < n_pages; i++) {
      void *addr = (uint8_t *)base + i * PAGE_SIZE;
      if (!page_alloc_at(addr)) {
        for (uint32_t j = 0; j < i; j++)
          page_free((uint8_t *)base + j * PAGE_SIZE);
        return -(int)ENOMEM;
      }
    }
    *seg = proc_image_segment_make(base, size, mem_class, flags);
    seg->base_page = mm_ptr_to_page(base);
    return 0;
  }

  return -(int)EINVAL;
}

static void mem_region_free_page_backed(const proc_image_segment_t *seg) {
  uint32_t n_pages = mem_region_page_count(seg->size);
  page_id_t base_id = seg->base_page;
  if (base_id != PAGE_ID_INVALID) {
    for (uint32_t i = 0; i < n_pages; i++) mm_page_free(base_id + (page_id_t)i);
  } else {
    for (uint32_t i = 0; i < n_pages; i++)
      page_free((uint8_t *)seg->base + i * PAGE_SIZE);
  }
}

void mem_region_free(const proc_image_segment_t *seg) {
  if (!seg || seg->size == 0) return;
  /* Either a usable base pointer or a valid base_page is required.
   * On i16, callers may set base_page only because seg->base would be
   * a truncated pointer that the page-backed paths below ignore. */
  if (!seg->base && seg->base_page == PAGE_ID_INVALID) return;
  if (mem_helper_free(seg) == 0) return;
  mem_region_free_page_backed(seg);
}

void mem_region_free_tracked_page_id(page_id_t id) {
  if (id == PAGE_ID_INVALID) return;
  mm_page_free(id);
}

/* ── Page-index wrappers ────────────────────────────────────────────── */

page_id_t mem_region_page_alloc(void) { return mm_page_alloc(); }

page_id_t mem_region_page_alloc_contiguous(uint32_t n_pages) {
  return mm_page_alloc_contiguous(n_pages);
}

page_id_t mem_region_page_alloc_largest_contiguous(uint32_t min_pages,
                                                   uint32_t max_pages,
                                                   uint32_t *got_pages) {
  return mm_page_alloc_largest_contiguous(min_pages, max_pages, got_pages);
}

void mem_region_page_free(page_id_t id) { mm_page_free(id); }

uint32_t mem_region_page_linear(page_id_t id) { return mm_page_linear(id); }

page_id_t mem_region_ptr_to_page(void *ptr) { return mm_ptr_to_page(ptr); }

#if !defined(__ia16__)
void *mem_region_page_to_ptr(page_id_t id) { return mm_page_to_ptr(id); }
#endif

void mem_region_page_read(page_id_t id, uint16_t off, void *buf, uint16_t len) {
  mm_page_read(id, off, buf, len);
}

void mem_region_page_write(page_id_t id, uint16_t off, const void *buf,
                           uint16_t len) {
  mm_page_write(id, off, buf, len);
}

void mem_region_page_zero(page_id_t id, uint16_t off, uint16_t len) {
  mm_page_zero(id, off, len);
}

/* ── Capacity queries ──────────────────────────────────────────────── */

/* Each query first asks the arch helper.  A negative return means
 * the class isn't arch-managed and the answer is the page-pool
 * counter — callers that ask about an unallocatable class get the
 * pool's counter, which is meaningless but harmless. */
uint32_t mem_region_total_bytes(ppap_mem_class_t mem_class) {
  int32_t v = mem_helper_total_bytes(mem_class);
  return v >= 0 ? (uint32_t)v : mem_region_page_pool_total_bytes();
}

uint32_t mem_region_free_bytes(ppap_mem_class_t mem_class) {
  int32_t v = mem_helper_free_bytes(mem_class);
  return v >= 0 ? (uint32_t)v : mem_region_page_pool_free_bytes();
}

uint32_t mem_region_largest_free_bytes(ppap_mem_class_t mem_class) {
  int32_t v = mem_helper_largest_free(mem_class);
  return v >= 0 ? (uint32_t)v : mem_region_page_pool_largest_free_bytes();
}
