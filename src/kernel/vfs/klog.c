/*
 * klog.c — Atomic kernel logging (SMP-safe)
 *
 * klogf() disables preemption and acquires SPIN_UART for the entire
 * output sequence, ensuring no interleaving from the other core.
 * Only the preemption timer is disabled — the UART ISR remains active
 * so it can drain the TX ring while klog spins on a full ring.
 *
 * Output goes through registered logger slots instead of calling the
 * UART driver directly. Targets install a primary logger during early
 * init and may add a secondary mirror logger.
 *
 * Supported format specifiers:
 *   %s — const char * string
 *   %u — uint32_t unsigned decimal
 *   %x — uint32_t as "0xXXXXXXXX"
 *   %% — literal '%'
 */

#include "kernel/vfs/klog.h"

#include <stdarg.h>
#include <stddef.h>

#include "kernel/core/arch.h"
#include "kernel/common/spinlock.h"

/* ── Registered loggers ──────────────────────────────────────────────────── */

static klog_putc_fn logger_putc[KLOG_LOGGER_COUNT];
static void (*logger_flush[KLOG_LOGGER_COUNT])(void);

void klog_set_logger(int id, klog_putc_fn putc, void (*flush)(void)) {
  if ((unsigned)id >= KLOG_LOGGER_COUNT) return;
  logger_putc[id] = putc;
  logger_flush[id] = flush;
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

/* ── Internal helpers (registered loggers) ──────────────────────────────── */

static void klog_putc(char c) {
  for (unsigned i = 0; i < KLOG_LOGGER_COUNT; i++) {
    if (!logger_putc[i]) continue;
    while (!logger_putc[i](c, NULL))
      ; /* logger remains responsible for making forward progress */
  }
}

static void klog_flush_all(void) {
  for (unsigned i = 0; i < KLOG_LOGGER_COUNT; i++) {
    if (logger_flush[i]) logger_flush[i]();
  }
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
  klog_flush_all();
}

/* ── Module aliases ─────────────────────────────────────────────────────────
 *
 * MOD_IMPL(vfs, klogf) expands to .klogf = vfs_klogf — provide that symbol.
 * Linker-level alias avoids the variadic forwarding problem.
 */
void vfs_klogf(const char *, ...) __attribute__((alias("klogf")));

void vfs_klog_set_logger(int id, klog_putc_fn putc, void (*flush)(void)) {
  klog_set_logger(id, putc, flush);
}
