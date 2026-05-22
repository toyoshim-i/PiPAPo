/*
 * core.c — VFS-side mod_core struct (i16 segment split)
 *
 * On i16, the mod_core struct lives in the VFS module and points
 * to the caller-side stubs (core_stubs.S). The stubs do the far
 * call to the core segment.
 *
 * Linked into ppap_pcxt_vfs (VFS binary).
 */

#include "kernel/common/core/mem_class.h"
#include "kernel/common/core/region_types.h"
#include "kernel/common/mod/module.h"
#include "kernel/common/sync/kmutex.h"
#include "kernel/core/mm/kmem.h"

/* Forward-declare the caller-side stubs from core_stubs.S, in mod_core.inc
 * order. */
void *kmem_alloc(kmem_pool_t *);
void kmem_free(kmem_pool_t *, void *);
uint32_t kmem_free_count(const kmem_pool_t *);
void kmem_pool_init(kmem_pool_t *, void *, size_t, uint32_t);
void kmutex_init(kmutex_t *);
void kmutex_lock(kmutex_t *);
void kmutex_release_owned(struct pcb *);
void kmutex_unlock(kmutex_t *);
void page_read(page_id_t, uint16_t, void *, uint16_t);
void page_write(page_id_t, uint16_t, const void *, uint16_t);
void page_zero(page_id_t, uint16_t, uint16_t);
int region_alloc(ppap_mem_class_t, uint32_t, uint32_t, region_t *);
void region_free(ppap_mem_class_t, uint32_t, const region_t *);
uint32_t region_free_bytes(ppap_mem_class_t);
uint32_t region_total_bytes(ppap_mem_class_t);
void sched_block_current(void *);
uint32_t sched_get_ticks(void);
void sched_sleep_current(void *);
void sched_sleep_current_unlock(void *, uint32_t, uint32_t);
void sched_switch(void);
void sched_wakeup(void *);
struct pcb;
int subsys_read_proc(int, struct pcb *, const char *, char *, int);
uint32_t time_now_sec(void);

/* MOD_IMPL(core, X) expands to .X = core_X — alias stubs */
#define core_kmem_alloc          kmem_alloc
#define core_kmem_free           kmem_free
#define core_kmem_free_count     kmem_free_count
#define core_kmem_pool_init      kmem_pool_init
#define core_kmutex_init         kmutex_init
#define core_kmutex_lock         kmutex_lock
#define core_kmutex_release_owned kmutex_release_owned
#define core_kmutex_unlock       kmutex_unlock
#define core_page_read           page_read
#define core_page_write          page_write
#define core_page_zero           page_zero
#define core_region_alloc        region_alloc
#define core_region_free         region_free
#define core_region_free_bytes   region_free_bytes
#define core_region_total_bytes  region_total_bytes
#define core_sched_block_current sched_block_current
#define core_sched_get_ticks     sched_get_ticks
#define core_sched_sleep_current sched_sleep_current
#define core_sched_sleep_current_unlock sched_sleep_current_unlock
#define core_sched_switch        sched_switch
#define core_sched_wakeup        sched_wakeup
#define core_subsys_read_proc    subsys_read_proc
#define core_time_now_sec        time_now_sec

#include "kernel/common/mod/mod_core.h"

MOD_DEFINE_BEGIN(core)
#define MOD_CORE_ENTRY(name, idx)  MOD_IMPL(core, name)
#include "kernel/common/mod/mod_core.inc"
#undef MOD_CORE_ENTRY
MOD_DEFINE_END()
