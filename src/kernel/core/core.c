/*
 * mod_core.c — Core module struct definition
 *
 * Wires the mod_core function pointer struct to the real
 * implementations in kmem.c, region.c, sched.c, etc.
 *
 * Core functions don't follow the mod_func naming convention
 * (they're kmem_alloc, not core_kmem_alloc) so we wire them manually.
 */

#include "kernel/common/mod/mod_core.h"
#include "kernel/common/sync/kmutex.h"
#include "kernel/core/mm/kmem.h"
#include "kernel/core/mm/page_io.h"
#include "kernel/core/mm/region.h"
#include "kernel/core/proc/sched.h"
#include "kernel/core/subsys/subsys.h"
#include "kernel/core/syscall/syscall.h"

mod_core_t mod_core = {
    .kmem_alloc = kmem_alloc,
    .kmem_free = kmem_free,
    .kmem_free_count = kmem_free_count,
    .kmem_pool_init = kmem_pool_init,
    .kmutex_init = kmutex_init,
    .kmutex_lock = kmutex_lock,
    .kmutex_release_owned = kmutex_release_owned,
    .kmutex_unlock = kmutex_unlock,
    .page_read = page_read,
    .page_write = page_write,
    .page_zero = page_zero,
    .region_alloc = region_alloc,
    .region_free = region_free,
    .region_free_bytes = region_free_bytes,
    .region_total_bytes = region_total_bytes,
    .sched_block_current = sched_block_current,
    .sched_get_ticks = sched_get_ticks,
    .sched_switch = sched_switch,
    .sched_wakeup = sched_wakeup,
    .subsys_read_proc = subsys_read_proc,
    .time_now_sec = time_now_sec,
};
