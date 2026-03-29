/*
 * klog.c — Atomic kernel logging (SMP-safe)
 *
 * klog() and klogf() disable preemption and acquire SPIN_UART for the
 * entire output sequence, ensuring no interleaving from the other core.
 * Only the preemption timer is disabled — the UART ISR remains active
 * so it can drain the TX ring while klog spins on a full ring.
 *
 * An optional mirror sink (e.g. fbcon for LCD) can be registered via
 * klog_set_mirror().  When set, output goes to both UART and the mirror.
 *
 * Supported format specifiers:
 *   %s — const char * string
 *   %u — uint32_t unsigned decimal
 *   %x — uint32_t as "0xXXXXXXXX"
 *   %% — literal '%'
 */

#include "klog.h"

#include <stdarg.h>

#include "arch/arch.h"
#include "drivers/uart.h"
#include "common/spinlock.h"

/* ── Mirror output sink ──────────────────────────────────────────────────── */

static int (*mirror_putc)(char c, void (*notify)(void));
static void (*mirror_flush)(void);

void klog_set_mirror(int (*putc)(char c, void (*notify)(void)),
                     void (*flush)(void)) {
  mirror_putc = putc;
  mirror_flush = flush;
}

/* ── Lock helpers ────────────────────────────────────────────────────────── *
 *
 * Disable preemption (SysTick TICKINT on ARM, all IRQs on m68k) then
 * acquire the SIO hardware spinlock.  The UART ISR stays active on ARM
 * so it can drain the TX ring asynchronously.
 */

static inline void klog_lock(void) {
  arch_preempt_disable();
  spin_lock(SPIN_UART);
}

static inline void klog_unlock(void) {
  spin_unlock(SPIN_UART);
  arch_preempt_enable();
}

/* ── Internal helpers (UART + mirror) ────────────────────────────────────── */

static void klog_putc(char c) {
  while (!uart_putc(c, NULL))
    ; /* ISR drains the ring — spin until space available */
  if (mirror_putc) mirror_putc(c, NULL);
}

static void klog_puts_raw(const char *s) {
  while (*s) {
    if (*s == '\n') klog_putc('\r');
    klog_putc(*s++);
  }
}

static void klog_print_hex32(uint32_t v) {
  klog_putc('0');
  klog_putc('x');
  for (int i = 7; i >= 0; i--) {
    unsigned nibble = (v >> (i * 4)) & 0xFu;
    klog_putc(nibble < 10u ? (char)('0' + nibble) : (char)('a' + nibble - 10u));
  }
}

static void klog_print_dec(uint32_t v) {
  char buf[10]; /* 2^32 = 4294967296 — at most 10 digits */
  int i = 0;
  if (v == 0u) {
    klog_putc('0');
    return;
  }
  while (v > 0u) {
    buf[i++] = (char)('0' + (v % 10u));
    v /= 10u;
  }
  while (--i >= 0) klog_putc(buf[i]);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void klog(const char *msg) {
  klog_lock();
  klog_puts_raw(msg);
  klog_unlock();
  if (mirror_flush) mirror_flush();
}

void klogf(const char *fmt, ...) {
  klog_lock();

  va_list ap;
  va_start(ap, fmt);

  while (*fmt) {
    if (*fmt == '%') {
      fmt++;
      switch (*fmt) {
        case 's':
          klog_puts_raw(va_arg(ap, const char *));
          break;
        case 'u':
          klog_print_dec((uint32_t)va_arg(ap, unsigned int));
          break;
        case 'x':
          klog_print_hex32((uint32_t)va_arg(ap, unsigned int));
          break;
        case 'l':
          fmt++;
          if (*fmt == 'u') {
            klog_print_dec((uint32_t)va_arg(ap, unsigned long));
          } else if (*fmt == 'x') {
            klog_print_hex32((uint32_t)va_arg(ap, unsigned long));
          } else if (*fmt == '\0') {
            goto done;
          } else {
            klog_putc('%');
            klog_putc('l');
            klog_putc(*fmt);
          }
          break;
        case '%':
          klog_putc('%');
          break;
        case '\0':
          goto done;
        default:
          klog_putc('%');
          klog_putc(*fmt);
          break;
      }
    } else {
      if (*fmt == '\n') klog_putc('\r');
      klog_putc(*fmt);
    }
    fmt++;
  }
done:
  va_end(ap);
  klog_unlock();
  if (mirror_flush) mirror_flush();
}
