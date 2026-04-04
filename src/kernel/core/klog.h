/*
 * klog.h — Atomic kernel logging (SMP-safe)
 *
 * All kernel diagnostic output should use klog() or klogf() to prevent
 * interleaved characters when both cores are printing concurrently.
 *
 * Simple message:
 *     klog("VFS: romfs mounted at /\n");
 *
 * Formatted message (%s, %u, %x, %%):
 *     klogf("INIT: pid=%u loaded\n", init->pid);
 */

#ifndef PPAP_KERNEL_KLOG_H
#define PPAP_KERNEL_KLOG_H

#include <stdint.h>

typedef int (*klog_putc_fn)(char c, void (*notify)(void));

#define KLOG_LOGGER_PRIMARY 0
#define KLOG_LOGGER_SECONDARY 1
#define KLOG_LOGGER_COUNT 2

/* Atomic single-string log (no format parsing). */
void klog(const char *msg);

/*
 * klogf(fmt, ...) — formatted atomic log.
 *
 *   %s  const char *
 *   %u  uint32_t decimal
 *   %x  uint32_t as "0xXXXXXXXX"
 *   %%  literal '%'
 */
void klogf(const char *fmt, ...);

#ifdef PPAP_DEBUG_LOG
#define PPAP_DEBUG_LOGF(tag, fmt, ...) \
  klogf("[" tag "] " fmt "\n", ##__VA_ARGS__)
#else
#define PPAP_DEBUG_LOGF(tag, fmt, ...) ((void)0)
#endif

/* Register or replace one logger slot. */
void klog_set_logger(int id, klog_putc_fn putc, void (*flush)(void));

#endif /* PPAP_KERNEL_KLOG_H */
