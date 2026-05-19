/*
 * page_ptr.c — Migration shim: page_id_t -> flat void *
 *
 * See page_ptr.h.  This file is omitted from pcxt's source list on
 * purpose, so the symbol is undefined in i16 builds.
 */

#include "kernel/core/mm/page_ptr.h"

#include <stddef.h>

#include "kernel/core/mm/page.h"

void *page_to_ptr(page_id_t id) {
  if (id == PAGE_ID_INVALID) return NULL;
  return (void *)(uintptr_t)page_linear(id);
}
