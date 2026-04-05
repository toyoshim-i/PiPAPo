/*
 * mod_core.inc — Core module function index (alphabetical order)
 *
 * Single source of truth for function names and their indices.
 * Included by C and assembly to prevent mismatches.
 *
 * Define MOD_CORE_ENTRY(name, idx) before including this file.
 * Entries are sorted alphabetically.  Order MUST match the
 * MOD_FUNC declarations in mod_core.h.
 */

MOD_CORE_ENTRY(blkdev_read,            0)
MOD_CORE_ENTRY(blkdev_write,           1)
MOD_CORE_ENTRY(kmem_alloc,             2)
MOD_CORE_ENTRY(kmem_free,              3)
MOD_CORE_ENTRY(kmem_free_count,        4)
MOD_CORE_ENTRY(kmem_pool_init,         5)
MOD_CORE_ENTRY(mem_region_alloc,       6)
MOD_CORE_ENTRY(mem_region_free,        7)
MOD_CORE_ENTRY(mem_region_free_bytes,  8)
MOD_CORE_ENTRY(mem_region_page_alloc,  9)
MOD_CORE_ENTRY(mem_region_page_free,   10)
MOD_CORE_ENTRY(mem_region_page_linear, 11)
MOD_CORE_ENTRY(mem_region_page_read,   12)
MOD_CORE_ENTRY(mem_region_page_write,  13)
MOD_CORE_ENTRY(mem_region_total_bytes, 14)
MOD_CORE_ENTRY(sched_get_ticks,        15)
MOD_CORE_ENTRY(sched_wakeup,           16)
MOD_CORE_ENTRY(sched_switch,           17)
MOD_CORE_ENTRY(subsys_read_proc,       18)
MOD_CORE_ENTRY(svc_set_restart,        19)

#ifndef MOD_CORE_FUNC_COUNT
#define MOD_CORE_FUNC_COUNT 20
#endif
