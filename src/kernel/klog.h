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

#ifndef PPAP_KLOG_H
#define PPAP_KLOG_H

#include <stdint.h>

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

/*
 * Register a mirror output sink (e.g. fbcon for LCD).
 * When set, all klog/klogf output is sent to both UART and the mirror.
 * UART always remains the primary debug channel.
 */
void klog_set_mirror(void (*putc)(char c), void (*flush)(void));

#endif /* PPAP_KLOG_H */
