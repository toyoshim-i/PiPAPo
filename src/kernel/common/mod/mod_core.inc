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
MOD_CORE_ENTRY(kmutex_init,            4)
MOD_CORE_ENTRY(kmutex_lock,            5)
MOD_CORE_ENTRY(kmutex_release_owned,   6)
MOD_CORE_ENTRY(kmutex_try_lock,        7)
MOD_CORE_ENTRY(kmutex_unlock,          8)
MOD_CORE_ENTRY(page_read,              9)
MOD_CORE_ENTRY(page_write,            10)
MOD_CORE_ENTRY(page_zero,             11)
MOD_CORE_ENTRY(region_alloc,          12)
MOD_CORE_ENTRY(region_free,           13)
MOD_CORE_ENTRY(region_free_bytes,     14)
MOD_CORE_ENTRY(region_total_bytes,    15)
MOD_CORE_ENTRY(sched_block_current,   16)
MOD_CORE_ENTRY(sched_get_ticks,       17)
MOD_CORE_ENTRY(sched_sleep_current,   18)
MOD_CORE_ENTRY(sched_sleep_current_unlock, 19)
MOD_CORE_ENTRY(sched_switch,          20)
MOD_CORE_ENTRY(sched_wakeup,          21)
MOD_CORE_ENTRY(subsys_read_proc,      22)
MOD_CORE_ENTRY(time_now_sec,          23)

#ifndef MOD_CORE_FUNC_COUNT
#define MOD_CORE_FUNC_COUNT 24
#endif
