/*
 * mem_region_kbuf.h — Kernel-buffer → (page_id, off) conversion helper
 *
 * R2 escape hatch (see docs/proposals/mem_region_wrapup.md).  Strictly
 * scoped: this helper exists so that file-system drivers can pass
 * **kernel-owned** metadata buffers (e.g. ufs_buf, sector_buf, fstab
 * scratch) to the page-indexed blkdev API without re-introducing a
 * general-purpose `void *` → page_id encoder.
 *
 * Allowed callers:
 *   - File-system drivers in src/kernel/vfs/ for metadata I/O backed
 *     by static/global kernel buffers.
 *   - Boot-time parsers under R4 (small kbuf, single function).
 *
 * Forbidden callers:
 *   - User-space pointer translation (use user_to_page in the syscall
 *     dispatcher instead).
 *   - Anything that already holds (page, off) from upstream — pass it
 *     through directly.
 *
 * The conversion is intentionally inline arithmetic.  It does NOT
 * validate that `kbuf` lies inside the page pool; the caller is
 * responsible for only passing kernel-owned buffer pointers.  On i16
 * the result page_id may live outside the page pool (DS=0 segment is
 * not part of the user page pool), and that is fine — mm_page_read/
 * write round-trip the page_id back to the original linear address
 * via segment:offset math.
 */

#ifndef PPAP_KERNEL_COMMON_MEM_REGION_KBUF_H
#define PPAP_KERNEL_COMMON_MEM_REGION_KBUF_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/common/core/page_types.h"

static inline void mem_region_kbuf_to_page(const void *kbuf, page_id_t *page,
                                           uint16_t *off) {
  uintptr_t addr = (uintptr_t)kbuf;

  *page = (page_id_t)(addr / PAGE_SIZE);
  *off = (uint16_t)(addr & (PAGE_SIZE - 1u));
}

#endif /* PPAP_KERNEL_COMMON_MEM_REGION_KBUF_H */
