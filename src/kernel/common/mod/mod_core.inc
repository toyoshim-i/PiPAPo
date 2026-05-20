/*
 * mod_core.inc — Core module function index (alphabetical order)
 *
 * Single source of truth for function names and their indices.
 * Included by C and assembly to prevent mismatches.
 *
 * Define MOD_CORE_ENTRY(name, idx) before including this file.
 * Entries are sorted alphabetically.  Order MUST match the
 * MOD_FUNC declarations in mod_core.h, and the PATCH_CORE list in
 * src/target/pcxt/kernel/core/target_pcxt.c.
 */

MOD_CORE_ENTRY(kmem_alloc,             0)
MOD_CORE_ENTRY(kmem_free,              1)
MOD_CORE_ENTRY(kmem_free_count,        2)
MOD_CORE_ENTRY(kmem_pool_init,         3)
MOD_CORE_ENTRY(page_read,              4)
MOD_CORE_ENTRY(page_write,             5)
MOD_CORE_ENTRY(page_zero,              6)
MOD_CORE_ENTRY(region_alloc,           7)
MOD_CORE_ENTRY(region_free,            8)
MOD_CORE_ENTRY(region_free_bytes,      9)
MOD_CORE_ENTRY(region_total_bytes,    10)
MOD_CORE_ENTRY(sched_block_current,   11)
MOD_CORE_ENTRY(sched_get_ticks,       12)
MOD_CORE_ENTRY(sched_switch,          13)
MOD_CORE_ENTRY(sched_wakeup,          14)
MOD_CORE_ENTRY(subsys_read_proc,      15)
MOD_CORE_ENTRY(time_now_sec,          16)

#ifndef MOD_CORE_FUNC_COUNT
#define MOD_CORE_FUNC_COUNT 17
#endif
