/*
 * image_alloc.h — proc_image_segment_t allocator helpers
 *
 * Bridge between the mm layer (region_alloc returns a region_t)
 * and the proc/exec layer (process images carry proc_image_segment_t
 * descriptors with vaddr / OWNED flags / class metadata).  The mm
 * layer doesn't know about proc_image_segment_t — these helpers
 * compose the two at the call site.
 *
 * Used by binary loaders (elf_loader, x_loader, com_loader, ...) and
 * by subsystem bridges (human68k_bridge, dos_host) that build process
 * images.  Static inline so each TU resolves directly to
 * region_alloc; no extra link-time symbol.
 */

#ifndef PPAP_KERNEL_CORE_EXEC_IMAGE_ALLOC_H
#define PPAP_KERNEL_CORE_EXEC_IMAGE_ALLOC_H

#include <stdint.h>

#include "kernel/common/core/proc_image.h"
#include "kernel/core/mm/region.h"

static inline int image_segment_alloc(proc_image_segment_t *seg,
                                      ppap_mem_class_t mem_class, uint32_t size,
                                      uint32_t image_flags) {
  region_t r;
  int rc = region_alloc(mem_class, size, 0u, &r);
  if (rc < 0) return rc;
  *seg = proc_image_segment_from_region(mem_class, size, image_flags, r);
  return 0;
}

static inline int image_segment_alloc_at(proc_image_segment_t *seg,
                                         ppap_mem_class_t mem_class, void *base,
                                         uint32_t size, uint32_t image_flags) {
  region_t r;
  int rc = region_alloc_at(mem_class, base, size, 0u, &r);
  if (rc < 0) return rc;
  *seg = proc_image_segment_from_region(mem_class, size, image_flags, r);
  return 0;
}

static inline void image_segment_free(const proc_image_segment_t *seg) {
  if (!seg || seg->size == 0) return;
  region_t r = {seg->base, seg->base_page};
  region_free(seg->mem_class, seg->size, &r);
}

#endif /* PPAP_KERNEL_CORE_EXEC_IMAGE_ALLOC_H */
