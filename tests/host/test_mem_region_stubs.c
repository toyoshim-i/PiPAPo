/*
 * test_mem_region_stubs.c — Minimal mem_region stubs for host-only tests
 */

#include "kernel/mm/mem_region.h"

#include <stdlib.h>

#include "kernel/mm/page.h"

#include "kernel/common/errno.h"

int mem_region_alloc(proc_image_segment_t *seg, ppap_mem_class_t mem_class,
                     uint32_t size, uint32_t flags) {
  void *base;

  if (!seg) return -(int)EINVAL;
  if (size == 0) {
    *seg = (proc_image_segment_t){0};
    return 0;
  }

  base = calloc(1u, size);
  if (!base) return -(int)ENOMEM;

  *seg = proc_image_segment_make(base, size, mem_class, flags);
  return 0;
}

int mem_region_alloc_at(proc_image_segment_t *seg, ppap_mem_class_t mem_class,
                        void *base, uint32_t size, uint32_t flags) {
  (void)seg;
  (void)mem_class;
  (void)base;
  (void)size;
  (void)flags;
  return -(int)ENOSYS;
}

void mem_region_free(const proc_image_segment_t *seg) {
  if (!seg || !seg->base) return;
  free(seg->base);
}

void mem_region_free_tracked_page(void *page) { free(page); }

void mem_region_free_tracked_page_id(page_id_t id) { (void)id; }

uint32_t mem_region_total_bytes(ppap_mem_class_t mem_class) {
  (void)mem_class;
  return 0u;
}

uint32_t mem_region_free_bytes(ppap_mem_class_t mem_class) {
  (void)mem_class;
  return 0u;
}

uint32_t mem_region_largest_free_bytes(ppap_mem_class_t mem_class) {
  (void)mem_class;
  return 0u;
}
