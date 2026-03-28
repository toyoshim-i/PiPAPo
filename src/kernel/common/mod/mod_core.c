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
#include "../../blkdev/blkdev.h"

/* Cross-module blkdev wrappers — execute dev->read/write in core's CS.
 * NOT static: called by name from core_entries.S on i16. */
int core_blkdev_read(blkdev_t *dev, void *buf,
                     uint32_t sector, uint32_t count) {
  return dev->read(dev, buf, sector, count);
}

int core_blkdev_write(blkdev_t *dev, const void *buf,
                      uint32_t sector, uint32_t count) {
  return dev->write(dev, buf, sector, count);
}

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
  .blkdev_read = core_blkdev_read,
  .blkdev_write = core_blkdev_write,
};
