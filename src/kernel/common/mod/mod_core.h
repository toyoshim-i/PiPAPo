/*
 * mod_core.h — Core kernel module interface
 *
 * The core module provides common services used by ALL other modules:
 * logging, slab memory allocation, and region-based page allocation.
 *
 * Inline functions (spinlock, string, arch_irq) do NOT need module
 * thunks — they compile into each module's code segment directly.
 * Only functions defined in .c files are exported here.
 *
 * Usage:
 *   #include "common/mod/mod_core.h"
 *   mod_core.klogf("VFS: %s\n", msg);
 *   void *p = mod_core.kmem_alloc(&pool);
 *   mod_core.mem_region_alloc(&seg, PPAP_MEM_RAM_DATA, PAGE_SIZE, flags);
 *
 * Implementation: src/kernel/klog.c, src/kernel/mm/kmem.c,
 *                 src/kernel/mm/mem_region.c
 */

#ifndef PPAP_KERNEL_MOD_MOD_CORE_H
#define PPAP_KERNEL_MOD_MOD_CORE_H

#include <stdint.h>
#include <stddef.h>

/* Forward declarations — full definitions in mm/ and blkdev/ headers */
struct kmem_pool;
typedef struct kmem_pool kmem_pool_t;
struct blkdev;
typedef struct blkdev blkdev_t;

#include "../../mm/mem_layout.h"  /* ppap_mem_class_t, proc_image_segment_t */
#include "../../mm/page.h"       /* page_id_t */

#include "module.h"

MOD_DECLARE_BEGIN(core)

  /* ── Logging ─────────────────────────────────────────────────────────── */

  /*
   * klog — Print a string to the kernel log (UART + optional mirror).
   *
   *   msg  NUL-terminated string to print.
   *
   * No formatting — use klogf for printf-style output.
   */
  MOD_FUNC(core, void, klog, const char *)

  /*
   * klogf — Printf-style kernel log output.
   *
   *   fmt  Format string (subset of printf: %s, %u, %d, %x, %c, %%).
   *   ...  Arguments matching the format specifiers.
   *
   * Note: variadic function pointers work in C but may need special
   * handling for far-call thunks on i16 when segments split.
   */
  MOD_FUNC(core, void, klogf, const char *, ...)

  /* ── Slab allocator ──────────────────────────────────────────────────── */

  /*
   * kmem_pool_init — Initialise a slab allocator pool.
   *
   *   pool      Pool descriptor to initialise.
   *   storage   Backing memory (statically allocated array).
   *   obj_size  Size of each object in bytes.
   *   count     Number of objects that fit in storage.
   */
  MOD_FUNC(core, void, kmem_pool_init, kmem_pool_t *, void *,
                                        size_t, uint32_t)

  /*
   * kmem_alloc — Allocate one object from a slab pool.
   *
   * Returns a pointer to a zeroed object, or NULL if pool is exhausted.
   */
  MOD_FUNC(core, void *, kmem_alloc, kmem_pool_t *)

  /*
   * kmem_free — Return an object to its slab pool.
   *
   *   pool  The pool the object was allocated from.
   *   ptr   Pointer to the object to free.
   */
  MOD_FUNC(core, void, kmem_free, kmem_pool_t *, void *)

  /*
   * kmem_free_count — Return the number of free objects in a pool.
   */
  MOD_FUNC(core, uint32_t, kmem_free_count, const kmem_pool_t *)

  /* ── Region allocator (page-granularity) ──────────────────────────────── */

  /*
   * mem_region_alloc — Allocate a memory region.
   *
   *   seg        Output segment descriptor (base, size, class filled in).
   *   mem_class  Memory class (PPAP_MEM_RAM_DATA, PPAP_MEM_RAM_STACK, ...).
   *   size       Requested size in bytes (rounded up to page boundary).
   *   flags      PROC_IMAGE_SEG_WRITABLE, PROC_IMAGE_SEG_OWNED, etc.
   *
   * Returns 0 on success, -ENOMEM or -EINVAL on failure.
   */
  MOD_FUNC(core, int, mem_region_alloc, proc_image_segment_t *,
                       ppap_mem_class_t, uint32_t, uint32_t)

  /*
   * mem_region_free — Release a previously allocated memory region.
   *
   *   seg  Segment descriptor returned by mem_region_alloc.
   */
  MOD_FUNC(core, void, mem_region_free, const proc_image_segment_t *)

  /*
   * mem_region_total_bytes — Total bytes available for a memory class.
   * mem_region_free_bytes  — Free bytes available for a memory class.
   *
   * Used by procfs for /proc/meminfo reporting.
   */
  MOD_FUNC(core, uint32_t, mem_region_total_bytes, ppap_mem_class_t)
  MOD_FUNC(core, uint32_t, mem_region_free_bytes, ppap_mem_class_t)

  /* ── Page-indexed memory ─────────────────────────────────────────────── */

  /*
   * mm_page_alloc — Allocate one page, return its index.
   *
   * Returns PAGE_ID_INVALID on OOM.
   */
  MOD_FUNC(core, page_id_t, mm_page_alloc, void)

  /*
   * mm_page_free — Free a page by index.
   */
  MOD_FUNC(core, void, mm_page_free, page_id_t)

  /*
   * mm_page_read — Read `len` bytes from page `id` at offset `off` into `buf`.
   */
  MOD_FUNC(core, void, mm_page_read, page_id_t, uint16_t, void *, uint16_t)

  /*
   * mm_page_write — Write `len` bytes from `buf` to page `id` at offset `off`.
   */
  MOD_FUNC(core, void, mm_page_write, page_id_t, uint16_t, const void *,
                                       uint16_t)

  /* ── Scheduler ────────────────────────────────────────────────────────── */

  /*
   * sched_wakeup — Wake all processes blocked on a wait channel.
   *
   *   channel  The wait_channel value to match (e.g. pipe_t *, tty_dev_t *).
   */
  MOD_FUNC(core, void, sched_wakeup, void *)

  /*
   * sched_yield — Voluntarily yield the CPU to another runnable process.
   */
  MOD_FUNC(core, void, sched_yield, void)

  /*
   * sched_get_ticks — Return the current tick count (monotonic).
   */
  MOD_FUNC(core, uint32_t, sched_get_ticks, void)

  /*
   * set_svc_restart — Mark the current syscall for restart after wake.
   */
  MOD_FUNC(core, void, set_svc_restart, void)

  /* ── UART (bootstrap console) ────────────────────────────────────────── */

  /*
   * UART lives in core because klog needs uart_putc to print before
   * VFS is initialized.  In a strict device-driver hierarchy, UART
   * would belong under VFS/devfs as a character device, but the
   * bootstrap dependency makes that impractical: the kernel must be
   * able to emit diagnostics from the earliest boot stage, long
   * before VFS mounts or the tty layer is ready.
   *
   * VFS-side consumers (tty.c, devfs.c) access UART through these
   * mod_core exports rather than calling the driver directly.
   */

  /*
   * uart_putc — Write one character to the UART.
   *
   *   c       Character to send.
   *   notify  Optional callback when TX becomes ready (NULL = no callback).
   *
   * Returns 1 on success, 0 if TX buffer is full.
   */
  MOD_FUNC(core, int, uart_putc, char, void (*)(void))

  /*
   * uart_getc — Read one character from the UART.
   *
   * Returns the character (0-255), or -1 if no data available.
   */
  MOD_FUNC(core, int, uart_getc, void)

  /*
   * uart_rx_avail — Check if UART has received data available.
   *
   * Returns non-zero if at least one byte is available.
   */
  MOD_FUNC(core, int, uart_rx_avail, void)

  /* ── Block device I/O (cross-module safe) ────────────────────────────── */

  /*
   * blkdev_read — Read sectors from a block device.
   *
   * Wrapper around dev->read() that executes in core's CS.
   * Needed because blkdev_t function pointers are near pointers
   * valid only in core's code segment.  VFS modules must call
   * this instead of dev->read() directly.
   *
   *   dev     Block device (in SS=0 shared data).
   *   buf     Destination buffer (in SS=0).
   *   sector  Starting sector number.
   *   count   Number of sectors to read.
   *
   * Returns 0 on success, negative errno on failure.
   */
  MOD_FUNC(core, int, blkdev_read, blkdev_t *, void *,
                                    uint32_t, uint32_t)

  /*
   * blkdev_write — Write sectors to a block device.
   */
  MOD_FUNC(core, int, blkdev_write, blkdev_t *, const void *,
                                     uint32_t, uint32_t)

MOD_DECLARE_END(core)

/* MOD_CORE_FUNC_COUNT is defined in mod_core.inc — the single source
 * of truth shared by both C and assembly stubs. */
#define MOD_CORE_ENTRY(name, idx) /* count only */
#include "mod_core.inc"
#undef MOD_CORE_ENTRY
_Static_assert(sizeof(mod_core_t) == MOD_CORE_FUNC_COUNT * sizeof(void (*)(void)),
               "mod_core_t size mismatch — update MOD_CORE_FUNC_COUNT in "
               "mod_core.inc");

#endif /* PPAP_KERNEL_MOD_MOD_CORE_H */
