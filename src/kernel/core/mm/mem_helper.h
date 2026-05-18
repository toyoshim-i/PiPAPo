/*
 * mem_helper.h — Arch-overridable hooks for the memory subsystem
 *
 * The shared page.c / mem_region.c implementation handles the common
 * case: a statically-located page pool plus a page-backed allocator
 * for every memory class.  Targets that need a different layout
 * (extra arenas, runtime-allocated pool, physical-aliasing checks)
 * override the hooks declared here.
 *
 * The shared code calls every hook unconditionally.  Default
 * implementations live in mem_helper.c and either no-op or return
 * -ENOSYS so callers naturally fall through to the generic path.
 * Strong overrides live in src/arch/<arch>/kernel/core/mem_helper.c
 * (wired into ARCH_<ARCH>_SOURCES in cmake/kernel.cmake).
 */

#ifndef PPAP_KERNEL_CORE_MM_MEM_HELPER_H
#define PPAP_KERNEL_CORE_MM_MEM_HELPER_H

#include <stdint.h>

#include "kernel/common/core/mem_class.h"
#include "kernel/common/core/page_types.h"
#include "kernel/common/core/region_types.h"
#include "kernel/core/mm/mem_region.h"

/* Reserve arch-specific text / rodata arenas.  Called from
 * mem_region_init() before mm_init's page-pool setup so the page-pool
 * sizing loop can cross-check arena placement.  Returns 0 on success
 * or -errno.  Default: no-op, returns 0. */
int mem_helper_init_arenas(void);

/* Set up the runtime page pool.  Default behaviour leaves the shared
 * page.c values (page_count = PAGE_COUNT_MAX, *base_out unchanged)
 * untouched.  Targets that allocate the pool from a runtime heap
 * override this to fill *base_out with the aligned pool base and
 * write to page_count via the shared global.  Returns 0 on success
 * or -errno on out-of-memory.  *base_out is uint32_t (not uintptr_t)
 * because on ia16 uintptr_t is the 16-bit near-pointer type and the
 * linear pool base can sit above 64 KB. */
int mem_helper_init_pool(uint32_t *base_out);

/* mem_region_alloc dispatch hook.  Return 0 if the request was
 * handled and *out filled, -ENOSYS to fall through to the generic
 * page-backed allocator, or -errno on real failure.  Default: -ENOSYS
 * for every class. */
int mem_helper_alloc(ppap_mem_class_t mem_class, uint32_t size, uint32_t flags,
                     region_t *out);

/* mem_region_free dispatch hook, mirror of mem_helper_alloc.  Class
 * and size identify the original allocation; r names the live region.
 * Return 0 if the region came from an arch arena and was freed,
 * -ENOSYS to fall through to the generic page-pool free path.
 * Default: -ENOSYS. */
int mem_helper_free(ppap_mem_class_t mem_class, uint32_t size,
                    const region_t *r);

/* Per-class capacity queries.  Each returns a negative value if the
 * class isn't arch-managed (so the shared code falls through to its
 * page-pool counters) or the byte count otherwise.  Default: -1. */
int32_t mem_helper_total_bytes(ppap_mem_class_t mem_class);
int32_t mem_helper_free_bytes(ppap_mem_class_t mem_class);
int32_t mem_helper_largest_free(ppap_mem_class_t mem_class);

/* Cross-check: does the candidate page-pool range [lo, hi) physically
 * alias any arch-managed text arena, via an I/D dual mapping that the
 * target's heap allocator doesn't track?  Used by mem_helper_init_pool
 * during its sizing loop and by the kernel test suite as a regression
 * guard.  Default: 0 (no alias). */
int mem_helper_pool_aliases_text(uintptr_t lo, uintptr_t hi);

/* Late-boot diagnostic: re-emit the memory-layout banner that mm_init
 * logged.  Useful on targets whose serial monitor attaches after the
 * early-boot output has scrolled off.  Default: no-op. */
void mem_helper_log_state(void);

#endif /* PPAP_KERNEL_CORE_MM_MEM_HELPER_H */
