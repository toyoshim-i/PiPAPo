/*
 * page_io.c — Default page-payload access (flat-pointer arches)
 *
 * 32-bit arches with a flat address space access page data through a
 * normal C pointer formed from page_linear(id) + off.  Arches whose
 * address space is segmented (i16) provide a strong override in
 * src/arch/<arch>/kernel/core/page_io.c.
 */

#include "kernel/core/mm/page_io.h"

#include <stdint.h>

#include "kernel/core/mm/page.h"

__attribute__((weak)) void page_read(page_id_t id, uint16_t off, void *buf,
                                     uint16_t len) {
  if (id == PAGE_ID_INVALID || len == 0) return;
  __builtin_memcpy(buf, (const uint8_t *)(uintptr_t)page_linear(id) + off, len);
}

__attribute__((weak)) void page_write(page_id_t id, uint16_t off,
                                      const void *buf, uint16_t len) {
  if (id == PAGE_ID_INVALID || len == 0) return;
  __builtin_memcpy((uint8_t *)(uintptr_t)page_linear(id) + off, buf, len);
}

__attribute__((weak)) void page_zero(page_id_t id, uint16_t off, uint16_t len) {
  if (id == PAGE_ID_INVALID || len == 0) return;
  __builtin_memset((uint8_t *)(uintptr_t)page_linear(id) + off, 0, len);
}
