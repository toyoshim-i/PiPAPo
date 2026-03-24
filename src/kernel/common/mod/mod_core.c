/*
 * mod_core.c — Core module struct definition
 *
 * Wires the mod_core function pointer struct to the real
 * implementations in klog.c and kmem.c.
 *
 * Core functions don't follow the mod_func naming convention
 * (they're klog, not core_klog) so we wire them manually.
 */

#include "mod_core.h"
#include "../../klog.h"
#include "../../mm/kmem.h"

mod_core_t mod_core = {
  .klog = klog,
  .klogf = klogf,
  .kmem_pool_init = kmem_pool_init,
  .kmem_alloc = kmem_alloc,
  .kmem_free = kmem_free,
  .kmem_free_count = kmem_free_count,
};
