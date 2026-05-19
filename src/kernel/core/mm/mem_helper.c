/*
 * mem_helper.c — Weak default implementations of mem_helper hooks
 *
 * Every hook here is a weak symbol that arch-specific code
 * (src/arch/<arch>/kernel/core/mem_helper.c) can override.  Defaults
 * are chosen so callers in page.c / region.c naturally fall
 * through to the shared page-backed allocator on targets that don't
 * need any special handling (ARM, RISC-V, m68k, ia16).
 */

#include "kernel/core/mm/mem_helper.h"

#include "common/errno.h"

__attribute__((weak)) int mem_helper_init_arenas(void) { return 0; }

__attribute__((weak)) int mem_helper_init_pool(uint32_t *base_out) {
  (void)base_out;
  return 0;
}

__attribute__((weak)) int mem_helper_alloc(ppap_mem_class_t mem_class,
                                           uint32_t size, uint32_t flags,
                                           region_t *out) {
  (void)mem_class;
  (void)size;
  (void)flags;
  (void)out;
  return -(int)ENOSYS;
}

__attribute__((weak)) int mem_helper_free(ppap_mem_class_t mem_class,
                                          uint32_t size, const region_t *r) {
  (void)mem_class;
  (void)size;
  (void)r;
  return -(int)ENOSYS;
}

__attribute__((weak)) int32_t mem_helper_total_bytes(ppap_mem_class_t cls) {
  (void)cls;
  return -1;
}

__attribute__((weak)) int32_t mem_helper_free_bytes(ppap_mem_class_t cls) {
  (void)cls;
  return -1;
}

__attribute__((weak)) int32_t mem_helper_largest_free(ppap_mem_class_t cls) {
  (void)cls;
  return -1;
}

__attribute__((weak)) int mem_helper_pool_aliases_text(uintptr_t lo,
                                                       uintptr_t hi) {
  (void)lo;
  (void)hi;
  return 0;
}

__attribute__((weak)) void mem_helper_log_state(void) {}
