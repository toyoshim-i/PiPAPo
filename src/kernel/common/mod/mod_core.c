/*
 * mod_core.c — Core module struct definition
 *
 * Wires the mod_core function pointer struct to the real
 * implementations in klog.c, kmem.c, and mem_region.c.
 *
 * Core functions don't follow the mod_func naming convention
 * (they're klog, not core_klog) so we wire them manually.
 */

#include "mod_core.h"
#include "../../klog.h"
#include "../../mm/kmem.h"
#include "../../mm/mem_region.h"

mod_core_t mod_core = {
  .klog = klog,
  .klogf = klogf,
  .kmem_pool_init = kmem_pool_init,
  .kmem_alloc = kmem_alloc,
  .kmem_free = kmem_free,
  .kmem_free_count = kmem_free_count,
  .mem_region_alloc = mem_region_alloc,
  .mem_region_free = mem_region_free,
  .mem_region_total_bytes = mem_region_total_bytes,
  .mem_region_free_bytes = mem_region_free_bytes,
};
