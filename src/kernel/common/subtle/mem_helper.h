/*
 * mem_helper.h — Page-cursor inline helpers (legacy, do not add new uses)
 *
 * These helpers exist only to support existing VFS code that manipulates
 * page_id + offset cursors directly.  New code should use the page-indexed
 * read/write API via mod_core instead:
 *
 *   mod_core.mem_region_page_read(page, off, buf, len);
 *   mod_core.mem_region_page_write(page, off, buf, len);
 *
 * The blkdev API will be refactored to accept (page_id, offset) natively,
 * eliminating the need for mem_region_page_linear() and ptr_ref() in VFS.
 * See: https://github.com/toyoshim-i/PiPAPo/issues/48
 *
 * DO NOT add new callers of these functions.
 */

#ifndef PPAP_KERNEL_COMMON_SUBTLE_MEM_HELPER_H
#define PPAP_KERNEL_COMMON_SUBTLE_MEM_HELPER_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/common/core/page_types.h" /* page_id_t, PAGE_SIZE */

/* Return the largest safe transfer that stays within the current page. */
static inline uint16_t mem_region_page_chunk_len(uint16_t off,
                                                 size_t remaining) {
  size_t chunk = PAGE_SIZE - off;

  if (chunk > remaining) chunk = remaining;
  return (uint16_t)chunk;
}

/* Advance a page_id + offset pair by delta bytes. */
static inline void mem_region_page_advance(page_id_t *page, uint16_t *off,
                                           size_t delta) {
  size_t pos = (size_t)*off + delta;

  *page += (page_id_t)(pos / PAGE_SIZE);
  *off = (uint16_t)(pos % PAGE_SIZE);
}

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
