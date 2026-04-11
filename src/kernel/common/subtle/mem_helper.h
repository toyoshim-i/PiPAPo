/*
 * mem_helper.h — Legacy ptr→(page,off) encoder
 *
 * Only mem_region_ptr_ref remains.  It is used by subsys bridge callers
 * (human68k, sos, cpm, h68k_emu) that still take void * from the eCPU
 * memory model.  Phase 3 of the mem_region_wrapup proposal will change
 * those bridge signatures to (page, off) and delete this file.
 *
 * DO NOT add new callers.
 */

#ifndef PPAP_KERNEL_COMMON_SUBTLE_MEM_HELPER_H
#define PPAP_KERNEL_COMMON_SUBTLE_MEM_HELPER_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/common/core/page_types.h" /* page_id_t, PAGE_SIZE */

/* Split a linear pointer into a page_id + byte offset pair. */
static inline int mem_region_ptr_ref(const void *ptr, page_id_t *page,
                                     uint16_t *off) {
  uintptr_t addr = (uintptr_t)ptr;

  if (addr == 0u) {
    *page = PAGE_ID_INVALID;
    *off = 0;
    return -1;
  }
  *page = (page_id_t)(addr / PAGE_SIZE);
  *off = (uint16_t)(addr & (PAGE_SIZE - 1u));
  return (*page == PAGE_ID_INVALID) ? -1 : 0;
}

#endif /* PPAP_KERNEL_COMMON_SUBTLE_MEM_HELPER_H */
