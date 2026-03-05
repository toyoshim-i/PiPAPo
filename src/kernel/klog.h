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

#include "spinlock.h"
#include "drivers/uart.h"

/* Atomic single-string log (no format parsing). */
static inline void klog(const char *msg)
{
    uint32_t s = spin_lock_irqsave(SPIN_UART);
    uart_puts(msg);
    spin_unlock_irqrestore(SPIN_UART, s);
}

/*
 * klogf(fmt, ...) — formatted atomic log.
 *
 *   %s  const char *
 *   %u  uint32_t decimal
 *   %x  uint32_t as "0xXXXXXXXX"
 *   %%  literal '%'
 */
void klogf(const char *fmt, ...);

#endif /* PPAP_KLOG_H */
