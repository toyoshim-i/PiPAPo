/*
 * mem_layout.h — Shared memory-class and process-image descriptors
 *
 * Step XT-2.1 introduces explicit memory classes so ports can describe
 * where executable, immutable, mutable, and stack memory live without
 * relying on raw address-range heuristics.
 */

#ifndef PPAP_KERNEL_MM_MEM_LAYOUT_H
#define PPAP_KERNEL_MM_MEM_LAYOUT_H

#include <stdint.h>

#include "page.h" /* page_id_t, PAGE_ID_INVALID */

typedef enum {
  PPAP_MEM_NONE = 0,
  PPAP_MEM_RAM_TEXT,
  PPAP_MEM_RAM_RODATA,
  PPAP_MEM_RAM_DATA,
  PPAP_MEM_EXT_TEXT,
  PPAP_MEM_EXT_RODATA,
  PPAP_MEM_ROM_TEXT,
  PPAP_MEM_ROM_RODATA,
  PPAP_MEM_RAM_STACK,
  PPAP_MEM_DEVICE_DMA,
} ppap_mem_class_t;

enum {
  PROC_IMAGE_SEG_EXECUTABLE = 1u << 0,
  PROC_IMAGE_SEG_WRITABLE   = 1u << 1,
  PROC_IMAGE_SEG_XIP        = 1u << 2,
  PROC_IMAGE_SEG_OWNED      = 1u << 3,
};

enum {
  PROC_IMAGE_FLAG_NONE              = 0u,
  PROC_IMAGE_FLAG_TEXT_XIP          = 1u << 0,
  PROC_IMAGE_FLAG_LITERAL_COUPLED   = 1u << 1,
  PROC_IMAGE_FLAG_LITERAL_PRELINKED = 1u << 2,
  PROC_IMAGE_FLAG_DATA_COUPLED      = 1u << 3,
  PROC_IMAGE_FLAG_TEXT_STAGED_EXT   = 1u << 4,
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
  proc_image_segment_t seg = proc_image_segment_make(base, size, mem_class,
                                                     flags);
  seg.vaddr = vaddr;
  return seg;
}

#endif /* PPAP_KERNEL_MM_MEM_LAYOUT_H */
