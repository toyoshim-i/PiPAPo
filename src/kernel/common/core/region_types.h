/*
 * region_types.h — Result shape of class-dispatched mm allocation
 *
 * The mm-layer's typed allocator (mem_region_alloc / mem_region_alloc_at,
 * renamed to region_* in M-3b) returns the minimum information it owns:
 * a base pointer and the page id when page-backed.  Callers that want a
 * fuller image-segment descriptor (vaddr, OWNED flags, mem_class) build
 * one locally at the call site — those fields are proc/exec-layer
 * concerns, not mm.
 *
 * Lives in common/core/ because mod_core.h pulls it across the module
 * boundary (mem_region_alloc / _free appear in the mod_core vtable on
 * pcxt).
 */

#ifndef PPAP_KERNEL_COMMON_CORE_REGION_TYPES_H
#define PPAP_KERNEL_COMMON_CORE_REGION_TYPES_H

#include "kernel/common/core/page_types.h"

typedef struct {
  void *base;          /* dereferenceable on 32-bit; truncated on i16 */
  page_id_t base_page; /* PAGE_ID_INVALID for non-page-backed Xtensa arenas */
} region_t;

#endif /* PPAP_KERNEL_COMMON_CORE_REGION_TYPES_H */
