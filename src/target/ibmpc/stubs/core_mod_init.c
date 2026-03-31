/*
 * core_mod_init.c — VFS-side mod_core struct (i16 segment split)
 *
 * On i16, the mod_core struct lives in the VFS module and points
 * to the caller-side stubs (core_stubs.S). The stubs do the far
 * call to the core segment.
 *
 * Linked into ppap_ibmpc_vfs (VFS binary).
 */

#ifdef __ia16__

#include "mm/kmem.h"  /* kmem_pool_t */
#include "mm/mem_layout.h"  /* proc_image_segment_t, ppap_mem_class_t */
#include "blkdev/blkdev.h"  /* blkdev_t */
#include "common/mod/module.h"

/* Forward-declare the caller-side stubs from core_stubs.S. */
void klog(const char *);
void klogf(const char *, ...);
void kmem_pool_init(kmem_pool_t *, void *, size_t, uint32_t);
void *kmem_alloc(kmem_pool_t *);
void kmem_free(kmem_pool_t *, void *);
uint32_t kmem_free_count(const kmem_pool_t *);
int mem_region_alloc(proc_image_segment_t *, ppap_mem_class_t,
                     uint32_t, uint32_t);
void mem_region_free(const proc_image_segment_t *);
uint32_t mem_region_total_bytes(ppap_mem_class_t);
uint32_t mem_region_free_bytes(ppap_mem_class_t);
uint16_t mem_region_page_alloc(void);
void mem_region_page_free(uint16_t);
void mem_region_page_read(uint16_t, uint16_t, void *, uint16_t);
void mem_region_page_write(uint16_t, uint16_t, const void *, uint16_t);
int core_blkdev_read(blkdev_t *, void *, uint32_t, uint32_t);
int core_blkdev_write(blkdev_t *, const void *, uint32_t, uint32_t);
void sched_wakeup(void *);
void sched_yield(void);
uint32_t sched_get_ticks(void);
void svc_set_restart(void);
int uart_putc(char, void (*)(void));
int uart_getc(void);
int uart_rx_avail(void);

/* MOD_IMPL(core, X) expands to .X = core_X — alias stubs */
#define core_klog                klog
#define core_klogf               klogf
#define core_kmem_pool_init      kmem_pool_init
#define core_kmem_alloc          kmem_alloc
#define core_kmem_free           kmem_free
#define core_kmem_free_count     kmem_free_count
#define core_mem_region_alloc    mem_region_alloc
#define core_mem_region_free     mem_region_free
#define core_mem_region_total_bytes mem_region_total_bytes
#define core_mem_region_free_bytes mem_region_free_bytes
#define core_mem_region_page_alloc  mem_region_page_alloc
#define core_mem_region_page_free   mem_region_page_free
#define core_mem_region_page_read   mem_region_page_read
#define core_mem_region_page_write  mem_region_page_write
#define core_sched_wakeup        sched_wakeup
#define core_sched_yield         sched_yield
#define core_sched_get_ticks     sched_get_ticks
#define core_svc_set_restart     svc_set_restart
#define core_uart_putc           uart_putc
#define core_uart_getc           uart_getc
#define core_uart_rx_avail       uart_rx_avail
#define core_blkdev_read         core_blkdev_read
#define core_blkdev_write        core_blkdev_write

#include "common/mod/mod_core.h"

MOD_DEFINE_BEGIN(core)
#define MOD_CORE_ENTRY(name, idx)  MOD_IMPL(core, name)
#include "common/mod/mod_core.inc"
#undef MOD_CORE_ENTRY
MOD_DEFINE_END()

#endif /* __ia16__ */
