/*
 * test_mem_region_stubs.c — Minimal mem_region stubs for host-only tests
 */

#include "kernel/core/mm/region.h"

#include <stdlib.h>

#include "common/errno.h"
#include "kernel/core/mm/page.h"

int region_alloc(ppap_mem_class_t mem_class, uint32_t size, uint32_t flags,
                     region_t *out) {
  (void)mem_class;
  (void)flags;

  if (!out) return -(int)EINVAL;
  if (size == 0) {
    out->base = NULL;
    out->base_page = PAGE_ID_INVALID;
    return 0;
  }

  void *base = calloc(1u, size);
  if (!base) return -(int)ENOMEM;

  out->base = base;
  out->base_page = PAGE_ID_INVALID;
  return 0;
}

int region_alloc_at(ppap_mem_class_t mem_class, void *base, uint32_t size,
                        uint32_t flags, region_t *out) {
  (void)mem_class;
  (void)base;
  (void)size;
  (void)flags;
  (void)out;
  return -(int)ENOSYS;
}

void region_free(ppap_mem_class_t mem_class, uint32_t size,
                     const region_t *r) {
  (void)mem_class;
  (void)size;
  if (!r || !r->base) return;
  free(r->base);
}

page_id_t page_alloc(void) { return PAGE_ID_INVALID; }

page_id_t page_alloc_n(uint32_t n) {
  (void)n;
  return PAGE_ID_INVALID;
}

void page_free(page_id_t id) { (void)id; }

uint32_t page_linear(page_id_t id) { return (uint32_t)id; }

page_id_t page_from_ptr(void *ptr) {
  (void)ptr;
  return PAGE_ID_INVALID;
}

void *page_to_ptr(page_id_t id) {
  (void)id;
  return NULL;
}

void page_read(page_id_t id, uint16_t off, void *buf, uint16_t len) {
  (void)id;
  (void)off;
  (void)buf;
  (void)len;
}

void page_write(page_id_t id, uint16_t off, const void *buf, uint16_t len) {
  (void)id;
  (void)off;
  (void)buf;
  (void)len;
}

uint32_t region_total_bytes(ppap_mem_class_t mem_class) {
  (void)mem_class;
  return 0u;
}

uint32_t region_free_bytes(ppap_mem_class_t mem_class) {
  (void)mem_class;
  return 0u;
}

uint32_t region_largest_free_bytes(ppap_mem_class_t mem_class) {
  (void)mem_class;
  return 0u;
}
