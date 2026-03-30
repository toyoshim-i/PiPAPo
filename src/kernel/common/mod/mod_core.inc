/*
 * mod_core_funcs.inc — Core module function index
 *
 * Single source of truth for function names and their indices.
 * Included by C (core_mod_init.c, mod_core.h) and assembly
 * (core_stubs.S, core_entries.S) to prevent mismatches.
 *
 * Define MOD_CORE_ENTRY(name, idx) before including this file.
 *
 * Order MUST match the MOD_FUNC declarations in mod_core.h.
 */

MOD_CORE_ENTRY(klog,                   0)
MOD_CORE_ENTRY(klogf,                  1)
MOD_CORE_ENTRY(kmem_pool_init,         2)
MOD_CORE_ENTRY(kmem_alloc,             3)
MOD_CORE_ENTRY(kmem_free,              4)
MOD_CORE_ENTRY(kmem_free_count,        5)
MOD_CORE_ENTRY(mem_region_alloc,       6)
MOD_CORE_ENTRY(mem_region_free,        7)
MOD_CORE_ENTRY(mem_region_total_bytes, 8)
MOD_CORE_ENTRY(mem_region_free_bytes,  9)
MOD_CORE_ENTRY(mm_page_alloc,        10)
MOD_CORE_ENTRY(mm_page_free,         11)
MOD_CORE_ENTRY(mm_page_read,         12)
MOD_CORE_ENTRY(mm_page_write,        13)
MOD_CORE_ENTRY(sched_wakeup,         14)
MOD_CORE_ENTRY(sched_yield,          15)
MOD_CORE_ENTRY(sched_get_ticks,      16)
MOD_CORE_ENTRY(set_svc_restart,      17)
MOD_CORE_ENTRY(uart_putc,            18)
MOD_CORE_ENTRY(uart_getc,            19)
MOD_CORE_ENTRY(uart_rx_avail,        20)
MOD_CORE_ENTRY(blkdev_read,          21)
MOD_CORE_ENTRY(blkdev_write,         22)

#ifndef MOD_CORE_FUNC_COUNT
#define MOD_CORE_FUNC_COUNT 23
#endif
