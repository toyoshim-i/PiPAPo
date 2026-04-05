/*
 * mod_core.c — Core module struct definition
 *
 * Wires the mod_core function pointer struct to the real
 * implementations in kmem.c, mem_region.c, sched.c, etc.
 *
 * Core functions don't follow the mod_func naming convention
 * (they're kmem_alloc, not core_kmem_alloc) so we wire them manually.
 */

#include "kernel/common/mod/mod_core.h"
#include "kernel/core/mm/kmem.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/driver/blkdev.h"
#include "kernel/core/proc/sched.h"
#include "kernel/core/subsys/subsys.h"
#include "kernel/core/syscall/syscall.h"

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
  .kmem_pool_init = kmem_pool_init,
  .kmem_alloc = kmem_alloc,
  .kmem_free = kmem_free,
  .kmem_free_count = kmem_free_count,
  .mem_region_alloc = mem_region_alloc,
  .mem_region_free = mem_region_free,
  .mem_region_free_bytes = mem_region_free_bytes,
  .mem_region_page_alloc = mem_region_page_alloc,
  .mem_region_page_free = mem_region_page_free,
  .mem_region_page_read = mem_region_page_read,
  .mem_region_page_write = mem_region_page_write,
  .mem_region_total_bytes = mem_region_total_bytes,
  .sched_wakeup = sched_wakeup,
  .sched_switch = sched_switch,
  .sched_get_ticks = sched_get_ticks,
  .subsys_read_proc = subsys_read_proc,
  .svc_set_restart = svc_set_restart,
  .blkdev_read = core_blkdev_read,
  .blkdev_write = core_blkdev_write,
};
