/*
 * spinlock_ids.h — shared spinlock identifiers
 *
 * IDs are shared across all targets so subsystems refer to the same
 * lock regardless of which spinlock.h overlay is active.  Each
 * implementation interprets the value according to its own conventions
 * (e.g. as a hardware lock index, or simply ignores it on single-core
 * targets).
 */

#ifndef PPAP_KERNEL_COMMON_SPINLOCK_IDS_H
#define PPAP_KERNEL_COMMON_SPINLOCK_IDS_H

enum {
  SPIN_PAGE = 0,   /* page allocator free list */
  SPIN_PROC = 1,   /* process table and PID counter */
  SPIN_VFS = 2,    /* mount table and vnode pool */
  SPIN_FS = 3,     /* filesystem driver sector/block buffers */
  SPIN_UART = 4,   /* UART TX serialisation (klog) */
  SPIN_TXRING = 5, /* UART TX ring buffer and IRQ-mask state */
  SPIN_I2C = 6,    /* I2C controller */
  SPIN_MEM = 7,    /* mem_region arena state */
  SPIN_FBCON = 8,  /* framebuffer console flush serialisation */
  SPIN_FD = 9,     /* global open-file descriptor pool */
  SPIN_PIPE = 10,  /* pipe pool and ring metadata */
  SPIN_DEVFS = 11, /* devfs runtime pseudo-device state */
  SPIN_TIME = 12,  /* wallclock epoch and tick snapshot pairing */
  SPIN_SCHED = 13, /* monotonic tick and CPU accounting counters */
};

#endif /* PPAP_KERNEL_COMMON_SPINLOCK_IDS_H */
