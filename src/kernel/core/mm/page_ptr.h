/*
 * page_ptr.h — Migration shim: page_id_t -> flat void *
 *
 * `page_to_ptr(id)` returns the linear base pointer of a page.  It is
 * NOT available on i16 (segmented address space) — pcxt deliberately
 * omits page_ptr.c from its source list, so any pcxt-side caller that
 * forgets its `#if !defined(__ia16__)` guard surfaces as a link error
 * rather than silent corruption.
 *
 * This header / function exists only to keep already-ported 32-bit
 * kernel code compiling while the codebase migrates to page-id-based
 * access.  New code MUST use page_io.h (page_read / page_write /
 * page_zero) or pass page_id_t through the API instead.
 */

#ifndef PPAP_KERNEL_CORE_MM_PAGE_PTR_H
#define PPAP_KERNEL_CORE_MM_PAGE_PTR_H

#include "kernel/common/core/page_types.h"

void *page_to_ptr(page_id_t id);

#endif /* PPAP_KERNEL_CORE_MM_PAGE_PTR_H */
