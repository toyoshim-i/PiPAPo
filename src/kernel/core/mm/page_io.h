/*
 * page_io.h — Page-payload access
 *
 * Pairs with mm/page_io.c (flat-pointer default) and the strong
 * override in arch/i16/.../page_io.c.  Callers that only need to copy
 * bytes in or out of a pool page should include this header rather
 * than the allocator's page.h.
 *
 * VFS code does not include this header directly: cross-module callers
 * reach the same three functions via mod_core.page_read/write/zero.
 */

#ifndef PPAP_KERNEL_CORE_MM_PAGE_IO_H
#define PPAP_KERNEL_CORE_MM_PAGE_IO_H

#include <stdint.h>

#include "kernel/common/core/page_types.h"

/* Read `len` bytes from page `id` at byte offset `off` into `buf`. */
void page_read(page_id_t id, uint16_t off, void *buf, uint16_t len);

/* Write `len` bytes from `buf` to page `id` at byte offset `off`. */
void page_write(page_id_t id, uint16_t off, const void *buf, uint16_t len);

/* Write `len` zero bytes to page `id` starting at byte offset `off`. */
void page_zero(page_id_t id, uint16_t off, uint16_t len);

#endif /* PPAP_KERNEL_CORE_MM_PAGE_IO_H */
