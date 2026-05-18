/*
 * proc_image.h — Process image segment descriptors
 *
 * proc/exec-layer types describing how a loaded process image is laid
 * out across memory regions.  Carries link-time vaddr, OWNED / XIP /
 * RW flags for the exit-time release path, and the memory class each
 * segment was allocated from.
 */

#ifndef PPAP_KERNEL_COMMON_CORE_PROC_IMAGE_H
#define PPAP_KERNEL_COMMON_CORE_PROC_IMAGE_H

#include <stdint.h>

#include "kernel/common/core/mem_class.h"
#include "kernel/common/core/page_types.h"
#include "kernel/common/core/region_types.h"

enum {
  PROC_IMAGE_SEG_EXECUTABLE = 1u << 0,
  PROC_IMAGE_SEG_WRITABLE = 1u << 1,
  PROC_IMAGE_SEG_XIP = 1u << 2,
  PROC_IMAGE_SEG_OWNED = 1u << 3,
};

enum {
  PROC_IMAGE_FLAG_NONE = 0u,
  PROC_IMAGE_FLAG_TEXT_XIP = 1u << 0,
  PROC_IMAGE_FLAG_LITERAL_COUPLED = 1u << 1,
  PROC_IMAGE_FLAG_LITERAL_PRELINKED = 1u << 2,
  PROC_IMAGE_FLAG_DATA_COUPLED = 1u << 3,
  PROC_IMAGE_FLAG_TEXT_STAGED_EXT = 1u << 4,
};

typedef struct {
  void *base;
  uint32_t size;
  uint32_t vaddr;
  ppap_mem_class_t mem_class;
  uint32_t flags;
  page_id_t base_page; /* PAGE_ID_INVALID for non-page-backed (XIP, arena) */
} proc_image_segment_t;

typedef struct {
  proc_image_segment_t text;
  proc_image_segment_t staged_text;
  proc_image_segment_t staged_rodata;
  proc_image_segment_t literal;
  proc_image_segment_t rodata;
  proc_image_segment_t data;
  proc_image_segment_t stack;
  uintptr_t entry;
  uint32_t flags;
} proc_image_t;

static inline proc_image_segment_t proc_image_segment_make(
    void *base, uint32_t size, ppap_mem_class_t mem_class, uint32_t flags) {
  proc_image_segment_t seg;
  seg.base = base;
  seg.size = size;
  seg.vaddr = 0u;
  seg.mem_class = mem_class;
  seg.flags = flags;
  seg.base_page = PAGE_ID_INVALID;
  return seg;
}

static inline proc_image_segment_t proc_image_segment_make_vaddr(
    void *base, uint32_t size, uint32_t vaddr, ppap_mem_class_t mem_class,
    uint32_t flags) {
  proc_image_segment_t seg =
      proc_image_segment_make(base, size, mem_class, flags);
  seg.vaddr = vaddr;
  return seg;
}

/* Build a process-image segment descriptor around a region_t result.
 * Common shape after a mem_region_alloc / region_alloc call: caller
 * supplies the proc-image-layer fields (mem_class, segment flags),
 * the mm layer supplied (base, base_page) via the region_t. */
static inline proc_image_segment_t proc_image_segment_from_region(
    ppap_mem_class_t mem_class, uint32_t size, uint32_t flags, region_t r) {
  proc_image_segment_t seg =
      proc_image_segment_make(r.base, size, mem_class, flags);
  seg.base_page = r.base_page;
  return seg;
}

#endif /* PPAP_KERNEL_COMMON_CORE_PROC_IMAGE_H */
