/*
 * mem_region.c — Memory-region allocation helpers
 */

#include "mem_region.h"

#include "kernel/common/errno.h"
#include "page.h"

static uint32_t mem_region_page_count(uint32_t size) {
  return (size + PAGE_SIZE - 1u) / PAGE_SIZE;
}

static int mem_region_alloc_page_backed(proc_image_segment_t *seg,
                                        ppap_mem_class_t mem_class,
                                        uint32_t size, uint32_t flags) {
  uint32_t n_pages;
  void *base;

  if (size == 0) {
    *seg = (proc_image_segment_t){0};
    return 0;
  }

  n_pages = mem_region_page_count(size);
  if (n_pages == 1) {
    base = page_alloc();
  } else {
    base = page_alloc_contiguous(n_pages);
  }
  if (!base) return -(int)ENOMEM;

  *seg = proc_image_segment_make(base, size, mem_class, flags);
  return 0;
}

int mem_region_alloc(proc_image_segment_t *seg, ppap_mem_class_t mem_class,
                     uint32_t size, uint32_t flags) {
  if (!seg) return -(int)EINVAL;

  switch (mem_class) {
    case PPAP_MEM_RAM_RODATA:
    case PPAP_MEM_RAM_DATA:
    case PPAP_MEM_RAM_STACK:
    case PPAP_MEM_DEVICE_DMA:
      return mem_region_alloc_page_backed(seg, mem_class, size, flags);

#if defined(__xtensa__)
    case PPAP_MEM_RAM_TEXT: {
      extern void *heap_caps_malloc(unsigned int size, uint32_t caps);
      void *base = heap_caps_malloc(size, (1u << 0));
      if (!base) return -(int)ENOMEM;
      *seg = proc_image_segment_make(base, size, mem_class, flags);
      return 0;
    }
#else
    case PPAP_MEM_RAM_TEXT:
      return mem_region_alloc_page_backed(seg, mem_class, size, flags);
#endif

    case PPAP_MEM_NONE:
    case PPAP_MEM_ROM_TEXT:
    case PPAP_MEM_ROM_RODATA:
    default:
      return -(int)EINVAL;
  }
}

void mem_region_free(const proc_image_segment_t *seg) {
  uint32_t n_pages;

  if (!seg || !seg->base || seg->size == 0) return;

  switch (seg->mem_class) {
    case PPAP_MEM_RAM_RODATA:
    case PPAP_MEM_RAM_DATA:
    case PPAP_MEM_RAM_STACK:
    case PPAP_MEM_DEVICE_DMA:
      n_pages = mem_region_page_count(seg->size);
      for (uint32_t i = 0; i < n_pages; i++)
        page_free((uint8_t *)seg->base + i * PAGE_SIZE);
      return;

#if defined(__xtensa__)
    case PPAP_MEM_RAM_TEXT: {
      extern void heap_caps_free(void *ptr);
      heap_caps_free(seg->base);
      return;
    }
#else
    case PPAP_MEM_RAM_TEXT:
      n_pages = mem_region_page_count(seg->size);
      for (uint32_t i = 0; i < n_pages; i++)
        page_free((uint8_t *)seg->base + i * PAGE_SIZE);
      return;
#endif

    case PPAP_MEM_NONE:
    case PPAP_MEM_ROM_TEXT:
    case PPAP_MEM_ROM_RODATA:
    default:
      return;
  }
}
